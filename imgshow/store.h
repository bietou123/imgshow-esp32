// ============================================================================
// store.h  —  持久化 + 数据模型
//
// 老代码所有数据都只在 RAM: 备忘录、上传的图片、天气 —— 一断电全没，
// 每次开机 Image 页永远显示 "Waiting for image upload"。
//
// 现在的分工:
//   NVS (Preferences)  —— 小而关键的结构化数据: WiFi 凭据、备忘录、设置项。
//                         原因: 原子写、掉电安全、有磨损均衡、不怕 LittleFS 被格。
//   LittleFS (2MB)     —— 大块二进制: 图片 /img/*.565、视频 /vid/*.v64。
//                         原因: NVS 单条上限 4000 字节，塞不进 32KB 的图。
//                         选 LittleFS 而不是 SPIFFS: 有掉电一致性，不会像
//                         SPIFFS 那样写坏就整个挂掉。
//
// ⚠️ 必须用 "No OTA (2MB APP/2MB SPIFFS)" 分区表，默认分区只有 1.2MB app
//    (老代码已经占 91%)，而且 SPIFFS 区拿不到 2MB。见 README。
// ============================================================================
#pragma once
#include <LittleFS.h>
#include <Preferences.h>
#include "config.h"

static Preferences prefs;
static bool g_fsOK = false;

// ---------------------------------------------------------------------------
// 数据模型 (net.h 写，页面读)
// ---------------------------------------------------------------------------
struct WeatherData {
  char  city[20]  = "--";
  int   tempC     = -999;
  int   humid     = -1;
  int   windKmh   = -1;
  char  desc[22]  = "--";
  bool  valid     = false;
  uint32_t stamp  = 0;   // millis 时间戳，用于显示"多久前更新"
};

struct StockData {
  char  name[14]  = "--";
  char  price[12] = "--";
  char  pct[10]   = "--";
  bool  up        = true;
  bool  valid     = false;
  float kline[24];
  uint8_t klineN  = 0;
  uint32_t stamp  = 0;
};

struct MatchData {
  char home[14] = ""; char away[14] = "";
  int8_t hs = -1, as = -1;
  char when[14] = "";
};

#define MEMO_MAX      8
#define MEMO_TEXT_LEN 26      // 屏幕一行 21 字符，留点余量
struct MemoItem { char text[MEMO_TEXT_LEN]; bool done; };

static WeatherData g_wx;
static StockData   g_stk;
#define MATCH_MAX 8
static MatchData   g_match[MATCH_MAX];
static uint8_t     g_matchN = 0;

static MemoItem g_memo[MEMO_MAX];
static uint8_t  g_memoN = 0;

// 设置项
struct Settings {
  char     city[20]   = CFG_WEATHER_CITY;
  uint8_t  stockIdx   = 0;
  uint8_t  brightness = 255;
  bool     autoCycle  = false;   // 主页自动轮播
  uint16_t cycleSec   = 15;
  uint16_t refreshMin = 15;      // 天气/股票自动刷新间隔(分钟)
};
static Settings g_set;

// ---------------------------------------------------------------------------
// 备忘录: NVS
// ---------------------------------------------------------------------------
void memoLoad() {
  prefs.begin("memo", true);
  g_memoN = prefs.getUChar("n", 0);
  if (g_memoN > MEMO_MAX) g_memoN = 0;
  char key[8];
  for (uint8_t i = 0; i < g_memoN; i++) {
    snprintf(key, sizeof(key), "t%u", i);
    String s = prefs.getString(key, "");
    strncpy(g_memo[i].text, s.c_str(), MEMO_TEXT_LEN - 1);
    g_memo[i].text[MEMO_TEXT_LEN - 1] = 0;
    snprintf(key, sizeof(key), "d%u", i);
    g_memo[i].done = prefs.getBool(key, false);
  }
  prefs.end();
}

void memoSave() {
  prefs.begin("memo", false);
  prefs.putUChar("n", g_memoN);
  char key[8];
  for (uint8_t i = 0; i < g_memoN; i++) {
    snprintf(key, sizeof(key), "t%u", i); prefs.putString(key, g_memo[i].text);
    snprintf(key, sizeof(key), "d%u", i); prefs.putBool(key, g_memo[i].done);
  }
  prefs.end();
}

bool memoAdd(const char* txt) {
  if (g_memoN >= MEMO_MAX) return false;
  strncpy(g_memo[g_memoN].text, txt, MEMO_TEXT_LEN - 1);
  g_memo[g_memoN].text[MEMO_TEXT_LEN - 1] = 0;
  g_memo[g_memoN].done = false;
  g_memoN++;
  memoSave();
  return true;
}

void memoDel(uint8_t i) {
  if (i >= g_memoN) return;
  for (uint8_t k = i; k + 1 < g_memoN; k++) g_memo[k] = g_memo[k + 1];
  g_memoN--;
  memoSave();
}

void memoClear() { g_memoN = 0; memoSave(); }

// ---------------------------------------------------------------------------
// 设置: NVS
// ---------------------------------------------------------------------------
void setLoad() {
  prefs.begin("cfg", true);
  String c = prefs.getString("city", CFG_WEATHER_CITY);
  strncpy(g_set.city, c.c_str(), sizeof(g_set.city) - 1);
  g_set.city[sizeof(g_set.city) - 1] = 0;
  g_set.stockIdx   = prefs.getUChar("stk", 0);
  g_set.brightness = prefs.getUChar("bri", 255);
  g_set.autoCycle  = prefs.getBool("cyc", false);
  g_set.cycleSec   = prefs.getUShort("cycs", 15);
  g_set.refreshMin = prefs.getUShort("rfm", 15);
  prefs.end();
}
void setSave() {
  prefs.begin("cfg", false);
  prefs.putString("city", g_set.city);
  prefs.putUChar("stk", g_set.stockIdx);
  prefs.putUChar("bri", g_set.brightness);
  prefs.putBool("cyc", g_set.autoCycle);
  prefs.putUShort("cycs", g_set.cycleSec);
  prefs.putUShort("rfm", g_set.refreshMin);
  prefs.end();
}

// ---------------------------------------------------------------------------
// LittleFS: 图片 / 视频清单
// ---------------------------------------------------------------------------
#define IMG_MAX_FILES  40
#define IMG_BYTES      (128 * 128 * 2)      // 32768
#define VID_MAX_FILES  8

static char    g_imgName[IMG_MAX_FILES][20];
static uint8_t g_imgN = 0;
static char    g_vidName[VID_MAX_FILES][20];
static uint8_t g_vidN = 0;

void fsScanDir(const char* dir, char names[][20], uint8_t maxN, uint8_t* outN) {
  *outN = 0;
  if (!g_fsOK) return;
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return;
  File f = d.openNextFile();
  while (f && *outN < maxN) {
    if (!f.isDirectory()) {
      const char* p = f.name();
      const char* slash = strrchr(p, '/');
      if (slash) p = slash + 1;
      strncpy(names[*outN], p, 19); names[*outN][19] = 0;
      (*outN)++;
    }
    f = d.openNextFile();
  }
  d.close();
}

void fsRescan() {
  fsScanDir("/img", g_imgName, IMG_MAX_FILES, &g_imgN);
  fsScanDir("/vid", g_vidName, VID_MAX_FILES, &g_vidN);
}

void storeInit() {
  // formatOnFail=true: 第一次烧录时 LittleFS 区是空的，自动格式化
  g_fsOK = LittleFS.begin(true);
  if (g_fsOK) {
    if (!LittleFS.exists("/img")) LittleFS.mkdir("/img");
    if (!LittleFS.exists("/vid")) LittleFS.mkdir("/vid");
    fsRescan();
  }
  memoLoad();
  setLoad();
}

inline size_t fsTotal() { return g_fsOK ? LittleFS.totalBytes() : 0; }
inline size_t fsUsed()  { return g_fsOK ? LittleFS.usedBytes()  : 0; }
inline size_t fsFree()  { return g_fsOK ? (LittleFS.totalBytes() - LittleFS.usedBytes()) : 0; }
