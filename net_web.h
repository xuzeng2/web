/**
 * ============================================================================
 *  net_web.h  —  WiFi 配网 + 网页控制（HTTP）
 * ----------------------------------------------------------------------------
 *  一、配网（WiFiManager 2.x，非阻塞模式）
 *      · 开机先试已保存的 WiFi；连不上（或从来没配过）→ 自动开热点
 *            SSID: FanCtrl-Setup   密码: 12345678
 *        手机连上后会自动弹配置页（不弹就手动开 http://192.168.4.1）
 *      · 选好 WiFi、填密码、保存 → 热点关闭、自动连上目标 WiFi
 *      · 配网期间主循环照常跑（非阻塞），风扇/屏幕都不耽误
 *      · 超时 WIFI_PORTAL_TIMEOUT 秒还没配好 → 关热点、重试已保存的 WiFi，
 *        断线后每 WIFI_RETRY_MS 重连一次
 *
 *  二、网页接口（和子测试项目 html 里的 fetch 完全对齐）
 *      GET  /            返回 LittleFS 里的 index.html
 *      GET  /status      当前状态（页面加载时用来恢复上次的样子）
 *      POST /post        下发控制（页面每次操作都调）
 *      GET  /stats       仪表盘：芯片温度 / 空闲内存 / 运行时间
 *      GET  /diag        调试用：红外、摄像头、电机、模式，一行全看到
 *      GET  /ir          红外实时帧（有没有人 / 温升 / 偏角 / 热点质心），页面红点用
 *
 *  三、网页文件放哪
 *      把 data/index.html 用「LittleFS 上传」烧进 flash：
 *        Arduino IDE 2.x → 装 arduino-littlefs-upload 插件 → Ctrl+Shift+P → Upload LittleFS
 *        arduino-cli    → arduino-cli upload -p COMx --board ... --input-dir data
 *      没烧也能用：/ 会退化成一个内置的简易控制页（够验证状态机）。
 * ============================================================================
 */
#ifndef NET_WEB_H
#define NET_WEB_H

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

#include "fan_config.h"
#include "fan_types.h"
#include "config_loader.h"     /* 配网热点名/密码可被 /config.json 覆盖 */

/* ArduinoJson 6 / 7 的文档类型不一样，这里统一一下 */
#if ARDUINOJSON_VERSION_MAJOR >= 7
  #define FAN_JSON_DOC  JsonDocument
#else
  #define FAN_JSON_DOC  StaticJsonDocument<1024>
#endif

namespace net {

static WebServer  server(80);
static WiFiManager wm;

static bool     staConnected  = false;
static bool     portalWasOn   = false;
static uint32_t lastRetryMs   = 0;
static bool     fsReady       = false;
static bool     timeSynced    = false;

/* ---------------------------------------------------------------------------
 * 内置兜底页面（LittleFS 没烧的时候用）
 *   只保留最小可用的控制项，样式从简；正式页面还是 data/index.html
 * ------------------------------------------------------------------------- */
static const char PAGE_FALLBACK[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>定向风扇 · 兜底页</title><style>
body{font-family:system-ui,sans-serif;background:#0d1b2a;color:#cfe3f7;margin:0;padding:18px}
h3{color:#8fe0ff;font-weight:600;letter-spacing:2px}
.card{background:#101f31;border:1px solid #223247;border-radius:14px;padding:14px;margin:12px 0}
.row{display:flex;justify-content:space-between;align-items:center;padding:7px 0}
button{background:#1e5b8a;color:#e2f0ff;border:1px solid #4d94ff;border-radius:10px;padding:8px 14px;font-size:15px}
button.off{background:#6e2b2b;border-color:#b34747}
input[type=range]{width:100%}
small{color:#5f7d9e}
</style></head><body>
<h3>🌀 定向风扇（兜底页）</h3>
<small>没找到 data/index.html，先用这个验证功能。</small>
<div class="card">
<div class="row"><span>电源</span><button id="pw" onclick="t('power')">切换</button></div>
<div class="row"><span>风速 <b id="sp">0</b>%</span></div>
<input type="range" id="s" min="0" max="100" step="5" onchange="setSpeed(this.value)">
<div class="row"><span>自然风</span><button onclick="t('nature')">切换</button></div>
<div class="row"><span>扫描/摇头</span><button onclick="t('oscillate')">切换</button></div>
<div class="row"><span>红外识别</span><button onclick="t('trackHeat')">切换</button></div>
<div class="row"><span>摄像头识别</span><button onclick="t('camera')">切换</button></div>
</div>
<div class="card" id="st">读取中…</div>
<script>
let S={};
function post(o){return fetch('/post',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(r=>r.json()).then(refresh)}
function t(k){post({[k]:!S[k]})}
function setSpeed(v){post({speed:+v})}
function refresh(){fetch('/status').then(r=>r.json()).then(d=>{S=d;
 document.getElementById('sp').textContent=d.speed;
 document.getElementById('s').value=d.speed;
 document.getElementById('pw').className=d.power?'':'off';
 document.getElementById('st').textContent=
 '电源:'+(d.power?'开':'关')+'  自然风:'+(d.nature?'开':'关')+
 '  摇头:'+(d.oscillate?'开':'关')+'  红外:'+(d.trackHeat?'开':'关')+
 '  摄像头:'+(d.camera?'开':'关')})}
function stats(){fetch('/stats').then(r=>r.json()).then(d=>{
 document.title='fan '+d.temp.toFixed(1)+'C heap'+d.freeHeap})}
refresh();setInterval(stats,5000);
</script></body></html>)HTML";

/* ---------------------------------------------------------------------------
 * 处理函数
 * ------------------------------------------------------------------------- */
static void handleRoot()
{
    if (fsReady && LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        server.streamFile(f, "text/html; charset=utf-8");
        f.close();
        return;
    }
    server.send_P(200, "text/html; charset=utf-8", PAGE_FALLBACK);
}

static void handleStatus()
{
    FAN_JSON_DOC doc;
    doc["power"]     = g_cmd.power;
    doc["speed"]     = g_cmd.speed;
    doc["nature"]    = g_cmd.nature;
    doc["oscillate"] = g_cmd.osc;
    doc["oscSpan"]   = g_cmd.oscSpan;   /* 摇摆幅度 0..180°（0 = 不扫）*/
    doc["trackHeat"] = g_cmd.heatOn;
    doc["camera"]    = g_cmd.camOn;
    doc["timerMin"]  = g_cmd.timerMin;
    /* 页面的定时逻辑用的是“绝对时间戳”，有 NTP 就给它一个，没有就给 0 */
    if (timeSynced && g_rt.timerDeadlineMs) {
        uint32_t remainMs = (g_rt.timerDeadlineMs > millis()) ? (g_rt.timerDeadlineMs - millis()) : 0;
        doc["timerEnd"] = (uint32_t)((time(nullptr) * 1000ULL) + remainMs);
    } else {
        doc["timerEnd"] = 0;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

/* ---------------------------------------------------------------------------
 * GET /ir —— 红外实时数据（网页上的热源红点就靠它）
 *   p   是否判定有人（已去抖）      hot 超阈值像素数
 *   d   最大温升，单位 0.1℃        a   偏角，单位 0.1°（+右 −左）
 *   c   置信度，单位 1%            x,y 热点质心，千分比 0..1000（8x8 视场）
 *   m   1 = 结论来自 TFLite 模型，0 = 阈值兜底
 * 全部走整数：不依赖 snprintf 的浮点支持，报文也只有几十字节。
 * ------------------------------------------------------------------------- */
static void handleIr()
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"p\":%d,\"hot\":%u,\"d\":%d,\"a\":%d,\"c\":%d,\"x\":%d,\"y\":%d,\"m\":%d}",
             g_rt.ir.person ? 1 : 0, (unsigned)g_rt.ir.hot,
             (int)lroundf(g_rt.ir.maxDelta * 10.0f),
             (int)lroundf(g_rt.ir.angleDeg * 10.0f),
             (int)lroundf(g_rt.ir.conf * 100.0f),
             (int)lroundf(g_rt.ir.xNorm * 1000.0f),
             (int)lroundf(g_rt.ir.yNorm * 1000.0f),
             g_rt.ir.byModel ? 1 : 0);
    server.send(200, "application/json", buf);
}

/* ---------------------------------------------------------------------------
 * GET /amg8833 —— 红外阵列原始数据（默认关闭，串口输 debug amg8833 true 才开）
 *   {"ok":1,"bg":24,"t":[[23,24,...8 个],[...],... 共 8 行]}
 *   t 是 8 行 × 8 列的整数℃，行优先；bg 是开机校准时算的背景均值。
 *   关闭时返回 404，避免平时被别人随手刷到、白白占用带宽。
 * ------------------------------------------------------------------------- */
static void handleAmg8833()
{
    if (!g_rt.dbgAmg) {
        server.send(404, "application/json", "{\"ok\":0,\"err\":\"debug off\"}");
        return;
    }

    char buf[560];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":1,\"bg\":%d,\"t\":[",
                     (int)lroundf(g_rt.bgMean));
    for (int r = 0; r < 8 && n > 0 && n < (int)sizeof(buf) - 24; r++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s[", r ? "," : "");
        for (int c = 0; c < 8 && n > 0 && n < (int)sizeof(buf) - 16; c++) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%d",
                          c ? "," : "", (int)lroundf(thermal::pix[r * 8 + c]));
        }
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]");
    }
    if (n > 0 && n < (int)sizeof(buf) - 4) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]}");
    } else {
        strcpy(buf, "{\"ok\":0,\"err\":\"overflow\"}");
    }
    server.send(200, "application/json", buf);
}

static void handlePost()
{
    String body = server.arg("plain");
    if (body.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"empty\"}");
        return;
    }

    FAN_JSON_DOC doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"json\"}");
        return;
    }

    /* 只认页面上真有的字段，其它一概忽略；范围一律夹死，别信浏览器 */
    if (!doc["power"].isNull())     g_cmd.power  = doc["power"].as<bool>();
    if (!doc["speed"].isNull()) {
        int s = doc["speed"].as<int>();
        if (s < 0)   s = 0;
        if (s > 100) s = 100;
        g_cmd.speed = (uint8_t)s;
    }
    if (!doc["nature"].isNull())    g_cmd.nature = doc["nature"].as<bool>();
    if (!doc["oscillate"].isNull()) g_cmd.osc    = doc["oscillate"].as<bool>();
    /* 摇摆幅度：0 = 不扫（网页滑杆拖到最左就等于关机摇头），上面夹到 0..180 */
    if (!doc["oscSpan"].isNull()) {
        int span = doc["oscSpan"].as<int>();
        if (span < OSC_SPAN_MIN) span = OSC_SPAN_MIN;
        if (span > OSC_SPAN_MAX) span = OSC_SPAN_MAX;
        g_cmd.oscSpan = (uint8_t)span;
    }
    if (!doc["trackHeat"].isNull()) g_cmd.heatOn = doc["trackHeat"].as<bool>();
    if (!doc["camera"].isNull())    g_cmd.camOn  = doc["camera"].as<bool>();

    /* 定时：优先用相对分钟（不依赖时钟），其次用绝对时间戳（需要 NTP）*/
    if (!doc["timerMin"].isNull()) {
        int m = doc["timerMin"].as<int>();
        g_cmd.timerMin = (m > 0) ? (uint32_t)m : 0;
    } else if (!doc["timerEnd"].isNull()) {
        uint64_t endMs = doc["timerEnd"].as<uint64_t>();
        if (endMs == 0) {
            g_cmd.timerMin = 0;
        } else if (timeSynced) {
            uint64_t nowMs = (uint64_t)time(nullptr) * 1000ULL;
            int m = (endMs > nowMs) ? (int)((endMs - nowMs) / 60000ULL) : 0;
            g_cmd.timerMin = (uint32_t)m;
        }
        /* 没对时就不处理：页面同时会发 timerMin，够用 */
    }

    g_cmd.dirty = true;      /* 交给主循环落地，HTTP 线程不碰硬件 */

    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleStats()
{
    FAN_JSON_DOC doc;
    doc["temp"]      = temperatureRead();
    doc["freeHeap"]  = ESP.getFreeHeap();
    doc["totalHeap"] = ESP.getHeapSize();
    doc["uptime"]    = (uint32_t)(millis() / 1000);
    doc["rssi"]      = WiFi.RSSI();
    doc["mode"]      = MODE_NAME[g_rt.mode];

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleDiag()
{
    FAN_JSON_DOC doc;
    doc["mode"]   = MODE_NAME[g_rt.mode];
    doc["bg"]     = g_rt.bgMean;
    doc["irPers"] = g_rt.ir.person;
    doc["irHot"]  = g_rt.ir.hot;
    doc["irDlt"]  = g_rt.ir.maxDelta;
    doc["irAng"]  = g_rt.ir.angleDeg;
    doc["camOn"]  = g_rt.cam.online;
    doc["camPers"] = g_rt.cam.person;
    doc["camFrm"] = g_rt.cam.frames;
    doc["angle"]  = g_rt.motorAngleDeg;
    doc["fanPct"] = g_rt.fanPct;
    doc["blow"]   = g_rt.blowing;
    doc["loopUs"] = g_rt.loopMaxUs;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleNotFound()
{
    server.send(404, "text/plain; charset=utf-8", "404");
}

/* ---------------------------------------------------------------------------
 * 联网成功后的收尾
 * ------------------------------------------------------------------------- */
static void onConnected()
{
    Serial.printf("[网络] 已连接：%s\n", WiFi.SSID().c_str());
    Serial.printf("[网络] 本机地址：%s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin(MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[网络] 局域网域名 http://%s.local\n", MDNS_NAME);
    }

    /* 对时：定时关机用的是真实时间戳，没有 NTP 也能用相对分钟，但有了更准 */
    configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
}

/* ---------------------------------------------------------------------------
 * 初始化：文件系统 + 路由 + 配网
 * ------------------------------------------------------------------------- */
static void begin()
{
    fsReady = LittleFS.begin(true);       /* true = 挂不上就格式化 */
    if (!fsReady) {
        Serial.println(F("[存储] LittleFS 挂载失败，网页将使用内置兜底页"));
    } else if (!LittleFS.exists("/index.html")) {
        Serial.println(F("[存储] 还没有 /index.html，请上传 data 目录（会用兜底页）"));
    }

    server.on("/",         HTTP_GET,  handleRoot);
    server.on("/index.html", HTTP_GET, handleRoot);
    server.on("/status",   HTTP_GET,  handleStatus);
    server.on("/stats",    HTTP_GET,  handleStats);
    server.on("/diag",     HTTP_GET,  handleDiag);
    server.on("/ir",       HTTP_GET,  handleIr);
    server.on("/amg8833",  HTTP_GET,  handleAmg8833);
    server.on("/post",     HTTP_POST, handlePost);
    server.onNotFound(handleNotFound);
    /* 注意：这里不能 server.begin()。
       1) 此时 lwIP 还没初始化（tcpip_init 尚未跑），NetworkServer::begin 里第一个 socket
          调用会去拿 lwIP 的 core-lock 信号量（NULL）→ assert xQueueSemaphoreTake queue.c:1709；
       2) 端口 80 还要留给 WiFiManager 的配网页。
       改为在 fan_main.ino 里等 WiFi 真正连上后再 net::server.begin()（见 netStartServerOnce）。 */
    Serial.println(F("[网页] 路由已注册，等 WiFi 拿到地址后再开 80 端口"));

    /* ---------------- 配网 ---------------- */
    WiFi.setHostname(WIFI_HOSTNAME);

    wm.setConfigPortalBlocking(false);          /* 关键：配网不阻塞主循环 */
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT);
    wm.setBreakAfterConfig(true);               /* 存完就退配网页，直接连目标 WiFi */
    wm.setConnectTimeout(WIFI_LINK_TIMEOUT);
    wm.setWiFiAutoReconnect(true);
    wm.setTitle("智能定向风扇");

    Serial.println(F("[网络] 正在连接已保存的网络；未配置会自动开热点"));
    /* 热点名/密码优先取 cfg::rt（/config.json 里能改），没配过就退回 fan_config.h 的宏。
       cfg::load() 必须在 net::begin() 之前调用过（见 setup()），否则这里还是宏的默认值。 */
    wm.autoConnect(cfg::apSsid(), cfg::apPass());
}

/* ---------------------------------------------------------------------------
 * 周期调用
 * ------------------------------------------------------------------------- */
static void service()
{
    wm.process();                 /* 非阻塞配网 / 重连都在这里面推进 */
    server.handleClient();

    bool on = (WiFi.status() == WL_CONNECTED);

    if (on && !staConnected) {
        staConnected = true;
        onConnected();
    } else if (!on && staConnected) {
        staConnected = false;
        Serial.println(F("[网络] 连接掉线，稍后重试"));
        lastRetryMs = millis();
    }

    /* 热点状态变化说一声，方便判断“到底在配网还是在干活” */
    bool portal = wm.getConfigPortalActive();
    if (portal != portalWasOn) {
        portalWasOn = portal;
        if (portal) {
            Serial.printf("[网络] 配网热点已开：%s / %s → http://192.168.4.1\n",
                          cfg::apSsid(), cfg::apPass());
        } else {
            Serial.println(F("[网络] 配网热点已关闭"));
        }
    }

    /* 断线重连（不弹热点，只重试已保存的凭据）*/
    if (!on && !portal && (millis() - lastRetryMs > WIFI_RETRY_MS)) {
        lastRetryMs = millis();
        WiFi.reconnect();
    }

    /* NTP 到点检查一次就够，别每次 loop 都读 */
    static uint32_t lastTimeChk = 0;
    if (!timeSynced && millis() - lastTimeChk > 5000) {
        lastTimeChk = millis();
        time_t t = time(nullptr);
        if (t > 1600000000) {         /* 2020 年之后才认为是真时间 */
            timeSynced = true;
            Serial.printf("[对时] 已同步时间：%s", ctime(&t));
        }
    }
}

static bool wifiOk()     { return WiFi.status() == WL_CONNECTED; }
static bool portalOn()   { return wm.getConfigPortalActive(); }

/* ★ 这里必须直接问 WiFi 栈，不能用缓存标志 staConnected：
     staConnected 只在 service() 里更新，而 setup() 的开机提示页
     （fan_main.ino 的 bootNetOverlay）在 loop 转起来之前就要判断联网没有。
     以前用缓存 → “*wm:AutoConnect: SUCCESS / STA IP 192.168.1.5” 都打出来了，
     屏幕上还写着“请连网”，热点名密码也一起往外报。 */

/* ---------------------------------------------------------------------------
 * 只推进配网状态机，不碰 WebServer
 *   setup() 里等联网的时候用：那时 loop 还没转起来，server 也还没 begin()，
 *   调用 service() 会顺手跑 server.handleClient() → 碰到没 begin 的 socket。
 * ------------------------------------------------------------------------- */
static void pump() { wm.process(); }

} /* namespace net */

#endif /* NET_WEB_H */
