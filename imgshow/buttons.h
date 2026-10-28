// ============================================================================
// buttons.h  —  非阻塞按钮事件层
//
// 修好了老代码的两个硬伤:
//  1. 老代码用一个全局 200ms 锁 (lastBtnEvent) —— 按任意一个键会把其他三个键
//     一起锁死，而且锁期内 prevBtnX 不更新，锁期内的松手+再按会被整段吞掉。
//     现在每个键独立消抖，互不影响。
//  2. 老代码只有"按下沿"，没有长按/双击。README 里写的 "Hold BACK=confirm"
//     根本没实现。现在真的有了。
//
// 事件语义:
//   EV_CLICK   —— 短按松手
//   EV_DOUBLE  —— 双击 (只在 wantDouble=true 的键上启用，避免给 UP/DOWN 引入延迟)
//   EV_LONG    —— 按住超过 LONG_MS，只触发一次
//   EV_REPEAT  —— EV_LONG 之后每 REPEAT_MS 触发一次 (UP/DOWN 快速滚动用)
// ============================================================================
#pragma once
#include <Arduino.h>

#define PIN_BTN_UP    32
#define PIN_BTN_DOWN  33
#define PIN_BTN_OK    25
#define PIN_BTN_BACK  26

enum BtnId  : uint8_t { B_UP = 0, B_DOWN, B_OK, B_BACK, B_COUNT };
enum BtnEvt : uint8_t { EV_NONE = 0, EV_CLICK, EV_DOUBLE, EV_LONG, EV_REPEAT };

static const uint16_t BTN_DEBOUNCE_MS = 20;    // 机械抖动窗口
static const uint16_t BTN_LONG_MS     = 700;   // 长按判定
static const uint16_t BTN_REPEAT_MS   = 140;   // 长按后自动重复间隔
static const uint16_t BTN_DBL_MS      = 230;   // 双击窗口

struct BtnState {
  uint8_t  pin;
  bool     wantDouble;   // 是否启用双击(会让单击延迟 BTN_DBL_MS，UP/DOWN 关掉)
  bool     rawLast;      // 上次原始读数(true=按下)
  bool     stable;       // 消抖后的稳定状态
  uint32_t rawChangeMs;
  uint32_t pressMs;
  uint32_t releaseMs;
  uint32_t lastRepeatMs;
  bool     longFired;
  bool     clickPending; // 等双击窗口
};

static BtnState g_btn[B_COUNT];
static BtnEvt   g_ev[B_COUNT];          // 本轮事件，每次 btnPoll() 重置
static uint32_t g_lastActivityMs = 0;   // 给息屏/省电用

void btnInit() {
  const uint8_t pins[B_COUNT] = { PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_OK, PIN_BTN_BACK };
  // UP/DOWN 不要双击(否则每次单击都要等 230ms，翻菜单会很黏)
  const bool dbl[B_COUNT]     = { false, false, true, false };
  for (uint8_t i = 0; i < B_COUNT; i++) {
    pinMode(pins[i], INPUT_PULLUP);
    g_btn[i] = BtnState{};
    g_btn[i].pin        = pins[i];
    g_btn[i].wantDouble = dbl[i];
  }
  g_lastActivityMs = millis();
}

// 每个 loop 调一次。执行时间 < 50us，不含任何 delay。
void btnPoll() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < B_COUNT; i++) g_ev[i] = EV_NONE;

  for (uint8_t i = 0; i < B_COUNT; i++) {
    BtnState& b = g_btn[i];
    bool raw = !digitalRead(b.pin);   // INPUT_PULLUP，按下=低

    if (raw != b.rawLast) { b.rawLast = raw; b.rawChangeMs = now; }

    // 原始读数稳定够久才认
    if (raw != b.stable && (now - b.rawChangeMs) >= BTN_DEBOUNCE_MS) {
      b.stable = raw;
      if (raw) {                       // ---- 按下 ----
        b.pressMs      = now;
        b.longFired    = false;
        b.lastRepeatMs = 0;
        g_lastActivityMs = now;
        if (b.wantDouble && b.clickPending && (now - b.releaseMs) <= BTN_DBL_MS) {
          b.clickPending = false;
          g_ev[i] = EV_DOUBLE;
        }
      } else {                         // ---- 松开 ----
        b.releaseMs = now;
        g_lastActivityMs = now;
        if (!b.longFired) {
          if (b.wantDouble) b.clickPending = true;   // 等等看是不是双击
          else              g_ev[i] = EV_CLICK;      // 立即出单击
        }
      }
    }

    // 长按 + 自动重复
    if (b.stable) {
      if (!b.longFired && (now - b.pressMs) >= BTN_LONG_MS) {
        b.longFired    = true;
        b.lastRepeatMs = now;
        g_ev[i] = EV_LONG;
      } else if (b.longFired && (now - b.lastRepeatMs) >= BTN_REPEAT_MS) {
        b.lastRepeatMs = now;
        g_ev[i] = EV_REPEAT;
      }
    }

    // 双击窗口过期 -> 补发单击
    if (b.clickPending && (now - b.releaseMs) > BTN_DBL_MS) {
      b.clickPending = false;
      if (g_ev[i] == EV_NONE) g_ev[i] = EV_CLICK;
    }
  }
}

inline BtnEvt btnEvent(BtnId id)      { return g_ev[id]; }
inline bool   btnHeld(BtnId id)       { return g_btn[id].stable; }
// UP/DOWN 常用: 单击或长按重复都算一次"步进"
inline bool   btnStep(BtnId id)       { return g_ev[id] == EV_CLICK || g_ev[id] == EV_REPEAT; }
inline bool   btnClick(BtnId id)      { return g_ev[id] == EV_CLICK; }
inline bool   btnLong(BtnId id)       { return g_ev[id] == EV_LONG; }
inline bool   btnDouble(BtnId id)     { return g_ev[id] == EV_DOUBLE; }
inline bool   btnAny() { for (uint8_t i=0;i<B_COUNT;i++) if (g_ev[i]!=EV_NONE) return true; return false; }
inline uint32_t btnIdleMs()           { return millis() - g_lastActivityMs; }
// 清掉本轮事件，用于页面切换后避免同一次按键被新页面重复消费
inline void   btnConsumeAll() { for (uint8_t i=0;i<B_COUNT;i++) g_ev[i] = EV_NONE; }
