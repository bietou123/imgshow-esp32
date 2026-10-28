// ============================================================================
// net.h  —  非阻塞网络层
//
// 这是本次改动最大的地方。老代码在按钮回调里直接同步调 HTTP:
//   handleMenu() -> doFetchStock() -> 两次 HTTPS GET，各 8s 超时
//   handleMenu() -> preloadRandomData() -> 天气 8s + 股票 5s
// 只要网络一慢，整个 UI 冻结最长 20 秒，按钮完全没反应，看着像死机。
//
// 现在: 所有 HTTP 都在 core0 的后台任务里跑，主循环只看 revision 计数器决定
// 要不要重画。UI 永远不阻塞。
//
// 顺带修的:
//  * 股票/K线改走 HTTP —— qt.gtimg.cn 和 sina 都支持明文，老代码非要上 HTTPS，
//    每次白花 ~45KB 堆做 TLS 握手，还慢好几秒。TLS 现在只留给足球那个 API。
//  * 天气改用 wttr.in 的一行格式 (format=%t|%h|%C|%w)，返回 ~30 字节。
//    老代码拉的是 format=j1，30KB JSON —— getString() 先吃 30KB 堆，
//    ArduinoJson 再吃 30KB，这是最容易 OOM 的一条路。
//  * K线解析老代码找 "\"close\":"，但新浪返回的是不带引号的 close:"3593.660"，
//    永远匹配不上 —— 所以 K 线图一直是 "No chart data"。改成找 close:" 。
//  * WiFi 断线现在会自动指数退避重连；NTP 对不上时会一直重试并在 UI 上说明。
//  * 老代码连不上 WiFi 就强制进 AP 配网 —— 路由器重启一次就得重新配。
//    现在只有"从来没配过"才进 AP，配过的就一直后台重连。
// ============================================================================
#pragma once
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include "store.h"

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #include "esp_eap_client.h"     // core 3.x 把 esp_wpa2.h 改名了
#else
  #include "esp_wpa2.h"
#endif

enum NetState : uint8_t { NS_BOOT, NS_CONNECTING, NS_ONLINE, NS_RETRY, NS_AP };
static NetState g_ns = NS_BOOT;

static bool     g_ntpOK      = false;
static uint32_t g_wxRev = 0, g_stkRev = 0, g_matchRev = 0;   // 数据版本号
static volatile bool g_netBusy = false;
static char     g_netMsg[26]  = "";

static SemaphoreHandle_t g_dataMtx = nullptr;
static QueueHandle_t     g_jobQ    = nullptr;

enum NetJob : uint8_t { JOB_WEATHER = 1, JOB_STOCK, JOB_FOOTBALL };

static const char* STOCK_CODES[] = CFG_STOCK_LIST;
static const char* STOCK_NAMES[] = CFG_STOCK_NAMES;
static const uint8_t STOCK_COUNT = sizeof(STOCK_CODES) / sizeof(STOCK_CODES[0]);

inline void netLock()   { if (g_dataMtx) xSemaphoreTake(g_dataMtx, portMAX_DELAY); }
inline void netUnlock() { if (g_dataMtx) xSemaphoreGive(g_dataMtx); }
inline bool netOnline() { return g_ns == NS_ONLINE && WiFi.isConnected(); }
inline bool netBusy()   { return g_netBusy; }

// 请求一次后台抓取。队列满了直接丢，不阻塞。
inline void netRequest(NetJob j) {
  if (g_jobQ) xQueueSend(g_jobQ, &j, 0);
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
// 只留可打印 ASCII。wttr.in 会返回 °C 和 → 这类 UTF-8，Adafruit 经典字体画不出来
static void asciiOnly(const char* src, char* dst, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < cap; i++) {
    uint8_t c = (uint8_t)src[i];
    if (c >= 32 && c < 127) dst[o++] = (char)c;
  }
  dst[o] = 0;
}

static void urlEncodeInto(const char* s, char* out, size_t cap) {
  static const char* hex = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; s[i] && o + 4 < cap; i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
    else { out[o++] = '%'; out[o++] = hex[(c >> 4) & 0xF]; out[o++] = hex[c & 0xF]; }
  }
  out[o] = 0;
}

// 把 HTTP 响应读进定长缓冲，超出直接截断 —— 绝不动态分配大 String
static int httpGetToBuf(const char* url, char* buf, size_t cap, bool tls = false,
                        const char* extraHdr = nullptr, const char* extraVal = nullptr) {
  buf[0] = 0;
  WiFiClient*       plain = nullptr;
  WiFiClientSecure* sec   = nullptr;
  HTTPClient http;
  bool begun = false;

  if (tls) {
    sec = new WiFiClientSecure();
    if (!sec) return -100;
    sec->setInsecure();              // 128x128 摆件不做证书校验，见 README 安全说明
    sec->setTimeout(9);
    begun = http.begin(*sec, url);
  } else {
    plain = new WiFiClient();
    if (!plain) return -100;
    begun = http.begin(*plain, url);
  }
  if (!begun) { delete plain; delete sec; return -101; }

  http.setTimeout(9000);
  http.setConnectTimeout(6000);
  http.setReuse(false);
  http.setUserAgent("curl/8.0");
  http.addHeader("Accept-Encoding", "identity");   // 别给我 gzip
  if (extraHdr) http.addHeader(extraHdr, extraVal);

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    WiFiClient* s = http.getStreamPtr();
    size_t o = 0;
    uint32_t t0 = millis();
    while (http.connected() && o + 1 < cap && millis() - t0 < 9000) {
      size_t avail = s->available();
      if (avail) {
        size_t n = s->readBytes(buf + o, min(avail, cap - 1 - o));
        o += n; t0 = millis();
        if (o + 1 >= cap) break;
      } else {
        int len = http.getSize();
        if (len >= 0 && o >= (size_t)len) break;
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    buf[o] = 0;
  }
  http.end();
  delete plain; delete sec;
  return code;
}

// ---------------------------------------------------------------------------
// 天气
// ---------------------------------------------------------------------------
static void fetchWeather() {
  static char buf[1400];
  WeatherData w;
  strncpy(w.city, g_set.city, sizeof(w.city) - 1);

#if CFG_WEATHER_PROVIDER == 0
  // wttr.in 一行格式，返回类似 "+31C|55%|Clear|->11km/h"
  char city[64]; urlEncodeInto(g_set.city, city, sizeof(city));
  char url[160];
  snprintf(url, sizeof(url),
           "http://wttr.in/%s?format=%%25t%%7C%%25h%%7C%%25C%%7C%%25w", city);
  int code = httpGetToBuf(url, buf, sizeof(buf), false);
  if (code == 200 && buf[0]) {
    char clean[80]; asciiOnly(buf, clean, sizeof(clean));
    // 按 | 切 4 段
    char* f[4] = {nullptr, nullptr, nullptr, nullptr};
    uint8_t n = 0; f[n++] = clean;
    for (char* p = clean; *p && n < 4; p++) if (*p == '|') { *p = 0; f[n++] = p + 1; }
    if (f[0]) w.tempC   = atoi(f[0]);
    if (f[1]) w.humid   = atoi(f[1]);
    if (f[2]) { while (*f[2] == ' ') f[2]++; strncpy(w.desc, f[2], sizeof(w.desc) - 1); }
    if (f[3]) { const char* d = f[3]; while (*d && !isdigit((unsigned char)*d)) d++; w.windKmh = atoi(d); }
    w.valid = (w.tempC > -100);
  }

#elif CFG_WEATHER_PROVIDER == 1
  // 和风天气 v7/weather/now，JSON 很小
  char url[220];
  snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s&key=%s",
           CFG_QWEATHER_HOST, CFG_QWEATHER_LOCID, CFG_QWEATHER_KEY);
  int code = httpGetToBuf(url, buf, sizeof(buf), true);
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, buf)) {
      JsonObject n = doc["now"];
      if (!n.isNull()) {
        w.tempC   = n["temp"].as<int>();
        w.humid   = n["humidity"].as<int>();
        w.windKmh = n["windSpeed"].as<int>();
        asciiOnly(n["text"] | "--", w.desc, sizeof(w.desc));
        w.valid = true;
      }
    }
  }

#else
  // OpenWeatherMap
  char city[64]; urlEncodeInto(g_set.city, city, sizeof(city));
  char url[230];
  snprintf(url, sizeof(url),
           "http://api.openweathermap.org/data/2.5/weather?q=%s&units=metric&appid=%s",
           city, CFG_OWM_KEY);
  int code = httpGetToBuf(url, buf, sizeof(buf), false);
  if (code == 200) {
    JsonDocument filter;                      // 只解析需要的字段，省一大半堆
    filter["main"]["temp"] = true;
    filter["main"]["humidity"] = true;
    filter["wind"]["speed"] = true;
    filter["weather"][0]["main"] = true;
    filter["name"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, buf, DeserializationOption::Filter(filter))) {
      w.tempC   = (int)lroundf(doc["main"]["temp"] | 0.0f);
      w.humid   = doc["main"]["humidity"] | -1;
      w.windKmh = (int)lroundf((doc["wind"]["speed"] | 0.0f) * 3.6f);
      asciiOnly(doc["weather"][0]["main"] | "--", w.desc, sizeof(w.desc));
      asciiOnly(doc["name"] | g_set.city, w.city, sizeof(w.city));
      w.valid = true;
    }
  }
#endif

  w.stamp = millis();
  netLock();
  if (w.valid || !g_wx.valid) g_wx = w;   // 抓失败就保留上次的好数据
  else g_wx.stamp = w.stamp;
  netUnlock();
  g_wxRev++;
}

// ---------------------------------------------------------------------------
// 股票行情 + K线 (全 HTTP，不用 TLS)
// ---------------------------------------------------------------------------
static void fetchStock() {
  static char buf[2600];
  uint8_t idx = g_set.stockIdx; if (idx >= STOCK_COUNT) idx = 0;
  StockData s;
  strncpy(s.name, STOCK_NAMES[idx], sizeof(s.name) - 1);

  char url[120];
  snprintf(url, sizeof(url), "http://qt.gtimg.cn/q=%s", STOCK_CODES[idx]);
  if (httpGetToBuf(url, buf, 1400, false) == 200) {
    char* q = strchr(buf, '"');
    if (q) {
      // 按 ~ 切字段: [3]=现价 [31]=涨跌额 [32]=涨跌幅%
      char* p = q + 1; uint8_t fi = 0;
      char* fields[36] = {nullptr};
      fields[fi++] = p;
      for (; *p && fi < 36; p++) if (*p == '~') { *p = 0; fields[fi++] = p + 1; }
      if (fi >= 33) {
        asciiOnly(fields[3], s.price, sizeof(s.price));
        asciiOnly(fields[32], s.pct, sizeof(s.pct));
        s.up = (atof(fields[31]) >= 0);
        s.valid = (s.price[0] != 0);
      }
    }
  }

  // K线: 新浪返回不带引号的 key —— close:"3593.660"
  snprintf(url, sizeof(url),
    "http://money.finance.sina.com.cn/quotes_service/api/json_v2.php/"
    "CN_MarketData.getKLineData?symbol=%s&scale=240&ma=no&datalen=24", STOCK_CODES[idx]);
  if (httpGetToBuf(url, buf, sizeof(buf), false) == 200) {
    const char* p = buf;
    while (s.klineN < 24) {
      const char* c = strstr(p, "close:\"");
      if (!c) { c = strstr(p, "\"close\":\""); if (!c) break; c += 9; }
      else c += 7;
      s.kline[s.klineN++] = atof(c);
      p = c;
    }
  }

  s.stamp = millis();
  netLock();
  if (s.valid || !g_stk.valid) g_stk = s;
  else { g_stk.stamp = s.stamp; if (s.klineN) { memcpy(g_stk.kline, s.kline, sizeof(s.kline)); g_stk.klineN = s.klineN; } }
  netUnlock();
  g_stkRev++;
}

// ---------------------------------------------------------------------------
// 足球赛程 (唯一还需要 TLS 的接口)
// 老代码 http.getString() 拉 ~60KB JSON 再喂给 JsonDocument —— 双份大堆，
// 在 WiFi+TLS 已经吃掉一半 RAM 的情况下基本必挂。改成定长缓冲 + Filter。
// ---------------------------------------------------------------------------
static void parseMatches(const char* json, bool past, uint8_t* n) {
  JsonDocument filter;
  JsonObject fe = filter["events"].add<JsonObject>();
  fe["strHomeTeam"] = true; fe["strAwayTeam"] = true;
  fe["intHomeScore"] = true; fe["intAwayScore"] = true;
  fe["dateEvent"] = true; fe["strTime"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter))) return;
  JsonArray arr = doc["events"].as<JsonArray>();
  if (arr.isNull()) return;

  int total = arr.size();
  int start = 0;
  if (past && total > 4) start = total - 4;      // 历史赛程只要最近 4 场
  for (int i = start; i < total && *n < MATCH_MAX; i++) {
    JsonObject m = arr[i];
    MatchData& d = g_match[*n];
    asciiOnly(m["strHomeTeam"] | "?", d.home, sizeof(d.home));
    asciiOnly(m["strAwayTeam"] | "?", d.away, sizeof(d.away));
    d.hs = m["intHomeScore"].isNull() ? -1 : m["intHomeScore"].as<int>();
    d.as = m["intAwayScore"].isNull() ? -1 : m["intAwayScore"].as<int>();
    const char* date = m["dateEvent"] | "";
    const char* tm   = m["strTime"]   | "";
    if (strlen(date) >= 10) snprintf(d.when, sizeof(d.when), "%.5s %.5s", date + 5, past ? "FT" : tm);
    else snprintf(d.when, sizeof(d.when), "%s", past ? "FT" : "TBD");
    (*n)++;
  }
}

static void fetchFootball() {
  char* buf = (char*)malloc(14000);   // 临时借 14KB，用完立刻还
  if (!buf) return;
  char url[160];
  uint8_t n = 0;

  snprintf(url, sizeof(url),
    "https://www.thesportsdb.com/api/v1/json/%s/eventspastleague.php?id=%s",
    CFG_SPORTSDB_KEY, CFG_SPORTSDB_LEAGUE);
  if (httpGetToBuf(url, buf, 14000, true) == 200) parseMatches(buf, true, &n);

  snprintf(url, sizeof(url),
    "https://www.thesportsdb.com/api/v1/json/%s/eventsnextleague.php?id=%s",
    CFG_SPORTSDB_KEY, CFG_SPORTSDB_LEAGUE);
  if (httpGetToBuf(url, buf, 14000, true) == 200) parseMatches(buf, false, &n);

  free(buf);
  netLock(); g_matchN = n; netUnlock();
  g_matchRev++;
}

// ---------------------------------------------------------------------------
// 后台任务
// ---------------------------------------------------------------------------
static void netTask(void*) {
  NetJob job;
  for (;;) {
    if (xQueueReceive(g_jobQ, &job, portMAX_DELAY) == pdTRUE) {
      // 没网就直接失败，别在这儿干等
      if (!WiFi.isConnected()) { snprintf(g_netMsg, sizeof(g_netMsg), "offline"); continue; }
      g_netBusy = true;
      switch (job) {
        case JOB_WEATHER:  snprintf(g_netMsg, sizeof(g_netMsg), "weather..."); fetchWeather();  break;
        case JOB_STOCK:    snprintf(g_netMsg, sizeof(g_netMsg), "quote...");   fetchStock();    break;
        case JOB_FOOTBALL: snprintf(g_netMsg, sizeof(g_netMsg), "matches..."); fetchFootball(); break;
      }
      g_netMsg[0] = 0;
      g_netBusy = false;
    }
  }
}

// ---------------------------------------------------------------------------
// WiFi 状态机 (非阻塞)
// ---------------------------------------------------------------------------
static uint32_t g_wifiT0 = 0, g_retryAt = 0, g_ntpT0 = 0;
static uint8_t  g_failCnt = 0;
static bool     g_haveCreds = false;
static String   g_ssid, g_user, g_pass;

static void wifiTryConnect() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower((wifi_power_t)(CFG_TX_POWER_DBM * 4 / 10));

  if (g_user.length() > 0) {           // 企业级 WPA2-Enterprise (公司/校园网)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_eap_client_set_identity((uint8_t*)g_user.c_str(), g_user.length());
    esp_eap_client_set_username((uint8_t*)g_user.c_str(), g_user.length());
    esp_eap_client_set_password((uint8_t*)g_pass.c_str(), g_pass.length());
    esp_wifi_sta_enterprise_enable();
#else
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)g_user.c_str(), g_user.length());
    esp_wifi_sta_wpa2_ent_set_username((uint8_t*)g_user.c_str(), g_user.length());
    esp_wifi_sta_wpa2_ent_set_password((uint8_t*)g_pass.c_str(), g_pass.length());
    esp_wifi_sta_wpa2_ent_enable();
#endif
    WiFi.begin(g_ssid.c_str());
  } else {
    WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  }
  g_ns = NS_CONNECTING;
  g_wifiT0 = millis();
}

void netStartAP();          // 在 web.h 里实现(要注册 HTTP 路由)

void netInit() {
  g_dataMtx = xSemaphoreCreateMutex();
  g_jobQ    = xQueueCreate(4, sizeof(NetJob));
  // HTTPS 握手要大栈，12KB 是实测能跑 mbedTLS 的下限附近
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);

  prefs.begin("wifi", true);
  g_ssid = prefs.getString("ssid", CFG_WIFI_SSID);
  g_user = prefs.getString("user", "");
  g_pass = prefs.getString("pass", CFG_WIFI_PASS);
  prefs.end();

  g_haveCreds = g_ssid.length() > 0;
  if (g_haveCreds) wifiTryConnect();
  else            netStartAP();
}

void netSaveCreds(const String& ssid, const String& user, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid); prefs.putString("user", user); prefs.putString("pass", pass);
  prefs.end();
}
void netClearCreds() { prefs.begin("wifi", false); prefs.clear(); prefs.end(); }

void webBegin();            // web.h

static void ntpTick() {
  uint32_t now = millis();
  if (g_ntpOK) return;
  if (now - g_ntpT0 < 3000) return;
  g_ntpT0 = now;
  struct tm ti;
  if (getLocalTime(&ti, 0) && ti.tm_year > 120) { g_ntpOK = true; return; }
  static uint32_t lastCfg = 0;
  if (now - lastCfg > 25000) {          // 25s 还没对上就重新发起
    lastCfg = now;
    configTime(CFG_TZ_OFFSET_SEC, 0, CFG_NTP_1, CFG_NTP_2, CFG_NTP_3);
  }
}

// 每个 loop 调一次，永不阻塞
void netTick() {
  uint32_t now = millis();
  switch (g_ns) {
    case NS_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        g_ns = NS_ONLINE; g_failCnt = 0;
        configTime(CFG_TZ_OFFSET_SEC, 0, CFG_NTP_1, CFG_NTP_2, CFG_NTP_3);
        g_ntpT0 = now;
        webBegin();
        netRequest(JOB_WEATHER);
        netRequest(JOB_STOCK);
      } else if (now - g_wifiT0 > 12000) {
        if (g_failCnt < 200) g_failCnt++;
        // 指数退避: 3s, 6s, 12s, 24s, 上限 60s。不会因为路由器重启就进 AP。
        uint32_t back = 3000UL << (g_failCnt > 5 ? 5 : g_failCnt - 1);
        if (back > 60000UL) back = 60000UL;
        g_retryAt = now + back;
        g_ns = NS_RETRY;
      }
      break;
    case NS_RETRY:
      if ((int32_t)(now - g_retryAt) >= 0) wifiTryConnect();
      break;
    case NS_ONLINE:
      if (WiFi.status() != WL_CONNECTED) { g_ns = NS_RETRY; g_retryAt = now + 2000; g_ntpOK = false; }
      else ntpTick();
      break;
    default: break;
  }
}

// 状态字符串给 UI 用
const char* netStateStr() {
  switch (g_ns) {
    case NS_AP:         return "AP";
    case NS_ONLINE:     return g_ntpOK ? "OK" : "NTP?";
    case NS_CONNECTING: return "...";
    case NS_RETRY:      return "RETRY";
    default:            return "--";
  }
}
