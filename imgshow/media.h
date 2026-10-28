// ============================================================================
// media.h  —  图片显示 + 视频播放
//
// ## 图片
// 128x128 RGB565 裸数据，一张固定 32768 字节，存 LittleFS 的 /img/ 下。
// 2MB 分区能放约 60 张。播放时按 512 字节分块从 flash 读，直接推给面板，
// 峰值 RAM 只有几百字节 (老代码为此常驻 32KB 缓冲)。
//
// ## 视频 —— 先把话说清楚
// 「ESP32 + 128x128 屏幕播放视频」这件事的真实边界:
//   * SPI 不是瓶颈。40MHz 下一帧 32768 字节 = 6.6ms，理论 150fps。
//   * 存储是死结。板载 4MB flash 扣掉 2MB 程序，LittleFS 最多 2MB。
//     128x128 全彩一帧 32KB  ->  2MB 只够 61 帧  ->  15fps 下 4 秒。
//     结论: 128x128 全分辨率的"存卡里的视频"在这块板子上不现实，不用试。
//   * LittleFS 顺序读实测 1~2MB/s，32KB 一帧要 20~30ms，
//     所以就算存得下，也只有 30fps 上限。
//
// 所以这里实现两条真能跑的路:
//   方案 A (离线/本地播放): 64x64 RGB565 + 2 倍像素放大铺满 128x128，10fps。
//        一帧 8192 字节 -> 2MB 能放 240 帧 -> 约 24 秒，循环播放。
//        画质是马赛克风，但流畅、不用手机、断电还在。适合做动图/表情/LOGO 动画。
//   方案 B (在线/实时推流): 电脑或手机通过裸 TCP 长连接连到 8888 端口，
//        背靠背发 128x128 LE RGB565 裸帧。ESP32 TCP 收包实测 0.8~1.2MB/s，
//        => 128x128 全分辨率 20~30fps，长度无限。这条才是"真能看视频"的路。
//        老代码是每帧一个 HTTP multipart POST，光协议开销就压到 5fps 左右，
//        而且每帧都要新建 TCP 连接，堆很容易碎。
//
// 想要长时间 + 全分辨率 + 离线，唯一出路是外挂 SD 卡 (见 README 后续改进)。
// GIF 也是个选项，但要多引入 AnimatedGIF 库和 ~40KB flash，本工程没做，
// README 里给了做法。
// ============================================================================
#pragma once
#include <LittleFS.h>
#include <WiFiServer.h>
#include "ui.h"
#include "store.h"
#include "config.h"

// ---------------------------------------------------------------------------
// 图片
// ---------------------------------------------------------------------------
static int8_t g_imgCur = -1;

bool imgDrawFile(const char* name) {
  if (!g_fsOK) return false;
  char path[40]; snprintf(path, sizeof(path), "/img/%s", name);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  if (f.size() < IMG_BYTES) { f.close(); return false; }

  static uint8_t chunk[512];
  TftStream ts;
  ts.begin(0, 0, SCR_W, SCR_H);
  uint32_t left = IMG_BYTES;
  while (left) {
    size_t want = left > sizeof(chunk) ? sizeof(chunk) : left;
    size_t got  = f.read(chunk, want);
    if (!got) break;
    ts.push(chunk, got);
    left -= got;
  }
  ts.end();
  f.close();
  return true;
}

// 显示第 i 张(带越界回绕)。返回是否成功
bool imgShow(int8_t i) {
  if (g_imgN == 0) return false;
  if (i < 0) i = g_imgN - 1;
  if (i >= (int8_t)g_imgN) i = 0;
  g_imgCur = i;
  return imgDrawFile(g_imgName[i]);
}

bool imgDelete(const char* name) {
  char path[40]; snprintf(path, sizeof(path), "/img/%s", name);
  bool ok = LittleFS.remove(path);
  if (ok) { fsRescan(); if (g_imgCur >= (int8_t)g_imgN) g_imgCur = g_imgN ? 0 : -1; }
  return ok;
}

// ---------------------------------------------------------------------------
// 视频文件: 16 字节头 + 连续裸帧
//   0..3  magic "IV1\0"
//   4..5  uint16 w        (64 或 128)
//   6..7  uint16 h
//   8..9  uint16 fps
//  10..11 uint16 frames
//  12..15 保留
// ---------------------------------------------------------------------------
struct VidHdr { char magic[4]; uint16_t w, h, fps, frames; uint32_t rsv; };

static File     g_vf;
static VidHdr   g_vh;
static bool     g_vidOpen  = false;
static bool     g_vidPause = false;
static uint16_t g_vidFrame = 0;
static uint32_t g_vidNextMs = 0;
static uint8_t  g_vidScale = 1;
static int16_t  g_vidX = 0, g_vidY = 0;
static int8_t   g_vidCur = -1;

void vidClose() {
  if (g_vidOpen) { g_vf.close(); g_vidOpen = false; }
  g_vidFrame = 0; g_vidPause = false;
}

bool vidOpen(int8_t i) {
  vidClose();
  if (!g_fsOK || g_vidN == 0) return false;
  if (i < 0) i = g_vidN - 1;
  if (i >= (int8_t)g_vidN) i = 0;
  g_vidCur = i;

  char path[40]; snprintf(path, sizeof(path), "/vid/%s", g_vidName[i]);
  g_vf = LittleFS.open(path, "r");
  if (!g_vf) return false;
  if (g_vf.read((uint8_t*)&g_vh, sizeof(g_vh)) != sizeof(g_vh)) { g_vf.close(); return false; }
  if (memcmp(g_vh.magic, "IV1", 3) != 0) { g_vf.close(); return false; }
  if (g_vh.w == 0 || g_vh.h == 0 || g_vh.w > 128 || g_vh.h > 128) { g_vf.close(); return false; }
  if (g_vh.fps == 0 || g_vh.fps > 40) g_vh.fps = 10;

  g_vidScale = (128 / g_vh.w >= 2 && 128 / g_vh.h >= 2) ? 2 : 1;
  g_vidX = (SCR_W - g_vh.w * g_vidScale) / 2;
  g_vidY = (SCR_H - g_vh.h * g_vidScale) / 2;
  // 帧数没写对也能放: 用文件长度反推
  uint32_t frameBytes = (uint32_t)g_vh.w * g_vh.h * 2;
  uint32_t avail = (g_vf.size() > sizeof(g_vh)) ? (g_vf.size() - sizeof(g_vh)) : 0;
  uint16_t real = frameBytes ? (uint16_t)(avail / frameBytes) : 0;
  if (g_vh.frames == 0 || g_vh.frames > real) g_vh.frames = real;
  if (g_vh.frames == 0) { g_vf.close(); return false; }

  g_vidOpen = true; g_vidFrame = 0; g_vidNextMs = millis();
  tft.fillScreen(COL_BG);
  return true;
}

// 画一帧。~15~30ms，期间不响应按钮，但两帧之间有空隙，按钮延迟感觉不到。
static void vidRenderFrame() {
  static uint8_t  raw[256];
  static uint16_t line[128];
  static uint16_t line2[128];
  uint16_t w = g_vh.w, h = g_vh.h, sc = g_vidScale;

  tft.startWrite();
  tft.setAddrWindow(g_vidX, g_vidY, w * sc, h * sc);
  for (uint16_t r = 0; r < h; r++) {
    if (g_vf.read(raw, w * 2) != (int)(w * 2)) break;
    for (uint16_t i = 0; i < w; i++) line[i] = (uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8);
    if (sc == 1) {
      tft.writePixels(line, w, true, false);
    } else {
      for (uint16_t i = 0; i < w; i++) { line2[i * 2] = line[i]; line2[i * 2 + 1] = line[i]; }
      tft.writePixels(line2, w * 2, true, false);   // 同一行推两遍 = 垂直放大
      tft.writePixels(line2, w * 2, true, false);
    }
  }
  tft.endWrite();
}

// 每个 loop 调，非阻塞。返回 true 表示这一轮画了帧
bool vidTick() {
  if (!g_vidOpen || g_vidPause) return false;
  uint32_t now = millis();
  if ((int32_t)(now - g_vidNextMs) < 0) return false;
  uint32_t period = 1000UL / g_vh.fps;
  g_vidNextMs += period;
  // 落后太多就丢帧对齐，避免越播越慢
  if ((int32_t)(now - g_vidNextMs) > (int32_t)(period * 3)) g_vidNextMs = now + period;

  vidRenderFrame();
  g_vidFrame++;
  if (g_vidFrame >= g_vh.frames) {          // 循环
    g_vidFrame = 0;
    g_vf.seek(sizeof(VidHdr));
  }
  return true;
}

bool vidDelete(const char* name) {
  vidClose();
  char path[40]; snprintf(path, sizeof(path), "/vid/%s", name);
  bool ok = LittleFS.remove(path);
  if (ok) { fsRescan(); g_vidCur = g_vidN ? 0 : -1; }
  return ok;
}

// ---------------------------------------------------------------------------
// 方案 B: 裸 TCP 实时推流
// 客户端连上后背靠背发 128*128*2 = 32768 字节的 LE RGB565 裸帧即可。
// 发送端参考 tools/stream_video.py
// ---------------------------------------------------------------------------
static WiFiServer  g_vsrv(CFG_VIDEO_TCP_PORT);
static WiFiClient  g_vcli;
static TftStream   g_vts;
static bool        g_vsrvUp   = false;
static uint32_t    g_liveFrames = 0;
static uint32_t    g_liveT0     = 0;
static uint8_t     g_liveFps    = 0;

void liveBegin() {
  if (g_vsrvUp) return;
  g_vsrv.begin();
  g_vsrv.setNoDelay(true);
  g_vsrvUp = true;
}

inline bool liveConnected() { return g_vcli && g_vcli.connected(); }
inline uint8_t liveFps()    { return g_liveFps; }

// 非阻塞: 只搬当前可读的字节
void liveTick() {
  if (!g_vsrvUp) return;
  if (!liveConnected()) {
    WiFiClient c = g_vsrv.available();
    if (c) {
      if (g_vcli) g_vcli.stop();
      g_vcli = c; g_vcli.setNoDelay(true);
      g_vts.begin(0, 0, SCR_W, SCR_H);
      g_liveFrames = 0; g_liveT0 = millis(); g_liveFps = 0;
    }
    return;
  }
  static uint8_t buf[1024];
  int guard = 0;                     // 一轮最多搬 16KB，保证按钮还能被轮询到
  while (g_vcli.available() && guard < 16) {
    int n = g_vcli.read(buf, sizeof(buf));
    if (n <= 0) break;
    g_vts.push(buf, n);
    if (g_vts.complete()) {          // 一帧满了，重开窗口接下一帧
      g_vts.end();
      g_vts.begin(0, 0, SCR_W, SCR_H);
      g_liveFrames++;
      uint32_t dt = millis() - g_liveT0;
      if (dt >= 1000) { g_liveFps = (uint8_t)(g_liveFrames * 1000UL / dt); g_liveFrames = 0; g_liveT0 = millis(); }
    }
    guard++;
  }
}

void liveDisconnect() {
  if (g_vcli) g_vcli.stop();
  if (g_vts.active()) g_vts.end();
  g_liveFps = 0;
}
