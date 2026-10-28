// ============================================================================
// app.h  —  页面状态机 + 各功能页
//
// 老代码用两个变量描述"我在哪": bool menuActive + enum Mode currentMode，
// 而且 menuSel 又是第三份真相。web 的 /mode/clock 只改 currentMode 不改 menuSel，
// 之后按 OK 就会从一个陈旧的 menuSel 往下跳 —— 页面跳到哪全靠猜。
// 键盘 (kbActive) 更糟: 只有 kbResult 删空了才能退出，而退出时 kbResult 必然
// 是空的，所以那个回调永远拿不到用户输入 —— 输入法在老代码里是彻底不通的，
// 而且 startKeyboard() 根本没人调用。
//
// 现在只有一个 g_page + 一个 4 层页面栈。逃生通道在中央调度里:
//   长按 BACK  =>  无条件回主菜单，任何页面、任何子状态都生效。
// 页面自己拿不到这个事件，所以不存在"进得去出不来"。
// ============================================================================
#pragma once
#include "config.h"
#include "ui.h"
#include "buttons.h"
#include "store.h"
#include "net.h"
#include "media.h"
#if __has_include("smoke_imgs.h")
#include "smoke_imgs.h"
#define HAS_LOCAL_GALLERY 1
#else
#define HAS_LOCAL_GALLERY 0
#define SMOKE_COUNT 1
#endif

// ---------------------------------------------------------------------------
// 共享文字缓存池: 进入页面时统一清零，避免 fillScreen 后缓存与屏幕不一致
// ---------------------------------------------------------------------------
#define UIC_N   10
#define UIC_LEN 26
static char g_uic[UIC_N][UIC_LEN];
inline void uiCacheReset() { memset(g_uic, 0, sizeof(g_uic)); }

// ---------------------------------------------------------------------------
// 页面表
// ---------------------------------------------------------------------------
enum PageId : uint8_t {
  P_MENU = 0, P_CLOCK, P_WEATHER, P_STOCK, P_MEMO, P_IMAGE, P_VIDEO,
  P_SETTINGS, P_TIMER, P_FACE, P_SMOKE, P_FOOTBALL, P_GAME, P_INFO,
  P_KEYBOARD, P_APINFO, P_COUNT
};

struct PageDef {
  const char* name;
  void (*enter)();
  void (*tick)();
  void (*evt)();
  bool ownsBack;      // true = 该页自己处理"短按 BACK"(只有键盘页需要)
};
// PAGES[] 定义在文件末尾(要先有各页函数)。中间没有代码用到它。

static PageId   g_page  = P_CLOCK;
static PageId   g_stack[4];
static uint8_t  g_sp    = 0;
static uint32_t g_pageT0 = 0;
static PageId   g_pending = P_COUNT;   // web 请求换页，由主循环执行

void gotoPage(PageId p);
inline void requestPage(PageId p) { g_pending = p; }

void pushPage(PageId p) {
  if (g_sp < 4) g_stack[g_sp++] = g_page;
  gotoPage(p);
}
void popPage() {
  if (g_sp > 0) gotoPage(g_stack[--g_sp]);
  else          gotoPage(P_MENU);
}

// ---------------------------------------------------------------------------
// 通用小部件
// ---------------------------------------------------------------------------
// 顶部左上角的网络状态，差分刷新
static void netBadge() {
  uiText(2, 2, 1, netOnline() ? COL_OK : COL_ERR, COL_BG, netStateStr(), g_uic[9], 6);
}

static void twoStepHint(const char* s) { uiToast(s, COL_WARN); }

// 时间字符串
static bool nowTm(struct tm* t) { return getLocalTime(t, 0) && t->tm_year > 120; }

// ===========================================================================
// 主菜单
// ===========================================================================
struct MenuItem { const char* label; PageId page; };
static const MenuItem MENU[] = {
  {"Clock",       P_CLOCK},
  {"Weather",     P_WEATHER},
  {"Stock",       P_STOCK},
  {"Memo",        P_MEMO},
  {"Image",       P_IMAGE},
  {"Video",       P_VIDEO},
  {"Face",        P_FACE},
  {"Gallery",     P_SMOKE},
  {"Football",    P_FOOTBALL},
  {"Timer",       P_TIMER},
  {"Dino Game",   P_GAME},
  {"Device Info", P_INFO},
  {"Settings",    P_SETTINGS},
};
static const uint8_t MENU_N = sizeof(MENU) / sizeof(MENU[0]);
static const uint8_t MENU_VIS = 5;
static uint8_t g_menuSel = 0, g_menuTop = 0;

static void menuRow(uint8_t slot, uint8_t idx, bool sel) {
  int16_t y = 18 + slot * 20;
  tft.fillRoundRect(4, y, 120, 18, 3, sel ? COL_SEL : COL_BG);
  char buf[24];
  snprintf(buf, sizeof(buf), "%c%s", sel ? '>' : ' ', MENU[idx].label);
  uiPut(10, y + 5, 1, sel ? COL_WARN : COL_FG, buf);
}

static void menuDrawAll() {
  for (uint8_t v = 0; v < MENU_VIS; v++) {
    uint8_t i = g_menuTop + v;
    if (i < MENU_N) menuRow(v, i, i == g_menuSel);
    else tft.fillRect(4, 18 + v * 20, 120, 18, COL_BG);
  }
  tft.fillRect(0, 119, SCR_W, 9, COL_BG);
  char nav[24]; snprintf(nav, sizeof(nav), "%u/%u  OK=open", g_menuSel + 1, MENU_N);
  uiPutCenter(120, 1, COL_DIM, nav);
  uiPut(2, 120, 1, COL_DIM, g_menuTop ? "^" : " ");
  uiPut(120, 120, 1, COL_DIM, (g_menuTop + MENU_VIS < MENU_N) ? "v" : " ");
}

static void menuEnter() { uiHeader("== MENU =="); menuDrawAll(); }
static void menuTick()  { netBadge(); }

static void menuMove(int8_t d) {
  uint8_t old = g_menuSel, oldTop = g_menuTop;
  g_menuSel = (uint8_t)((g_menuSel + MENU_N + d) % MENU_N);
  if (g_menuSel < g_menuTop) g_menuTop = g_menuSel;
  if (g_menuSel >= g_menuTop + MENU_VIS) g_menuTop = g_menuSel - MENU_VIS + 1;
  if (g_menuTop != oldTop) menuDrawAll();
  else {   // 只重画变化的两行，不整屏刷 -> 完全不闪
    menuRow(old - g_menuTop, old, false);
    menuRow(g_menuSel - g_menuTop, g_menuSel, true);
    char nav[24]; snprintf(nav, sizeof(nav), "%u/%u  OK=open", g_menuSel + 1, MENU_N);
    tft.fillRect(0, 119, SCR_W, 9, COL_BG);
    uiPutCenter(120, 1, COL_DIM, nav);
  }
}

static void menuEvt() {
  if (btnStep(B_UP))   menuMove(-1);
  if (btnStep(B_DOWN)) menuMove(+1);
  if (btnClick(B_OK))  gotoPage(MENU[g_menuSel].page);
}

// ===========================================================================
// 时钟
// ===========================================================================
static void clockEnter() {
  uiHeader("CLOCK");
  uiHint("OK=weather  BACK=menu");
}
static void clockTick() {
  static uint32_t last = 0;
  netBadge();
  if (millis() - last < 200) return;
  last = millis();

  struct tm t;
  if (!nowTm(&t)) {
    uiTextCenter(40, 1, COL_WARN, COL_BG, "waiting for NTP", g_uic[0], 20);
    uiTextCenter(56, 1, COL_DIM, COL_BG, netOnline() ? "online, syncing.." : "no network", g_uic[1], 20);
    return;
  }
  if (g_uic[0][0]) { tft.fillRect(0, 36, SCR_W, 30, COL_BG); g_uic[0][0] = 0; g_uic[1][0] = 0; }

  char b[24];
  snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
  uiText(19, 26, 3, COL_ACCENT, COL_BG, b, g_uic[2], 5);
  snprintf(b, sizeof(b), "%02d", t.tm_sec);
  uiText(52, 54, 2, COL_FG, COL_BG, b, g_uic[3], 2);
  snprintf(b, sizeof(b), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  uiText(34, 78, 1, COL_OK, COL_BG, b, g_uic[4], 10);
  static const char* D[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  uiText(52, 92, 1, COL_WARN, COL_BG, D[t.tm_wday], g_uic[5], 3);
  if (g_wx.valid) snprintf(b, sizeof(b), "%dC %.14s", g_wx.tempC, g_wx.desc);
  else            snprintf(b, sizeof(b), "%s", "");
  uiTextCenter(106, 1, COL_DIM, COL_BG, b, g_uic[6], 21);
}
static void clockEvt() { if (btnClick(B_OK)) gotoPage(P_WEATHER); }

// ===========================================================================
// 天气
// ===========================================================================
static uint32_t g_wxSeen = 0xFFFFFFFF;
static void wxEnter() {
  uiHeader("WEATHER");
  uiHint("OK=refresh  hold=city");
  g_wxSeen = 0xFFFFFFFF;
  if (!g_wx.valid || millis() - g_wx.stamp > (uint32_t)g_set.refreshMin * 60000UL) netRequest(JOB_WEATHER);
}
static void wxTick() {
  netBadge();
  static uint32_t lastBusy = 0;
  if (netBusy() && millis() - lastBusy > 400) {
    lastBusy = millis();
    uiText(100, 2, 1, COL_WARN, COL_BG, (millis() / 400) % 2 ? "**" : "  ", g_uic[8], 2);
  }
  if (g_wxSeen == g_wxRev) return;
  g_wxSeen = g_wxRev;

  char b[26];
  netLock(); WeatherData w = g_wx; netUnlock();
  uiTextCenter(16, 1, COL_ACCENT, COL_BG, w.city, g_uic[0], 21);
  if (!w.valid) {
    uiTextCenter(50, 2, COL_ERR, COL_BG, "no data", g_uic[1], 10);
    uiTextCenter(76, 1, COL_DIM, COL_BG, netOnline() ? "API failed, OK=retry" : "offline", g_uic[2], 21);
    return;
  }
  snprintf(b, sizeof(b), "%dC", w.tempC);
  uiTextCenter(34, 3, COL_FG, COL_BG, b, g_uic[1], 6);
  uiTextCenter(66, 1, COL_OK, COL_BG, w.desc, g_uic[2], 21);
  snprintf(b, sizeof(b), "Humid %d%%", w.humid < 0 ? 0 : w.humid);
  uiText(4, 84, 1, COL_WARN, COL_BG, b, g_uic[3], 12);
  snprintf(b, sizeof(b), "Wind %dkm/h", w.windKmh < 0 ? 0 : w.windKmh);
  uiText(4, 96, 1, COL_WARN, COL_BG, b, g_uic[4], 14);
  snprintf(b, sizeof(b), "%lus ago", (unsigned long)((millis() - w.stamp) / 1000));
  uiText(4, 108, 1, COL_DIM, COL_BG, b, g_uic[5], 12);
}
void kbOpen(const char* title, const char* init, void (*done)(const char*));
static void wxCityDone(const char* s) {
  if (s && s[0]) { strncpy(g_set.city, s, sizeof(g_set.city) - 1); g_set.city[sizeof(g_set.city)-1]=0; setSave(); netRequest(JOB_WEATHER); }
}
static void wxEvt() {
  if (btnClick(B_OK)) { netRequest(JOB_WEATHER); uiToast("refreshing...", COL_DIM); g_wxSeen = 0xFFFFFFFF; }
  if (btnLong(B_OK))  kbOpen("City name", g_set.city, wxCityDone);
}

// ===========================================================================
// 股票
// ===========================================================================
static uint32_t g_stkSeen = 0xFFFFFFFF;
static void stkEnter() {
  uiHeader("STOCK");
  uiHint("U/D=index OK=refresh");
  g_stkSeen = 0xFFFFFFFF;
  if (!g_stk.valid || millis() - g_stk.stamp > (uint32_t)g_set.refreshMin * 60000UL) netRequest(JOB_STOCK);
}
static void stkTick() {
  netBadge();
  if (g_stkSeen == g_stkRev) return;
  g_stkSeen = g_stkRev;
  netLock(); StockData s = g_stk; netUnlock();

  char b[26];
  uiText(2, 14, 1, COL_ACCENT, COL_BG, s.name, g_uic[0], 13);
  snprintf(b, sizeof(b), "%s%s%%", s.up ? "+" : "", s.pct);
  uiText(76, 14, 1, s.up ? COL_OK : COL_ERR, COL_BG, b, g_uic[1], 8);
  uiText(4, 26, 2, s.up ? COL_OK : COL_ERR, COL_BG, s.price, g_uic[2], 9);

  // K线区 y=48..112，只有新数据才重画
  tft.fillRect(0, 46, SCR_W, 68, COL_BG);
  if (s.klineN >= 2) {
    float mn = s.kline[0], mx = s.kline[0];
    for (uint8_t i = 1; i < s.klineN; i++) { if (s.kline[i] < mn) mn = s.kline[i]; if (s.kline[i] > mx) mx = s.kline[i]; }
    float rg = mx - mn; if (rg < 0.001f) rg = 0.001f;
    const int16_t Y0 = 48, Y1 = 112, X0 = 4, X1 = 124;
    tft.drawFastHLine(X0, Y0, X1 - X0, 0x2104);
    tft.drawFastHLine(X0, (Y0 + Y1) / 2, X1 - X0, 0x2104);
    tft.drawFastHLine(X0, Y1, X1 - X0, 0x2104);
    uint16_t fill = s.up ? tft.color565(0, 50, 0) : tft.color565(50, 0, 0);
    int16_t px = -1, py = -1;
    for (uint8_t i = 0; i < s.klineN; i++) {
      int16_t x = X0 + (int16_t)((int32_t)i * (X1 - X0) / (s.klineN - 1));
      int16_t y = Y1 - (int16_t)((s.kline[i] - mn) / rg * (Y1 - Y0));
      if (y < Y0) y = Y0; if (y > Y1) y = Y1;
      tft.drawFastVLine(x, y, Y1 - y, fill);
      if (px >= 0) tft.drawLine(px, py, x, y, s.up ? COL_OK : COL_ERR);
      px = x; py = y;
    }
  } else {
    uiPutCenter(76, 1, COL_DIM, s.valid ? "no kline data" : (netOnline() ? "fetching..." : "offline"));
  }
  g_uic[3][0] = 0;
}
static void stkEvt() {
  if (btnStep(B_UP))   { g_set.stockIdx = (g_set.stockIdx + STOCK_COUNT - 1) % STOCK_COUNT; setSave(); netRequest(JOB_STOCK); uiToast("loading...", COL_DIM); }
  if (btnStep(B_DOWN)) { g_set.stockIdx = (g_set.stockIdx + 1) % STOCK_COUNT;               setSave(); netRequest(JOB_STOCK); uiToast("loading...", COL_DIM); }
  if (btnClick(B_OK))  { netRequest(JOB_STOCK); uiToast("refreshing...", COL_DIM); }
}

// ===========================================================================
// 备忘录
// ===========================================================================
static uint8_t  g_memoSel = 0;
static uint32_t g_memoConfirmMs = 0;

static void memoDraw() {
  tft.fillRect(0, 12, SCR_W, 106, COL_BG);
  uint8_t rows = g_memoN + 1;                 // 最后一行是 "+ New"
  if (g_memoSel >= rows) g_memoSel = rows - 1;
  uint8_t top = 0;
  const uint8_t VIS = 7;
  if (g_memoSel >= VIS) top = g_memoSel - VIS + 1;
  for (uint8_t v = 0; v < VIS && top + v < rows; v++) {
    uint8_t i = top + v;
    int16_t y = 14 + v * 14;
    bool sel = (i == g_memoSel);
    if (sel) tft.fillRect(2, y - 1, 124, 12, COL_SEL);
    char b[24];
    if (i == g_memoN) snprintf(b, sizeof(b), "%c+ New item", sel ? '>' : ' ');
    else snprintf(b, sizeof(b), "%c%c%.18s", sel ? '>' : ' ', g_memo[i].done ? 'x' : '-', g_memo[i].text);
    uiPut(4, y, 1, sel ? COL_WARN : (i < g_memoN && g_memo[i].done ? COL_DIM : COL_FG), b);
  }
  if (g_memoN == 0) uiPut(4, 30, 1, COL_DIM, "empty - add via web");
}
static void memoEnter() {
  uiHeader("MEMO");
  uiHint("OK=done hold=del");
  g_memoSel = 0; g_memoConfirmMs = 0;
  memoDraw();
}
static void memoTick() { netBadge(); }
// 注意: kbClose 是先 popPage(回到本页并重绘)再调回调，所以这里要再画一次
static void memoNewDone(const char* s) { if (s && s[0]) { memoAdd(s); memoDraw(); } }
static void memoEvt() {
  uint8_t rows = g_memoN + 1;
  if (btnStep(B_UP))   { g_memoSel = (g_memoSel + rows - 1) % rows; g_memoConfirmMs = 0; memoDraw(); }
  if (btnStep(B_DOWN)) { g_memoSel = (g_memoSel + 1) % rows;        g_memoConfirmMs = 0; memoDraw(); }
  if (btnClick(B_OK)) {
    if (g_memoSel == g_memoN) kbOpen("New memo", "", memoNewDone);
    else { g_memo[g_memoSel].done = !g_memo[g_memoSel].done; memoSave(); memoDraw(); }
  }
  if (btnLong(B_OK) && g_memoSel < g_memoN) {
    // 两段确认: 第一次长按提示，3 秒内再长按才真删
    if (g_memoConfirmMs && millis() - g_memoConfirmMs < 3000) {
      memoDel(g_memoSel); g_memoConfirmMs = 0; memoDraw();
    } else { g_memoConfirmMs = millis(); twoStepHint("hold again to DELETE"); }
  }
}

// ===========================================================================
// 图片
// ===========================================================================
static bool     g_imgSlide = false;
static uint32_t g_imgSlideT = 0, g_imgConfirm = 0;

static void imgInfoScreen() {
  tft.fillScreen(COL_BG);
  uiHeader("IMAGE");
  uiPutCenter(30, 1, COL_FG, "no image on flash");
  uiPutCenter(46, 1, COL_DIM, "upload from phone:");
  char b[26];
  snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  uiPutCenter(60, 1, COL_ACCENT, netOnline() ? b : "connect wifi first");
  snprintf(b, sizeof(b), "free %uKB", (unsigned)(fsFree() / 1024));
  uiPutCenter(80, 1, COL_DIM, b);
  uiHint("BACK=menu");
}
static void imgEnter() {
  g_imgConfirm = 0;
  fsRescan();
  if (g_imgN == 0) { imgInfoScreen(); return; }
  if (g_imgCur < 0 || g_imgCur >= (int8_t)g_imgN) g_imgCur = 0;
  imgShow(g_imgCur);
  g_imgSlideT = millis();
}
static void imgTick() {
  if (g_imgN == 0) return;
  if (g_imgSlide && millis() - g_imgSlideT > 5000) { g_imgSlideT = millis(); imgShow(g_imgCur + 1); }
}
static void imgEvt() {
  if (g_imgN == 0) { if (btnClick(B_OK)) imgEnter(); return; }
  if (btnStep(B_UP))   { imgShow(g_imgCur - 1); g_imgSlideT = millis(); g_imgConfirm = 0; }
  if (btnStep(B_DOWN)) { imgShow(g_imgCur + 1); g_imgSlideT = millis(); g_imgConfirm = 0; }
  if (btnClick(B_OK))  { imgShow(g_imgCur + 1); g_imgSlideT = millis(); g_imgConfirm = 0; }
  if (btnDouble(B_OK)) { g_imgSlide = !g_imgSlide; uiToast(g_imgSlide ? "slideshow ON" : "slideshow OFF", COL_ACCENT); g_imgSlideT = millis(); }
  if (btnLong(B_OK)) {
    if (g_imgConfirm && millis() - g_imgConfirm < 3000) {
      char n[20]; strncpy(n, g_imgName[g_imgCur], 19); n[19] = 0;
      imgDelete(n); g_imgConfirm = 0; imgEnter();
    } else { g_imgConfirm = millis(); twoStepHint("hold again to DELETE"); }
  }
}

// ===========================================================================
// 视频
// ===========================================================================
enum VidMode : uint8_t { VM_FILE, VM_LIVE };
static VidMode  g_vm = VM_FILE;
static uint32_t g_vidConfirm = 0;

static void vidInfoScreen() {
  tft.fillScreen(COL_BG);
  uiHeader("VIDEO");
  char b[26];
  if (g_vm == VM_LIVE) {
    uiPutCenter(26, 1, COL_ACCENT, "LIVE TCP stream");
    snprintf(b, sizeof(b), "%s", WiFi.localIP().toString().c_str());
    uiPutCenter(42, 1, netOnline() ? COL_FG : COL_ERR, netOnline() ? b : "no network");
    snprintf(b, sizeof(b), "port %d", CFG_VIDEO_TCP_PORT);
    uiPutCenter(54, 1, COL_FG, b);
    uiPutCenter(70, 1, COL_DIM, "run stream_video.py");
    uiPutCenter(82, 1, COL_DIM, "128x128 raw RGB565");
    uiPutCenter(94, 1, COL_WARN, "waiting client...");
  } else {
    uiPutCenter(30, 1, COL_FG, "no clip on flash");
    uiPutCenter(46, 1, COL_DIM, "make one with");
    uiPutCenter(58, 1, COL_ACCENT, "tools/mkvideo.py");
    uiPutCenter(74, 1, COL_DIM, "then upload via web");
    snprintf(b, sizeof(b), "free %uKB", (unsigned)(fsFree() / 1024));
    uiPutCenter(92, 1, COL_DIM, b);
  }
  uiHint("dbl OK=file/live");
}
static void vidEnter() {
  g_vidConfirm = 0;
  liveBegin();
  fsRescan();
  if (g_vm == VM_LIVE) { if (!liveConnected()) vidInfoScreen(); else tft.fillScreen(COL_BG); return; }
  if (g_vidN == 0) { vidInfoScreen(); return; }
  if (g_vidCur < 0 || g_vidCur >= (int8_t)g_vidN) g_vidCur = 0;
  if (!vidOpen(g_vidCur)) vidInfoScreen();
}
static void vidPageTick() {
  if (g_vm == VM_LIVE) { liveTick(); return; }
  vidTick();
}
static void vidEvt() {
  if (btnDouble(B_OK)) {
    vidClose(); liveDisconnect();
    g_vm = (g_vm == VM_FILE) ? VM_LIVE : VM_FILE;
    gotoPage(P_VIDEO);
    return;
  }
  if (g_vm == VM_LIVE) return;
  if (g_vidN == 0) { if (btnClick(B_OK)) gotoPage(P_VIDEO); return; }
  if (btnStep(B_UP))   { vidOpen(g_vidCur - 1); g_vidConfirm = 0; }
  if (btnStep(B_DOWN)) { vidOpen(g_vidCur + 1); g_vidConfirm = 0; }
  if (btnClick(B_OK))  { g_vidPause = !g_vidPause; if (g_vidPause) uiToast("PAUSED", COL_WARN); else g_vidNextMs = millis(); }
  if (btnLong(B_OK)) {
    if (g_vidConfirm && millis() - g_vidConfirm < 3000) {
      char n[20]; strncpy(n, g_vidName[g_vidCur], 19); n[19] = 0;
      vidDelete(n); g_vidConfirm = 0; gotoPage(P_VIDEO);
    } else { g_vidConfirm = millis(); twoStepHint("hold again to DELETE"); }
  }
}

// ===========================================================================
// 表情 (移植自 nomi)
// ===========================================================================
static uint16_t FACE_EYE, FACE_RED, FACE_BLU;
static float    g_faceAnim = 0;
static uint8_t  g_faceType = 0;
static bool     g_faceAuto = true;
static uint32_t g_faceSwitch = 0;

static void faceRing(int x, int y, int r, uint16_t c) { tft.drawCircle(x, y, r, c); tft.drawCircle(x, y, r - 1, c); }
static void faceCapsule(int cx, int cy, int hh) {
  if (hh < 3) hh = 3;
  tft.drawRoundRect(cx - 10, cy - hh, 20, hh * 2, 10, FACE_EYE);
  tft.drawRoundRect(cx - 9, cy - hh + 1, 18, hh * 2 - 2, 9, FACE_EYE);
}
static void faceHeart(int cx, int cy, int s, uint16_t c) {
  tft.fillCircle(cx - s / 2, cy - s / 3, s / 2 + 1, c);
  tft.fillCircle(cx + s / 2, cy - s / 3, s / 2 + 1, c);
  tft.fillTriangle(cx - s, cy - s / 4, cx + s, cy - s / 4, cx, cy + s, c);
}
static void faceDraw() {
  tft.fillRect(0, 12, SCR_W, 104, COL_BG);   // 只擦脸区，不动标题和提示
  g_faceAnim += 0.15f;
  int elx = 40, erx = 88, cy = 56;
  switch (g_faceType) {
    case 0: { int dx = (int)(4 * sin(g_faceAnim * 0.3f)), dy = (int)(3 * sin(g_faceAnim * 0.2f));
              faceCapsule(elx + dx, cy + dy, 18); faceCapsule(erx + dx, cy + dy, 18); break; }
    case 1: { for (int i = -15; i <= 15; i++) { int y = (int)(9.0f * cos(i * 3.14159f / 30.0f));
                tft.drawPixel(elx + i, cy + y, FACE_EYE); tft.drawPixel(elx + i, cy + y + 1, FACE_EYE);
                tft.drawPixel(erx + i, cy + y, FACE_EYE); tft.drawPixel(erx + i, cy + y + 1, FACE_EYE); }
              int br = 4 + (int)(1.5f * sin(g_faceAnim * 2));
              tft.fillCircle(36, 80, br, FACE_RED); tft.fillCircle(92, 80, br, FACE_RED); break; }
    case 2: { faceHeart(elx, cy, 11, FACE_RED); faceHeart(erx, cy, 11, FACE_RED);
              faceHeart(64 + (int)(20 * sin(g_faceAnim * 0.6f)), 26 + (int)(5 * sin(g_faceAnim * 1.2f)), 6, FACE_RED); break; }
    case 3: { int r = 14 + (int)(2 * sin(g_faceAnim * 3));
              faceRing(elx, cy, r, FACE_EYE); faceRing(erx, cy, r, FACE_EYE); faceRing(64, 94, 5, FACE_EYE); break; }
    case 4: { int s = 12, j = (int)(2 * sin(g_faceAnim * 5));
              for (int w = -1; w <= 1; w++) {
                tft.drawLine(elx-s, cy-s+j+w, elx+s, cy+s+j+w, FACE_EYE);
                tft.drawLine(elx-s, cy+s+j+w, elx+s, cy-s+j+w, FACE_EYE);
                tft.drawLine(erx-s, cy-s-j+w, erx+s, cy+s-j+w, FACE_EYE);
                tft.drawLine(erx-s, cy+s-j+w, erx+s, cy-s-j+w, FACE_EYE); } break; }
    case 5: { faceCapsule(elx, cy, 18); faceCapsule(erx, cy, 18);
              float t1 = fmodf(g_faceAnim * 0.5f, 4.0f), t2 = fmodf(g_faceAnim * 0.5f + 2.0f, 4.0f);
              int r1 = 3 - (int)t1; if (r1 < 1) r1 = 1;
              int r2 = 3 - (int)t2; if (r2 < 1) r2 = 1;
              tft.fillCircle(elx + 2, cy + 20 + (int)(t1 * 12), r1, FACE_BLU);
              tft.fillCircle(erx + 2, cy + 20 + (int)(t2 * 12), r2, FACE_BLU); break; }
    case 6: { faceCapsule(elx, cy, 18);
              if (fmodf(g_faceAnim, 10.0f) < 2.5f) {
                for (int i = -14; i <= 14; i++) { int y = (int)(7.0f * cos(i * 3.14159f / 28.0f));
                  tft.drawPixel(erx + i, cy + y + 4, FACE_EYE); tft.drawPixel(erx + i, cy + y + 5, FACE_EYE); }
              } else faceCapsule(erx, cy, 18); break; }
    default:{ tft.drawFastHLine(30, cy, 20, FACE_EYE); tft.drawFastHLine(30, cy + 1, 20, FACE_EYE);
              tft.drawFastHLine(78, cy, 20, FACE_EYE); tft.drawFastHLine(78, cy + 1, 20, FACE_EYE);
              int zy = 32 + (int)(4 * sin(g_faceAnim * 0.8f));
              uiPut(95, zy, 2, tft.color565(150, 200, 255), "Z");
              uiPut(107, zy - 12, 1, tft.color565(150, 200, 255), "z"); break; }
  }
}
static const char* FACE_NAME[] = {"idle","happy","love","surprise","dead","cry","wink","sleep"};
static void faceEnter() {
  FACE_EYE = tft.color565(255, 240, 200);
  FACE_RED = tft.color565(255, 50, 80);
  FACE_BLU = tft.color565(80, 150, 255);
  uiHeader(FACE_NAME[g_faceType], COL_DIM);
  uiHint("U/D=face  OK=auto");
  g_faceSwitch = millis();
}
static void faceTick() {
  static uint32_t last = 0;
  if (g_faceAuto && millis() - g_faceSwitch > 8000) {
    g_faceSwitch = millis(); g_faceType = random(0, 8); uiHeader(FACE_NAME[g_faceType], COL_DIM);
  }
  if (millis() - last < 80) return;
  last = millis();
  faceDraw();
}
static void faceEvt() {
  if (btnStep(B_UP))   { g_faceType = (g_faceType + 7) % 8; uiHeader(FACE_NAME[g_faceType], COL_DIM); g_faceSwitch = millis(); }
  if (btnStep(B_DOWN)) { g_faceType = (g_faceType + 1) % 8; uiHeader(FACE_NAME[g_faceType], COL_DIM); g_faceSwitch = millis(); }
  if (btnClick(B_OK))  { g_faceAuto = !g_faceAuto; uiToast(g_faceAuto ? "auto ON" : "auto OFF", COL_ACCENT); }
}

// ===========================================================================
// 烟盒图 (PROGMEM)
// ===========================================================================
static uint8_t  g_smokeIdx = 0;
static uint32_t g_smokeT = 0;
static bool     g_smokeAuto = true;
static void smokeShow() {
#if HAS_LOCAL_GALLERY
  tft.drawRGBBitmap(0, 0, (const uint16_t*)smoke_images[g_smokeIdx], 128, 128);
#else
  tft.fillScreen(COL_BG);
  uiPutCenter(58, 1, COL_DIM, "No local images");
#endif
  g_smokeT = millis();
}
static void smokeEnter() { smokeShow(); }
static void smokeTick()  { if (g_smokeAuto && millis() - g_smokeT > 5000) { g_smokeIdx = (g_smokeIdx + 1) % SMOKE_COUNT; smokeShow(); } }
static void smokeEvt() {
  if (btnStep(B_UP))   { g_smokeIdx = (g_smokeIdx + SMOKE_COUNT - 1) % SMOKE_COUNT; smokeShow(); }
  if (btnStep(B_DOWN)) { g_smokeIdx = (g_smokeIdx + 1) % SMOKE_COUNT; smokeShow(); }
  if (btnClick(B_OK))  { g_smokeAuto = !g_smokeAuto; uiToast(g_smokeAuto ? "auto ON" : "auto OFF", COL_ACCENT); }
}

// ===========================================================================
// 足球
// ===========================================================================
static uint8_t  g_mIdx = 0;
static uint32_t g_mSeen = 0xFFFFFFFF;
static void fbEnter() {
  uiHeader("WORLD CUP");
  uiHint("U/D=page OK=refresh");
  g_mSeen = 0xFFFFFFFF; g_mIdx = 0;
  if (g_matchN == 0) netRequest(JOB_FOOTBALL);
}
static void fbDraw() {
  tft.fillRect(0, 12, SCR_W, 106, COL_BG);
  if (g_matchN == 0) { uiPutCenter(60, 1, COL_DIM, netBusy() ? "loading..." : (netOnline() ? "no data, OK=retry" : "offline")); return; }
  if (g_mIdx >= g_matchN) g_mIdx = 0;
  MatchData& m = g_match[g_mIdx];
  uiPut(4, 22, 1, COL_FG, m.home);
  uiPut(4, 80, 1, COL_FG, m.away);
  char sc[10];
  if (m.hs < 0) snprintf(sc, sizeof(sc), "vs"); else snprintf(sc, sizeof(sc), "%d-%d", m.hs, m.as);
  uiPutCenter(44, 3, COL_ACCENT, sc);
  uiPut(4, 96, 1, COL_OK, m.when);
  char nav[20]; snprintf(nav, sizeof(nav), "%u/%u", g_mIdx + 1, g_matchN);
  uiPut(100, 96, 1, COL_DIM, nav);
}
static void fbTick() { netBadge(); if (g_mSeen != g_matchRev) { g_mSeen = g_matchRev; fbDraw(); } }
static void fbEvt() {
  if (g_matchN) {
    if (btnStep(B_UP))   { g_mIdx = (g_mIdx + g_matchN - 1) % g_matchN; fbDraw(); }
    if (btnStep(B_DOWN)) { g_mIdx = (g_mIdx + 1) % g_matchN; fbDraw(); }
  }
  if (btnClick(B_OK)) { netRequest(JOB_FOOTBALL); uiToast("loading...", COL_DIM); }
}

// ===========================================================================
// 定时器
// 老代码用 millis() < timerTarget 判断，49.7 天回绕时会瞬间"到时"。
// 改成有符号差值比较，回绕安全。
// ===========================================================================
static uint32_t g_tmTarget = 0;
static uint16_t g_tmSetMin = 5;
static bool     g_tmRun = false, g_tmAlarm = false;
static char     g_tmName[18] = "Timer";

static void tmChrome() {
  uiHeader("TIMER");
  if (g_tmAlarm)      uiHint("OK=dismiss");
  else if (g_tmRun)   uiHint("OK=stop  BACK=menu");
  else                uiHint("U/D=set  OK=start");
}
static void tmEnter() { g_tmAlarm = false; tmChrome(); uiPutCenter(20, 1, COL_FG, g_tmName); }
static void tmTick() {
  netBadge();
  uint32_t now = millis();
  char b[12];
  if (g_tmAlarm) {
    static uint32_t f = 0; static bool on = false;
    if (now - f > 350) { f = now; on = !on;
      if (on) buzBeep(2300, 180);         // 没装蜂鸣器时是空操作
      tft.fillRect(0, 30, SCR_W, 60, on ? COL_ERR : COL_BG);
      uiPutCenter(46, 2, on ? COL_FG : COL_ERR, "TIME UP");
      uiPutCenter(70, 1, on ? COL_FG : COL_WARN, g_tmName);
      g_uic[0][0] = 0;
    }
    return;
  }
  if (g_tmRun) {
    int32_t rem = (int32_t)(g_tmTarget - now);
    if (rem <= 0) { g_tmRun = false; g_tmAlarm = true; tmChrome(); return; }
    uint32_t s = (uint32_t)rem / 1000;
    snprintf(b, sizeof(b), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    uiTextCenter(48, 3, COL_OK, COL_BG, b, g_uic[0], 5);
  } else {
    snprintf(b, sizeof(b), "%02u:00", g_tmSetMin);
    uiTextCenter(48, 3, COL_WARN, COL_BG, b, g_uic[0], 5);
  }
}
static void tmEvt() {
  if (g_tmAlarm) { if (btnClick(B_OK) || btnClick(B_UP) || btnClick(B_DOWN)) { g_tmAlarm = false; gotoPage(P_TIMER); } return; }
  if (g_tmRun) { if (btnClick(B_OK)) { g_tmRun = false; gotoPage(P_TIMER); } return; }
  if (btnStep(B_UP))   { if (g_tmSetMin < 99) g_tmSetMin++; }
  if (btnStep(B_DOWN)) { if (g_tmSetMin > 1)  g_tmSetMin--; }
  if (btnClick(B_OK))  { g_tmTarget = millis() + (uint32_t)g_tmSetMin * 60000UL; g_tmRun = true; g_tmAlarm = false; tmChrome(); g_uic[0][0] = 0; }
}
void timerStartFromWeb(const char* name, uint16_t minutes) {
  strncpy(g_tmName, name, sizeof(g_tmName) - 1); g_tmName[sizeof(g_tmName) - 1] = 0;
  if (minutes < 1) minutes = 1; if (minutes > 999) minutes = 999;
  g_tmSetMin = minutes;
  g_tmTarget = millis() + (uint32_t)minutes * 60000UL;
  g_tmRun = true; g_tmAlarm = false;
  requestPage(P_TIMER);
}

// ===========================================================================
// 小恐龙 (保留原有玩法，改用新按钮层)
// ===========================================================================
#define DINO_GROUND 104
#define DINO_W 12
#define DINO_H 14
#define CACTUS_W 6
#define CACTUS_MAX 3
static float    g_dy, g_dvel, g_dspeed;
static bool     g_djump, g_dover;
static int      g_dscore, g_dbest = 0, g_dspawn;
static uint32_t g_dframe;
static struct { float x; int h; bool active; } g_cac[CACTUS_MAX];
static int      g_prevDy, g_prevCx[CACTUS_MAX];

static void gameReset() {
  g_dy = DINO_GROUND; g_dvel = 0; g_djump = false; g_dover = false;
  g_dscore = 0; g_dspeed = 3.0f; g_dframe = millis(); g_dspawn = 30;
  for (int i = 0; i < CACTUS_MAX; i++) { g_cac[i].active = false; g_prevCx[i] = 130; }
  g_prevDy = DINO_GROUND;
  tft.fillScreen(COL_BG);
  tft.drawFastHLine(0, DINO_GROUND + DINO_H, SCR_W, tft.color565(80, 80, 80));
  uiHint("UP=jump  BACK=menu");
}
static void gameEnter() { gameReset(); }
static void gameTick() {
  uint32_t now = millis();
  if (now - g_dframe < 40) return;
  g_dframe = now;
  if (g_dover) return;

  if ((btnHeld(B_UP) || btnHeld(B_OK)) && !g_djump) { g_djump = true; g_dvel = -5.5f; }
  if (g_djump) { g_dvel += 0.5f; g_dy += g_dvel; if (g_dy >= DINO_GROUND) { g_dy = DINO_GROUND; g_dvel = 0; g_djump = false; } }

  if (--g_dspawn <= 0) {
    for (int i = 0; i < CACTUS_MAX; i++) if (!g_cac[i].active) { g_cac[i].active = true; g_cac[i].x = 130; g_cac[i].h = random(10, 20); g_prevCx[i] = 130; break; }
    g_dspawn = random(25, 50);
  }
  for (int i = 0; i < CACTUS_MAX; i++) if (g_cac[i].active) {
    g_cac[i].x -= g_dspeed;
    if (g_cac[i].x < -CACTUS_W) { g_cac[i].active = false; g_dscore++; tft.fillRect(0, 0, 40, 10, COL_BG); }
  }
  int dy = (int)g_dy;
  for (int i = 0; i < CACTUS_MAX; i++) {
    if (!g_cac[i].active) continue;
    int cx = (int)g_cac[i].x, ctop = DINO_GROUND + DINO_H - g_cac[i].h;
    if (8 + DINO_W > cx && 8 < cx + CACTUS_W && dy + DINO_H > ctop) {
      g_dover = true;
      if (g_dscore > g_dbest) g_dbest = g_dscore;
      tft.fillRect(6, 30, 116, 70, COL_BG);
      uiPutCenter(36, 2, COL_ERR, "GAME");
      uiPutCenter(56, 2, COL_ERR, "OVER");
      char b[20]; snprintf(b, sizeof(b), "score %d  best %d", g_dscore, g_dbest);
      uiPutCenter(80, 1, COL_FG, b);
      uiPutCenter(92, 1, COL_ACCENT, "OK = restart");
      return;
    }
  }
  g_dspeed = 3.0f + g_dscore * 0.12f; if (g_dspeed > 5.5f) g_dspeed = 5.5f;

  tft.fillRect(8, g_prevDy, DINO_W, DINO_H, COL_BG);
  tft.fillRect(8, dy, DINO_W, DINO_H, COL_FG);
  g_prevDy = dy;
  for (int i = 0; i < CACTUS_MAX; i++) if (g_cac[i].active) {
    int cx = (int)g_cac[i].x, px = g_prevCx[i], ctop = DINO_GROUND + DINO_H - g_cac[i].h;
    if (px != cx) tft.fillRect(px, ctop, CACTUS_W, g_cac[i].h, COL_BG);
    tft.fillRect(cx, ctop, CACTUS_W, g_cac[i].h, tft.color565(60, 180, 60));
    g_prevCx[i] = cx;
  }
  char s[8]; snprintf(s, sizeof(s), "%d", g_dscore);
  uiText(2, 2, 1, COL_WARN, COL_BG, s, g_uic[0], 5);
}
static void gameEvt() { if (g_dover && (btnClick(B_OK) || btnClick(B_UP))) gameReset(); }

// ===========================================================================
// 设备信息
// ===========================================================================
static void infoEnter() { uiHeader("DEVICE INFO"); uiHint("BACK=menu"); }
static void infoTick() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  char b[26]; int16_t y = 14;
  snprintf(b, sizeof(b), "WiFi %s %ddBm", netStateStr(), netOnline() ? WiFi.RSSI() : 0);
  uiText(2, y, 1, COL_FG, COL_BG, b, g_uic[0], 21); y += 11;
  snprintf(b, sizeof(b), "IP %s", netOnline() ? WiFi.localIP().toString().c_str() : "-");
  uiText(2, y, 1, COL_FG, COL_BG, b, g_uic[1], 21); y += 11;
  snprintf(b, sizeof(b), "SSID %.16s", WiFi.SSID().c_str());
  uiText(2, y, 1, COL_DIM, COL_BG, b, g_uic[2], 21); y += 11;
  uint32_t s = millis() / 1000;
  snprintf(b, sizeof(b), "Up %lud%luh%lum", (unsigned long)(s / 86400), (unsigned long)((s % 86400) / 3600), (unsigned long)((s % 3600) / 60));
  uiText(2, y, 1, COL_FG, COL_BG, b, g_uic[3], 21); y += 11;
  snprintf(b, sizeof(b), "Heap %uKB min%uKB", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024));
  uiText(2, y, 1, COL_OK, COL_BG, b, g_uic[4], 21); y += 11;
  snprintf(b, sizeof(b), "App %uKB/%uKB", (unsigned)(ESP.getSketchSize() / 1024),
           (unsigned)((ESP.getSketchSize() + ESP.getFreeSketchSpace()) / 1024));
  uiText(2, y, 1, COL_FG, COL_BG, b, g_uic[5], 21); y += 11;
  snprintf(b, sizeof(b), "FS %uK/%uK %ui %uv", (unsigned)(fsUsed() / 1024), (unsigned)(fsTotal() / 1024), g_imgN, g_vidN);
  uiText(2, y, 1, COL_ACCENT, COL_BG, b, g_uic[6], 21); y += 11;
  snprintf(b, sizeof(b), "SDK %.10s %dMHz", ESP.getSdkVersion(), ESP.getCpuFreqMHz());
  uiText(2, y, 1, COL_DIM, COL_BG, b, g_uic[7], 21); y += 11;
  uiText(2, y, 1, COL_DIM, COL_BG, "TFT 5/2/4 BTN 32/33/25/26", g_uic[8], 21);
}
static void infoEvt() {}

// ===========================================================================
// 设置
// ===========================================================================
enum SetRow : uint8_t { SR_CITY, SR_STOCK, SR_BRIGHT, SR_CYCLE, SR_CYCSEC, SR_REFRESH,
                        SR_WIFI, SR_CLRMEMO, SR_FORMAT, SR_REBOOT, SR_N };
static uint8_t  g_setSel = 0, g_setTop = 0;
static bool     g_setEdit = false;
static uint32_t g_setConfirm = 0;
static const uint8_t SET_VIS = 7;

static void setRowText(uint8_t r, char* out, size_t cap) {
  switch (r) {
    case SR_CITY:    snprintf(out, cap, "City   %.12s", g_set.city); break;
    case SR_STOCK:   snprintf(out, cap, "Index  %.12s", STOCK_NAMES[g_set.stockIdx % STOCK_COUNT]); break;
    case SR_BRIGHT:
#if CFG_BL_PIN >= 0
      snprintf(out, cap, "Light  %u", g_set.brightness); break;
#else
      snprintf(out, cap, "Light  n/a(BLK 3V3)"); break;
#endif
    case SR_CYCLE:   snprintf(out, cap, "Cycle  %s", g_set.autoCycle ? "ON" : "OFF"); break;
    case SR_CYCSEC:  snprintf(out, cap, "Cyc s  %u", g_set.cycleSec); break;
    case SR_REFRESH: snprintf(out, cap, "Refr m %u", g_set.refreshMin); break;
    case SR_WIFI:    snprintf(out, cap, "WiFi setup (hold)"); break;
    case SR_CLRMEMO: snprintf(out, cap, "Clear memo (hold)"); break;
    case SR_FORMAT:  snprintf(out, cap, "Format FS (hold)"); break;
    default:         snprintf(out, cap, "Reboot (hold)"); break;
  }
}
static void setDraw() {
  tft.fillRect(0, 12, SCR_W, 106, COL_BG);
  if (g_setSel >= SET_VIS) g_setTop = g_setSel - SET_VIS + 1; else g_setTop = 0;
  for (uint8_t v = 0; v < SET_VIS && g_setTop + v < SR_N; v++) {
    uint8_t r = g_setTop + v; int16_t y = 14 + v * 14;
    bool sel = (r == g_setSel);
    if (sel) tft.fillRect(2, y - 1, 124, 12, g_setEdit ? tft.color565(0, 90, 0) : COL_SEL);
    char t[26], b[28];
    setRowText(r, t, sizeof(t));
    snprintf(b, sizeof(b), "%c%s", sel ? (g_setEdit ? '*' : '>') : ' ', t);
    uiPut(4, y, 1, sel ? COL_WARN : COL_FG, b);
  }
  uiHint(g_setEdit ? "U/D=change OK=save" : "OK=edit  BACK=menu");
}
static void setEnter() { uiHeader("SETTINGS"); g_setEdit = false; g_setConfirm = 0; setDraw(); }
static void setTick()  { netBadge(); }
static void setCityDone(const char* s) {
  if (s && s[0]) { strncpy(g_set.city, s, sizeof(g_set.city) - 1); g_set.city[sizeof(g_set.city)-1]=0; setSave(); netRequest(JOB_WEATHER); setDraw(); }
}
void netEnterAPFromUI();
static void setEvt() {
  if (!g_setEdit) {
    if (btnStep(B_UP))   { g_setSel = (g_setSel + SR_N - 1) % SR_N; g_setConfirm = 0; setDraw(); }
    if (btnStep(B_DOWN)) { g_setSel = (g_setSel + 1) % SR_N;        g_setConfirm = 0; setDraw(); }
    if (btnClick(B_OK)) {
      if (g_setSel == SR_CITY) { kbOpen("City name", g_set.city, setCityDone); return; }
      if (g_setSel >= SR_WIFI) { twoStepHint("hold OK to confirm"); return; }
      g_setEdit = true; setDraw();
    }
    if (btnLong(B_OK) && g_setSel >= SR_WIFI) {
      if (g_setConfirm && millis() - g_setConfirm < 3000) {
        g_setConfirm = 0;
        switch (g_setSel) {
          case SR_WIFI:    netClearCreds(); uiToast("rebooting to AP", COL_WARN); delay(400); ESP.restart(); break;
          case SR_CLRMEMO: memoClear(); uiToast("memo cleared", COL_OK); break;
          case SR_FORMAT:  vidClose(); uiToast("formatting...", COL_WARN); LittleFS.format(); storeInit(); g_imgCur = -1; g_vidCur = -1; setDraw(); break;
          default:         uiToast("rebooting", COL_WARN); delay(300); ESP.restart(); break;
        }
      } else { g_setConfirm = millis(); twoStepHint("hold again to CONFIRM"); }
    }
    return;
  }
  // 编辑模式
  int8_t d = btnStep(B_UP) ? +1 : (btnStep(B_DOWN) ? -1 : 0);
  if (d) {
    switch (g_setSel) {
      case SR_STOCK:   g_set.stockIdx = (uint8_t)((g_set.stockIdx + STOCK_COUNT + d) % STOCK_COUNT); break;
      case SR_BRIGHT:  { int v = g_set.brightness + d * 15; if (v < 15) v = 15; if (v > 255) v = 255; g_set.brightness = (uint8_t)v; uiBacklight(g_set.brightness); } break;
      case SR_CYCLE:   g_set.autoCycle = !g_set.autoCycle; break;
      case SR_CYCSEC:  { int v = g_set.cycleSec + d * 5; if (v < 5) v = 5; if (v > 120) v = 120; g_set.cycleSec = (uint16_t)v; } break;
      case SR_REFRESH: { int v = g_set.refreshMin + d * 5; if (v < 5) v = 5; if (v > 180) v = 180; g_set.refreshMin = (uint16_t)v; } break;
      default: break;
    }
    setDraw();
  }
  if (btnClick(B_OK)) { g_setEdit = false; setSave(); if (g_setSel == SR_STOCK) netRequest(JOB_STOCK); setDraw(); }
}

// ===========================================================================
// 屏幕键盘 (模态页)
//
// 老键盘的问题: BACK 既是退格又是退出，非空时只能退格，所以"确认"这条路
// 根本不存在，回调永远收到空串。现在职责分清:
//   U/D 选字符(长按连续) / OK 短按追加 / OK 双击空格 / OK 长按 保存并返回
//   BACK 短按 退格 / BACK 长按 放弃并返回 (中央逃生通道)
// ===========================================================================
static const char KB_CHARS[] =
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:-_/()+#@";
static const uint8_t KB_N = sizeof(KB_CHARS) - 1;
static char  g_kbBuf[MEMO_TEXT_LEN];
static uint8_t g_kbLen = 0, g_kbCur = 0;
static char  g_kbTitle[18] = "";
static void (*g_kbDone)(const char*) = nullptr;

void kbOpen(const char* title, const char* init, void (*done)(const char*)) {
  strncpy(g_kbTitle, title, sizeof(g_kbTitle) - 1); g_kbTitle[sizeof(g_kbTitle) - 1] = 0;
  g_kbBuf[0] = 0; g_kbLen = 0;
  if (init) { strncpy(g_kbBuf, init, sizeof(g_kbBuf) - 1); g_kbBuf[sizeof(g_kbBuf) - 1] = 0; g_kbLen = strlen(g_kbBuf); }
  g_kbCur = 0; g_kbDone = done;
  pushPage(P_KEYBOARD);
}
static void kbEnter() {
  uiHeader(g_kbTitle);
  uiPut(4, 88,  1, COL_DIM, "OK=add  2xOK=space");
  uiPut(4, 100, 1, COL_DIM, "hold OK = SAVE");
  uiPut(4, 112, 1, COL_DIM, "BACK=del hold=cancel");
}
static void kbDraw() {
  char b[MEMO_TEXT_LEN + 2];
  snprintf(b, sizeof(b), "%s_", g_kbBuf);
  uiText(2, 18, 1, COL_ACCENT, COL_BG, b, g_uic[0], 21);
  char c[2] = { KB_CHARS[g_kbCur], 0 };
  // 左中右三个字符，中间是当前选择
  char l[2] = { KB_CHARS[(g_kbCur + KB_N - 1) % KB_N], 0 };
  char r[2] = { KB_CHARS[(g_kbCur + 1) % KB_N], 0 };
  uiText(34, 52, 2, COL_DIM,  COL_BG, l, g_uic[1], 1);
  uiText(58, 46, 3, COL_WARN, COL_BG, c, g_uic[2], 1);
  uiText(86, 52, 2, COL_DIM,  COL_BG, r, g_uic[3], 1);
}
static void kbTick() { static uint32_t last = 0; if (millis() - last > 60) { last = millis(); kbDraw(); } }
static void kbClose(bool save) {
  void (*cb)(const char*) = g_kbDone;
  g_kbDone = nullptr;
  popPage();
  if (save && cb) cb(g_kbBuf);
  else if (save && !cb) { }
}
static void kbEvt() {
  if (btnStep(B_UP))   g_kbCur = (g_kbCur + KB_N - 1) % KB_N;
  if (btnStep(B_DOWN)) g_kbCur = (g_kbCur + 1) % KB_N;
  if (btnClick(B_OK) && g_kbLen < sizeof(g_kbBuf) - 1) { g_kbBuf[g_kbLen++] = KB_CHARS[g_kbCur]; g_kbBuf[g_kbLen] = 0; }
  if (btnDouble(B_OK) && g_kbLen < sizeof(g_kbBuf) - 1) { g_kbBuf[g_kbLen++] = ' '; g_kbBuf[g_kbLen] = 0; }
  if (btnLong(B_OK)) { kbClose(true); return; }
  if (btnClick(B_BACK) && g_kbLen) { g_kbBuf[--g_kbLen] = 0; g_uic[0][0] = 0; tft.fillRect(0, 18, SCR_W, 9, COL_BG); }
}

// ===========================================================================
// AP 配网提示页
// ===========================================================================
static void apEnter() {
  uiHeader("WIFI SETUP", COL_WARN);
  uiPutCenter(24, 1, COL_FG,     "Connect to hotspot");
  uiPutCenter(38, 1, COL_ACCENT, CFG_AP_SSID);
  uiPutCenter(54, 1, COL_DIM,    "password");
  uiPutCenter(66, 1, COL_ACCENT, strlen(CFG_AP_PASS) ? CFG_AP_PASS : "(open)");
  uiPutCenter(86, 1, COL_FG,     "then open");
  uiPutCenter(98, 1, COL_OK,     "http://192.168.4.1");
}
static void apTick() {}
static void apEvt()  {}

// ===========================================================================
// 页面表 + 中央调度
// ===========================================================================
static const PageDef PAGES[P_COUNT] = {
  /* P_MENU     */ {"Menu",     menuEnter,  menuTick,  menuEvt,  false},
  /* P_CLOCK    */ {"Clock",    clockEnter, clockTick, clockEvt, false},
  /* P_WEATHER  */ {"Weather",  wxEnter,    wxTick,    wxEvt,    false},
  /* P_STOCK    */ {"Stock",    stkEnter,   stkTick,   stkEvt,   false},
  /* P_MEMO     */ {"Memo",     memoEnter,  memoTick,  memoEvt,  false},
  /* P_IMAGE    */ {"Image",    imgEnter,   imgTick,   imgEvt,   false},
  /* P_VIDEO    */ {"Video",    vidEnter,   vidPageTick, vidEvt, false},
  /* P_SETTINGS */ {"Settings", setEnter,   setTick,   setEvt,   false},
  /* P_TIMER    */ {"Timer",    tmEnter,    tmTick,    tmEvt,    false},
  /* P_FACE     */ {"Face",     faceEnter,  faceTick,  faceEvt,  false},
  /* P_SMOKE    */ {"Smoke",    smokeEnter, smokeTick, smokeEvt, false},
  /* P_FOOTBALL */ {"Football", fbEnter,    fbTick,    fbEvt,    false},
  /* P_GAME     */ {"Game",     gameEnter,  gameTick,  gameEvt,  false},
  /* P_INFO     */ {"Info",     infoEnter,  infoTick,  infoEvt,  false},
  /* P_KEYBOARD */ {"Keyboard", kbEnter,    kbTick,    kbEvt,    true },
  /* P_APINFO   */ {"AP",       apEnter,    apTick,    apEvt,    false},
};

void gotoPage(PageId p) {
  if (p >= P_COUNT) p = P_MENU;
  // 离开视频页要收拾干净，否则文件句柄和 TCP 连接会一直挂着
  if (g_page == P_VIDEO && p != P_VIDEO) { vidClose(); liveDisconnect(); }
  g_page = p;
  g_pageT0 = millis();
  tft.fillScreen(COL_BG);
  uiCacheReset();
  if (PAGES[p].enter) PAGES[p].enter();
  btnConsumeAll();          // 别让触发跳转的那一次按键被新页面再吃一遍
}

void gotoMenuHard() { g_sp = 0; g_kbDone = nullptr; gotoPage(P_MENU); }

// 主循环调用。整个函数不含任何 delay。
void appTick() {
  btnPoll();

  // ---- 逃生通道: 长按 BACK 无条件回主菜单，页面拿不到这个事件 ----
  if (btnLong(B_BACK)) {
    if (g_page != P_APINFO) { gotoMenuHard(); return; }
  }
  // ---- 短按 BACK: 出栈 / 回主菜单。ownsBack 的页面自己处理 ----
  if (btnClick(B_BACK) && !PAGES[g_page].ownsBack) {
    if (g_page == P_MENU) { /* 已经在主菜单，什么都不做 */ }
    else if (g_page == P_APINFO) { }
    else if (g_sp > 0) { popPage(); return; }
    else { gotoPage(P_MENU); return; }
  }

  if (PAGES[g_page].evt)  PAGES[g_page].evt();
  if (PAGES[g_page].tick) PAGES[g_page].tick();

  // ---- web / 推流触发的换页 ----
  if (g_pending != P_COUNT) { PageId p = g_pending; g_pending = P_COUNT; gotoPage(p); }

  // ---- 有人连上视频推流端口就自动切到视频页 ----
  if (g_page != P_VIDEO && g_vsrvUp && g_vsrv.hasClient()) { g_vm = VM_LIVE; gotoPage(P_VIDEO); }

  // ---- 主页自动轮播 (设置里开) ----
  if (g_set.autoCycle && g_sp == 0 && btnIdleMs() > 30000) {
    static const PageId CYC[] = {P_CLOCK, P_WEATHER, P_STOCK, P_MEMO, P_FACE};
    static uint8_t ci = 0;
    static uint32_t ct = 0;
    bool inCyc = false;
    for (uint8_t i = 0; i < sizeof(CYC) / sizeof(CYC[0]); i++) if (g_page == CYC[i]) { inCyc = true; ci = i; }
    if (inCyc && millis() - ct > (uint32_t)g_set.cycleSec * 1000UL) {
      ct = millis();
      ci = (ci + 1) % (sizeof(CYC) / sizeof(CYC[0]));
      gotoPage(CYC[ci]);
    } else if (!inCyc) ct = millis();
  }
}
