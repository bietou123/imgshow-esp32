// ============================================================================
// ui.h  —  屏幕 + 抗闪烁绘图层
//
// 老代码每次刷新都 tft.fillScreen() 再整屏重画 (时钟每秒一次) —— 128x128 全屏
// 填黑约 6ms，然后文字一个个画上去，肉眼能看到明显闪黑。
//
// 这里的核心原语是 uiText(): 按字符跟缓存比较，只重画变化的字符格。
// 时钟走秒时只有 2 个字符格 (12x24 像素) 被重画，视觉上完全静止。
// fillScreen() 只允许在"进入页面"时调用一次。
// ============================================================================
#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include "config.h"

#define PIN_TFT_CS   5
#define PIN_TFT_DC   2
#define PIN_TFT_RST  4
// SCK=18 / MOSI=23 走 VSPI 默认脚，由 SPI 库处理，不用声明

#define SCR_W 128
#define SCR_H 128

static Adafruit_ST7735 tft = Adafruit_ST7735(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

// ---- 配色 ----
#define COL_BG      ST77XX_BLACK
#define COL_FG      ST77XX_WHITE
#define COL_DIM     0x7BEF
#define COL_ACCENT  ST77XX_CYAN
#define COL_OK      ST77XX_GREEN
#define COL_WARN    ST77XX_YELLOW
#define COL_ERR     ST77XX_RED
#define COL_SEL     0x0410      // 菜单选中条 深蓝

void buzInit();   // 蜂鸣器，实现在下面

void uiInit() {
  tft.initR(INITR_144GREENTAB);
  tft.setRotation(0);
  tft.setSPISpeed(40000000);
  tft.fillScreen(COL_BG);
#if CFG_BL_PIN >= 0
  // 背光 PWM。ESP32 core 3.x 改了 LEDC API，这里两版都兼容
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(CFG_BL_PIN, 5000, 8);
    ledcWrite(CFG_BL_PIN, 255);
  #else
    ledcSetup(0, 5000, 8);
    ledcAttachPin(CFG_BL_PIN, 0);
    ledcWrite(0, 255);
  #endif
#endif
  buzInit();
}

void uiBacklight(uint8_t duty) {
#if CFG_BL_PIN >= 0
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(CFG_BL_PIN, duty);
  #else
    ledcWrite(0, duty);
  #endif
#else
  (void)duty;
#endif
}

// ---------------------------------------------------------------------------
// 蜂鸣器(可选，CFG_BUZZER_PIN = -1 时整段是空操作)
// 非阻塞: buzBeep() 立刻返回，buzTick() 到时间自动停。没有 delay。
// ---------------------------------------------------------------------------
static uint32_t g_buzOffMs = 0;

void buzInit() {
#if CFG_BUZZER_PIN >= 0
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(CFG_BUZZER_PIN, 2000, 8);
    ledcWrite(CFG_BUZZER_PIN, 0);
  #else
    ledcSetup(1, 2000, 8);                  // 通道 1，通道 0 留给背光
    ledcAttachPin(CFG_BUZZER_PIN, 1);
    ledcWrite(1, 0);
  #endif
#endif
}

void buzBeep(uint16_t freq, uint16_t ms) {
#if CFG_BUZZER_PIN >= 0
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(CFG_BUZZER_PIN, freq);
    ledcWrite(CFG_BUZZER_PIN, 128);
  #else
    ledcWriteTone(1, freq);
    ledcWrite(1, 128);
  #endif
  g_buzOffMs = millis() + ms;
#else
  (void)freq; (void)ms;
#endif
}

void buzTick() {
#if CFG_BUZZER_PIN >= 0
  if (g_buzOffMs && (int32_t)(millis() - g_buzOffMs) >= 0) {
    g_buzOffMs = 0;
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(CFG_BUZZER_PIN, 0);
  #else
    ledcWrite(1, 0);
  #endif
  }
#endif
}

// ---------------------------------------------------------------------------
// 差分文字。cache 必须是调用方持有的静态缓冲，长度 >= cap+1
// 只重画和 cache 不同的字符格；变短时把尾巴擦掉。
// ---------------------------------------------------------------------------
void uiText(int16_t x, int16_t y, uint8_t size, uint16_t fg, uint16_t bg,
            const char* s, char* cache, uint8_t cap) {
  uint8_t cw = 6 * size, ch = 8 * size;
  uint8_t n = 0; while (s[n] && n < cap) n++;
  uint8_t old = 0; while (cache[old] && old < cap) old++;

  for (uint8_t i = 0; i < n; i++) {
    if (cache[i] != s[i]) {
      tft.drawChar(x + i * cw, y, s[i], fg, bg, size);
      cache[i] = s[i];
    }
  }
  if (old > n) {   // 擦掉多余尾巴
    tft.fillRect(x + n * cw, y, (old - n) * cw, ch, bg);
  }
  cache[n] = 0;
}

// 居中版。cap 决定居中用的宽度基准，字符串短了会自动居中且擦掉旧的残留。
void uiTextCenter(int16_t y, uint8_t size, uint16_t fg, uint16_t bg,
                  const char* s, char* cache, uint8_t cap) {
  uint8_t cw = 6 * size;
  uint8_t n = 0; while (s[n] && n < cap) n++;
  int16_t x = (SCR_W - n * cw) / 2; if (x < 0) x = 0;
  // 居中的起点随长度变化。长度一变起点就变，必须整行擦一次否则留残影；
  // 长度不变时走纯差分，不闪。
  uint8_t old = 0; while (cache[old] && old < cap) old++;
  if (old != n) {
    tft.fillRect(0, y, SCR_W, 8 * size, bg);
    cache[0] = 0;
  }
  for (uint8_t i = 0; i < n; i++) {
    if (cache[i] != s[i]) { tft.drawChar(x + i * cw, y, s[i], fg, bg, size); cache[i] = s[i]; }
  }
  cache[n] = 0;
}

// 一次性文字(不做差分)，用于进入页面时画静态框架
void uiPut(int16_t x, int16_t y, uint8_t size, uint16_t fg, const char* s) {
  tft.setTextSize(size); tft.setTextColor(fg); tft.setCursor(x, y); tft.print(s);
}
void uiPutCenter(int16_t y, uint8_t size, uint16_t fg, const char* s) {
  int16_t w = strlen(s) * 6 * size;
  uiPut((SCR_W - w) / 2, y, size, fg, s);
}

// 页面标题条(进入页面画一次)
void uiHeader(const char* title, uint16_t col = COL_ACCENT) {
  tft.fillRect(0, 0, SCR_W, 11, COL_BG);
  uiPutCenter(2, 1, col, title);
  tft.drawFastHLine(0, 10, SCR_W, 0x2104);
}

// 底部提示条(进入页面画一次)
void uiHint(const char* s) {
  tft.fillRect(0, 119, SCR_W, 9, COL_BG);
  uiPutCenter(120, 1, COL_DIM, s);
}

// 加载中提示，覆盖屏幕中间一条，不整屏清
void uiToast(const char* s, uint16_t col = COL_WARN) {
  tft.fillRect(0, 56, SCR_W, 16, COL_BG);
  uiPutCenter(60, 1, col, s);
}

// ---------------------------------------------------------------------------
// TftStream —— 把一串 little-endian RGB565 字节流直接推到面板
//
// 老代码开了 static uint8_t imgBuf[32768]，白占 32KB RAM (总量的 10%)，
// 而且必须收满整帧才能画。现在收到多少推多少，RAM 占用降到 128 字节，
// HTTP 分块长度是奇数也能正确处理(carry 一个半字节)。
// ---------------------------------------------------------------------------
class TftStream {
public:
  void begin(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (_active) end();
    _total = (uint32_t)w * h; _done = 0; _bufN = 0; _haveCarry = false; _active = true;
    tft.startWrite();
    tft.setAddrWindow(x, y, w, h);
  }
  void push(const uint8_t* d, size_t n) {
    if (!_active) return;
    size_t i = 0;
    if (_haveCarry && n > 0) {
      _buf[_bufN++] = (uint16_t)_carry | ((uint16_t)d[0] << 8);
      _haveCarry = false; i = 1;
      if (_bufN >= BUFPX) flush();
    }
    for (; i + 1 < n; i += 2) {
      _buf[_bufN++] = (uint16_t)d[i] | ((uint16_t)d[i + 1] << 8);
      if (_bufN >= BUFPX) flush();
    }
    if (i < n) { _carry = d[i]; _haveCarry = true; }
  }
  void end() {
    if (!_active) return;
    flush();
    tft.endWrite();
    _active = false;
  }
  bool complete() const { return _done >= _total; }
  bool active()   const { return _active; }
  uint32_t done() const { return _done; }
private:
  static const uint16_t BUFPX = 64;
  void flush() {
    if (_bufN) {
      uint16_t n = _bufN;
      if (_done + n > _total) n = (uint16_t)(_total - _done);
      if (n) { tft.writePixels(_buf, n, true, false); _done += n; }
      _bufN = 0;
    }
  }
  uint16_t _buf[BUFPX];
  uint16_t _bufN = 0;
  uint32_t _total = 0, _done = 0;
  uint8_t  _carry = 0;
  bool     _haveCarry = false, _active = false;
};
