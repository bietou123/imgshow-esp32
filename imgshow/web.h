// ============================================================================
// web.h  —  配网页 + 手机控制台 + 上传
//
// 相比老代码的改动:
//  * 上传图片不再经过 static uint8_t imgBuf[32768]。改成在 upload 回调里
//    边收边写 LittleFS，RAM 峰值 = HTTP 分块大小(~1.4KB)。
//  * 图片转 RGB565 全在手机浏览器的 canvas 里做，ESP32 只负责存字节。
//    所以手机相册里任意 jpg/png/heic(浏览器能解就行) 都能直接传。
//  * 加了 HTTP Basic 认证 (config.h 里 CFG_WEB_USER/PASS)。老代码任何局域网
//    设备都能调 /reset 把 WiFi 配置擦掉 —— 这是本项目最实际的安全问题。
//  * AP 热点加了密码。老代码 softAP 是开放的，谁都能连上来改配置。
//  * 备忘录改成手机网页输入为主(屏幕键盘做备份手段)：4 个按钮打字太痛苦。
//
// 端点一览:
//   GET  /                 配网页(AP模式) 或 控制台(STA模式)
//   GET  /scan             扫描 WiFi -> JSON
//   POST /save             保存凭据并重启
//   GET  /status           运行状态 JSON
//   POST /upimg            上传 128x128 RGB565 (32768B) -> /img/xxx.565
//   POST /upvid            上传 IV1 视频     -> /vid/xxx.v64
//   POST /frame            推一帧直接上屏(浏览器投屏用，慢，真投屏走 TCP 8888)
//   GET  /files /del       文件列表 / 删除
//   GET  /memo             ?add= / ?del= / ?clear=1
//   GET  /timer            ?min=&name=
//   GET  /page             ?p=  跳页
//   GET  /refresh          ?w=1&s=1&f=1 触发后台抓取
//   GET  /cfg              ?city=&stk=&bri=&cyc=&cycs=&rfm=
//   GET  /reset  /reboot   清 WiFi 配置 / 重启
// ============================================================================
#pragma once
#include <WebServer.h>
#include <LittleFS.h>
#include "config.h"
#include "store.h"
#include "net.h"
#include "media.h"
#include "app.h"

static WebServer server(80);
static bool g_webUp = false;

// ---------------------------------------------------------------------------
// 认证: AP 配网模式不校验(此时用户还没法拿到密码页)，STA 下全部校验
// ---------------------------------------------------------------------------
static bool webAuth() {
  if (g_ns == NS_AP) return true;
  if (strlen(CFG_WEB_USER) == 0) return true;         // 用户主动关掉了认证
  if (server.authenticate(CFG_WEB_USER, CFG_WEB_PASS)) return true;
  server.requestAuthentication();
  return false;
}

// 文件名消毒: 只留 [A-Za-z0-9._-]，去掉原扩展名，最长 12 字符
static void safeName(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < cap && o < 12; i++) {
    char c = in[i];
    if (c == '.') break;                              // 到扩展名就停
    if (isalnum((unsigned char)c) || c == '_' || c == '-') out[o++] = c;
  }
  out[o] = 0;
  if (o == 0) snprintf(out, cap, "f%lu", (unsigned long)(millis() / 1000));
}

// ---------------------------------------------------------------------------
// 页面 (PROGMEM，避免占 RAM)
// ---------------------------------------------------------------------------
static const char AP_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>imgshow setup</title><style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}
h2{color:#0cf;margin:0 0 12px}input,button,select{width:100%;box-sizing:border-box;
padding:10px;margin:5px 0;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee;font-size:16px}
button{background:#06c;border:0;font-weight:bold}small{color:#888}
</style></head><body>
<h2>imgshow WiFi</h2>
<select id=ss><option value="">-- scan --</option></select>
<button onclick=sc()>Scan</button>
<input id=sd placeholder="SSID (手动填)">
<input id=us placeholder="账号 (企业网才填, 家用留空)">
<input id=pw type=password placeholder="密码">
<button onclick=sv()>Save &amp; Reboot</button>
<p id=m><small>保存后设备重启并连接你的路由器。<br>连上后屏幕右上角显示 OK。</small></p>
<script>
function sc(){m.innerHTML='scanning...';fetch('/scan').then(r=>r.json()).then(a=>{
ss.innerHTML='<option value="">-- pick --</option>'+a.map(x=>`<option value="${x.s}">${x.s} (${x.r})</option>`).join('');
m.innerHTML='<small>选一个，或手动填写</small>';});}
ss.onchange=()=>{if(ss.value)sd.value=ss.value};
function sv(){if(!sd.value){m.innerHTML='SSID 不能为空';return;}
m.innerHTML='saving...';
fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'s='+encodeURIComponent(sd.value)+'&u='+encodeURIComponent(us.value)+'&p='+encodeURIComponent(pw.value)})
.then(r=>r.text()).then(t=>m.innerHTML=t);}
</script></body></html>)HTML";

static const char MAIN_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>imgshow</title><style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:12px}
h3{color:#0cf;margin:16px 0 6px;font-size:15px;border-bottom:1px solid #333;padding-bottom:4px}
input,button,select{box-sizing:border-box;padding:9px;margin:3px 0;border-radius:6px;
border:1px solid #444;background:#1c1c1c;color:#eee;font-size:15px}
input,select{width:100%}button{background:#06c;border:0;color:#fff;font-weight:bold}
.r{display:flex;gap:6px}.r>*{flex:1}#st{font:12px monospace;color:#9c9;white-space:pre-wrap}
.g{display:grid;grid-template-columns:1fr 1fr 1fr;gap:5px}.g button{background:#333;font-weight:normal}
li{font:13px monospace;margin:2px 0}ul{padding-left:18px}.d{background:#822}
small{color:#888}
</style></head><body>
<h3>STATUS</h3><div id=st>loading...</div>

<h3>IMAGE 上传 (自动裁成 128x128)</h3>
<input type=file id=fi accept="image/*"><button onclick=up()>Upload</button>
<div id=um><small>手机相册直接选，浏览器本地转 RGB565</small></div>

<h3>VIDEO 片段 (.v64, tools/mkvideo.py 生成)</h3>
<input type=file id=fv accept=".v64,.bin"><button onclick=uv()>Upload clip</button>
<div id=vm></div>

<h3>MEMO</h3>
<div class=r><input id=mt placeholder="新备忘 (25字符内)"><button onclick=ma()>Add</button></div>
<ul id=ml></ul><button class=d onclick="q('/memo?clear=1')">Clear all</button>

<h3>TIMER</h3>
<div class=r><input id=tn placeholder=name value=Timer><input id=tm type=number value=5 min=1 max=600>
<button onclick="q('/timer?min='+tm.value+'&name='+encodeURIComponent(tn.value))">Start</button></div>

<h3>SETTINGS</h3>
<div class=r><input id=ci placeholder="city e.g. Hangzhou"><button onclick="q('/cfg?city='+encodeURIComponent(ci.value))">Set city</button></div>
<div class=r><input id=br type=number min=15 max=255 value=255><button onclick="q('/cfg?bri='+br.value)">Brightness</button></div>
<div class=r><button onclick="q('/refresh?w=1')">Weather</button><button onclick="q('/refresh?s=1')">Stock</button><button onclick="q('/refresh?f=1')">Match</button></div>

<h3>GOTO PAGE</h3>
<div class=g>
<button onclick="q('/page?p=0')">Menu</button><button onclick="q('/page?p=1')">Clock</button>
<button onclick="q('/page?p=2')">Weather</button><button onclick="q('/page?p=3')">Stock</button>
<button onclick="q('/page?p=4')">Memo</button><button onclick="q('/page?p=5')">Image</button>
<button onclick="q('/page?p=6')">Video</button><button onclick="q('/page?p=9')">Face</button>
<button onclick="q('/page?p=13')">Info</button></div>

<h3>FILES</h3><ul id=fl></ul>
<h3>DANGER</h3>
<div class=r><button class=d onclick="if(confirm('reboot?'))q('/reboot')">Reboot</button>
<button class=d onclick="if(confirm('erase wifi?'))q('/reset')">Erase WiFi</button></div>

<script>
function q(u){return fetch(u).then(r=>r.text()).then(t=>{setTimeout(rf,300);return t});}
function rf(){fetch('/status').then(r=>r.json()).then(j=>{
st.textContent=`ip ${j.ip}  rssi ${j.rssi}dBm  ${j.net}
heap ${(j.heap/1024).toFixed(1)}KB  fs ${(j.fsu/1024)|0}/${(j.fst/1024)|0}KB
page ${j.page}   img ${j.img}  vid ${j.vid}
wx ${j.wx}   stk ${j.stk}`;
ml.innerHTML=j.memo.map((m,i)=>`<li>${m.d?'[x]':'[ ]'} ${m.t} <a href=# onclick="q('/memo?del='+${i});return false">del</a></li>`).join('');
fl.innerHTML=j.files.map(f=>`<li>${f} <a href=# onclick="q('/del?f='+encodeURIComponent('${f}'));return false">del</a></li>`).join('');
if(!ci.value)ci.value=j.city;});}
function r565(img){const c=document.createElement('canvas');c.width=128;c.height=128;
const x=c.getContext('2d');const s=Math.max(128/img.width,128/img.height);
const w=img.width*s,h=img.height*s;x.drawImage(img,(128-w)/2,(128-h)/2,w,h);
const d=x.getImageData(0,0,128,128).data,o=new Uint8Array(32768);
for(let i=0,j=0;i<d.length;i+=4){const v=((d[i]&248)<<8)|((d[i+1]&252)<<3)|(d[i+2]>>3);
o[j++]=v&255;o[j++]=v>>8;}return o;}
function up(){const f=fi.files[0];if(!f){um.innerHTML='pick a file';return;}
um.innerHTML='converting...';const im=new Image();im.onload=()=>{const b=r565(im);
const fd=new FormData();fd.append('f',new Blob([b]),(f.name.split('.')[0]||'img'));
um.innerHTML='uploading 32KB...';
fetch('/upimg',{method:'POST',body:fd}).then(r=>r.text()).then(t=>{um.innerHTML=t;rf();});};
im.onerror=()=>um.innerHTML='decode failed';im.src=URL.createObjectURL(f);}
function uv(){const f=fv.files[0];if(!f){vm.innerHTML='pick a .v64';return;}
const fd=new FormData();fd.append('f',f,f.name);vm.innerHTML='uploading '+((f.size/1024)|0)+'KB...';
fetch('/upvid',{method:'POST',body:fd}).then(r=>r.text()).then(t=>{vm.innerHTML=t;rf();});}
function ma(){if(!mt.value)return;q('/memo?add='+encodeURIComponent(mt.value)).then(()=>mt.value='');}
rf();setInterval(rf,3000);
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// 处理器
// ---------------------------------------------------------------------------
static void hRoot() {
  if (g_ns == NS_AP) { server.send_P(200, "text/html", AP_HTML); return; }
  if (!webAuth()) return;
  server.send_P(200, "text/html", MAIN_HTML);
}

static void hScan() {
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n && i < 15; i++) {
    if (i) j += ',';
    j += "{\"s\":\"" + WiFi.SSID(i) + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

static void hSave() {
  String s = server.arg("s"), u = server.arg("u"), p = server.arg("p");
  if (!s.length()) { server.send(400, "text/plain", "SSID empty"); return; }
  netSaveCreds(s, u, p);
  server.send(200, "text/plain", "saved, rebooting...");
  delay(300);
  ESP.restart();
}

static void hStatus() {
  if (!webAuth()) return;
  String j = "{";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"net\":\"" + String(netStateStr()) + (netBusy() ? " busy" : "") + "\"";
  j += ",\"heap\":" + String((uint32_t)ESP.getFreeHeap());
  j += ",\"fsu\":" + String((uint32_t)fsUsed()) + ",\"fst\":" + String((uint32_t)fsTotal());
  j += ",\"page\":\"" + String(PAGES[g_page].name) + "\"";
  j += ",\"img\":" + String(g_imgN) + ",\"vid\":" + String(g_vidN);
  j += ",\"city\":\"" + String(g_set.city) + "\"";
  netLock();
  char t[64];
  snprintf(t, sizeof(t), "%dC %d%% %s", g_wx.tempC, g_wx.humid, g_wx.valid ? g_wx.desc : "n/a");
  j += ",\"wx\":\"" + String(t) + "\"";
  snprintf(t, sizeof(t), "%s %s %s%%", g_stk.name, g_stk.price, g_stk.pct);
  j += ",\"stk\":\"" + String(t) + "\"";
  netUnlock();
  j += ",\"memo\":[";
  for (uint8_t i = 0; i < g_memoN; i++) {
    if (i) j += ',';
    j += "{\"t\":\"" + String(g_memo[i].text) + "\",\"d\":" + String(g_memo[i].done ? 1 : 0) + "}";
  }
  j += "],\"files\":[";
  bool first = true;
  for (uint8_t i = 0; i < g_imgN; i++) { if (!first) j += ','; first = false; j += "\"/img/" + String(g_imgName[i]) + "\""; }
  for (uint8_t i = 0; i < g_vidN; i++) { if (!first) j += ','; first = false; j += "\"/vid/" + String(g_vidName[i]) + "\""; }
  j += "]}";
  server.send(200, "application/json", j);
}

// ---- 上传: 边收边写 flash ----
static File     g_upF;
static bool     g_upErr  = false;
static uint32_t g_upN    = 0;
static char     g_upPath[48];

static void upBegin(const char* dir, const char* ext, uint32_t needFree) {
  HTTPUpload& u = server.upload();
  g_upErr = false; g_upN = 0; g_upPath[0] = 0;
  if (!g_fsOK) { g_upErr = true; return; }
  if (fsFree() < needFree) { g_upErr = true; return; }
  char nm[20]; safeName(u.filename.c_str(), nm, sizeof(nm));
  snprintf(g_upPath, sizeof(g_upPath), "%s/%s%s", dir, nm, ext);
  if (LittleFS.exists(g_upPath)) LittleFS.remove(g_upPath);
  g_upF = LittleFS.open(g_upPath, "w");
  if (!g_upF) g_upErr = true;
}
static void upWrite() {
  HTTPUpload& u = server.upload();
  if (g_upErr || !g_upF) return;
  if (g_upF.write(u.buf, u.currentSize) != u.currentSize) g_upErr = true;   // 写满了
  g_upN += u.currentSize;
}
static void upFinish(uint32_t expect) {
  if (g_upF) g_upF.close();
  if (!g_upErr && expect && g_upN != expect) g_upErr = true;
  if (g_upErr && g_upPath[0]) LittleFS.remove(g_upPath);
  fsRescan();
}

static void hUpImg() {   // upload 回调
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START)      upBegin("/img", ".565", IMG_BYTES + 8192);
  else if (u.status == UPLOAD_FILE_WRITE) upWrite();
  else if (u.status == UPLOAD_FILE_END)   upFinish(IMG_BYTES);
  else if (u.status == UPLOAD_FILE_ABORTED) { if (g_upF) g_upF.close(); if (g_upPath[0]) LittleFS.remove(g_upPath); g_upErr = true; }
}
static void hUpImgDone() {
  if (g_upErr) { server.send(500, "text/plain", "failed (need 32768B / no space)"); return; }
  if (g_imgCur < 0) g_imgCur = 0;
  if (g_page == P_IMAGE) requestPage(P_IMAGE);      // 重进一次好立刻看到
  server.send(200, "text/plain", "OK 32KB stored");
}

static void hUpVid() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) { vidClose(); upBegin("/vid", ".v64", 4096); }
  else if (u.status == UPLOAD_FILE_WRITE) upWrite();
  else if (u.status == UPLOAD_FILE_END)   upFinish(0);
  else if (u.status == UPLOAD_FILE_ABORTED) { if (g_upF) g_upF.close(); if (g_upPath[0]) LittleFS.remove(g_upPath); g_upErr = true; }
}
static void hUpVidDone() {
  if (g_upErr) { server.send(500, "text/plain", "failed / disk full"); return; }
  char msg[48]; snprintf(msg, sizeof(msg), "OK %uKB", (unsigned)(g_upN / 1024));
  server.send(200, "text/plain", msg);
}

// ---- /frame: 浏览器投屏。一帧一个 POST，协议开销大，只有 ~5fps。
//      要流畅请走 TCP 8888 (tools/stream_video.py)。
static void hFrame() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    g_vm = VM_LIVE;
    if (g_page != P_VIDEO) requestPage(P_VIDEO);
    if (!g_vts.active()) g_vts.begin(0, 0, SCR_W, SCR_H);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (g_vts.active()) g_vts.push(u.buf, u.currentSize);
    if (g_vts.complete()) { g_vts.end(); g_vts.begin(0, 0, SCR_W, SCR_H); }
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (g_vts.active()) g_vts.end();
  }
}

static void hDel() {
  if (!webAuth()) return;
  String f = server.arg("f");
  if (!f.startsWith("/img/") && !f.startsWith("/vid/")) { server.send(400, "text/plain", "bad path"); return; }
  if (f.startsWith("/vid/")) vidClose();
  bool ok = LittleFS.remove(f);
  fsRescan();
  if (g_imgCur >= (int8_t)g_imgN) g_imgCur = g_imgN ? 0 : -1;
  if (g_vidCur >= (int8_t)g_vidN) g_vidCur = g_vidN ? 0 : -1;
  server.send(ok ? 200 : 404, "text/plain", ok ? "deleted" : "not found");
}

static void hMemo() {
  if (!webAuth()) return;
  if (server.hasArg("clear")) { memoClear(); }
  else if (server.hasArg("del")) { memoDel((uint8_t)server.arg("del").toInt()); }
  else if (server.hasArg("add")) {
    char t[MEMO_TEXT_LEN];
    asciiOnly(server.arg("add").c_str(), t, sizeof(t));   // 经典字体只画 ASCII
    if (!t[0]) { server.send(400, "text/plain", "ascii only"); return; }
    if (!memoAdd(t)) { server.send(507, "text/plain", "memo full (8)"); return; }
  }
  if (g_page == P_MEMO) requestPage(P_MEMO);
  server.send(200, "text/plain", "ok");
}

static void hTimer() {
  if (!webAuth()) return;
  uint16_t m = (uint16_t)server.arg("min").toInt();
  if (m == 0 || m > 600) { server.send(400, "text/plain", "1..600 min"); return; }
  char nm[16]; asciiOnly(server.arg("name").c_str(), nm, sizeof(nm));
  timerStartFromWeb(nm[0] ? nm : "Timer", m);
  server.send(200, "text/plain", "started");
}

static void hPage() {
  if (!webAuth()) return;
  int p = server.arg("p").toInt();
  if (p < 0 || p >= P_COUNT) { server.send(400, "text/plain", "range"); return; }
  requestPage((PageId)p);
  server.send(200, "text/plain", PAGES[p].name);
}

static void hRefresh() {
  if (!webAuth()) return;
  if (server.hasArg("w")) netRequest(JOB_WEATHER);
  if (server.hasArg("s")) netRequest(JOB_STOCK);
  if (server.hasArg("f")) netRequest(JOB_FOOTBALL);
  server.send(200, "text/plain", "queued");
}

static void hCfg() {
  if (!webAuth()) return;
  if (server.hasArg("city")) {
    char c[20]; asciiOnly(server.arg("city").c_str(), c, sizeof(c));
    if (c[0]) { strncpy(g_set.city, c, sizeof(g_set.city) - 1); g_set.city[sizeof(g_set.city) - 1] = 0; netRequest(JOB_WEATHER); }
  }
  if (server.hasArg("stk"))  { int v = server.arg("stk").toInt();  if (v >= 0 && v < STOCK_COUNT) { g_set.stockIdx = v; netRequest(JOB_STOCK); } }
  if (server.hasArg("bri"))  { int v = server.arg("bri").toInt();  if (v < 15) v = 15; if (v > 255) v = 255; g_set.brightness = v; uiBacklight(v); }
  if (server.hasArg("cyc"))  g_set.autoCycle = server.arg("cyc").toInt() != 0;
  if (server.hasArg("cycs")) { int v = server.arg("cycs").toInt(); if (v >= 5 && v <= 120) g_set.cycleSec = v; }
  if (server.hasArg("rfm"))  { int v = server.arg("rfm").toInt();  if (v >= 5 && v <= 180) g_set.refreshMin = v; }
  setSave();
  server.send(200, "text/plain", "ok");
}

static void hReset() {
  if (!webAuth()) return;
  netClearCreds();
  server.send(200, "text/plain", "wifi erased, rebooting");
  delay(300); ESP.restart();
}
static void hReboot() {
  if (!webAuth()) return;
  server.send(200, "text/plain", "rebooting");
  delay(300); ESP.restart();
}

// ---------------------------------------------------------------------------
// 注册 + 启动
// ---------------------------------------------------------------------------
static void webRoutes() {
  server.on("/",        HTTP_GET,  hRoot);
  server.on("/scan",    HTTP_GET,  hScan);
  server.on("/save",    HTTP_POST, hSave);
  server.on("/status",  HTTP_GET,  hStatus);
  server.on("/upimg",   HTTP_POST, hUpImgDone, hUpImg);
  server.on("/upvid",   HTTP_POST, hUpVidDone, hUpVid);
  server.on("/frame",   HTTP_POST, [](){ server.send(200, "text/plain", "ok"); }, hFrame);
  server.on("/del",     HTTP_GET,  hDel);
  server.on("/memo",    HTTP_GET,  hMemo);
  server.on("/timer",   HTTP_GET,  hTimer);
  server.on("/page",    HTTP_GET,  hPage);
  server.on("/refresh", HTTP_GET,  hRefresh);
  server.on("/cfg",     HTTP_GET,  hCfg);
  server.on("/reset",   HTTP_GET,  hReset);
  server.on("/reboot",  HTTP_GET,  hReboot);
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); });
}

void webBegin() {
  if (g_webUp) return;
  webRoutes();
  server.begin();
  g_webUp = true;
}

// net.h 里声明: 只有"从来没配过 WiFi"时才走这条路
void netStartAP() {
  WiFi.mode(WIFI_AP);
  // softAP 密码必须 >=8 位，否则库会直接开成开放热点
  if (strlen(CFG_AP_PASS) >= 8) WiFi.softAP(CFG_AP_SSID, CFG_AP_PASS);
  else                         WiFi.softAP(CFG_AP_SSID);
  g_ns = NS_AP;
  webBegin();
  requestPage(P_APINFO);
}

// 设置页里手动重新配网(不擦已有凭据，重启就恢复)
void netEnterAPFromUI() { netStartAP(); }

// 每个 loop 调；WebServer 本身是非阻塞轮询
inline void webTick() { if (g_webUp) server.handleClient(); }
