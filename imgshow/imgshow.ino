// ============================================================================
// imgshow.ino  —  ESP32 + ST7735 多功能桌面摆件 (主文件)
//
// 这个文件刻意写得很薄: 只做初始化和调度。功能都在同目录的头文件里:
//   config.h    所有可配置项/密钥 (不进 git)
//   ui.h        屏幕 + 差分绘图 + RGB565 流式推屏
//   buttons.h   4 按钮事件层 (消抖/短按/长按/双击/连发，全非阻塞)
//   store.h     NVS(设置/备忘) + LittleFS(图片/视频) + 数据模型
//   net.h       WiFi 状态机 + core0 后台 HTTP 任务 (UI 永不因网络卡住)
//   media.h     图片显示 + 视频文件播放 + TCP 实时推流
//   app.h       页面状态机 + 各功能页
//   web.h       手机配网页 + 控制台 + 上传
//
// ⚠️ 编译前必须选:  分区方案 = "No OTA (2MB APP/2MB SPIFFS)"
//    默认分区只有 1.2MB APP，装不下；而且拿不到 2MB LittleFS。
//
// loop() 里没有任何 delay()，所以按钮响应始终 < 30ms。
// ============================================================================
#include "config.h"

#if CFG_DISABLE_BROWNOUT
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
#endif

#include "ui.h"
#include "buttons.h"
#include "store.h"
#include "net.h"
#include "media.h"
#include "app.h"
#include "web.h"

// 天气/股票的定时自动刷新
static uint32_t g_autoRefT = 0;

void setup() {
#if CFG_DISABLE_BROWNOUT
  // 屏幕+WiFi 同时开时电流尖峰容易触发欠压复位。USB 供电不稳的话必须关。
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  uiInit();
  uiPutCenter(56, 1, COL_ACCENT, "imgshow booting");

  btnInit();
  storeInit();                 // LittleFS + 读设置/备忘
  uiBacklight(g_set.brightness);

  netInit();                   // 非阻塞: 起后台任务，发起连接就返回

  gotoPage(P_CLOCK);           // 开机停在时钟页(没网也能看)
  g_autoRefT = millis();
}

void loop() {
  webTick();                   // HTTP 轮询
  netTick();                   // WiFi 状态机 + NTP
  appTick();                   // 按钮 + 当前页面
  buzTick();                   // 蜂鸣器到时自动关

  // 定时刷新: 只有联网时才排队
  uint32_t period = (uint32_t)g_set.refreshMin * 60000UL;
  if (netOnline() && millis() - g_autoRefT > period) {
    g_autoRefT = millis();
    netRequest(JOB_WEATHER);
    netRequest(JOB_STOCK);
  }
  // 不 delay: 视频页/推流页要满速跑，其他页面事件驱动，空转开销可忽略
}
