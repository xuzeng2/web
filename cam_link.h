/**
 * ============================================================================
 *  cam_link.h  —  与 ESP32-CAM 的 UART 协议（主控端）
 * ----------------------------------------------------------------------------
 *  物理层：Serial1 @115200，S3 TX17 → CAM RX，S3 RX16 ← CAM TX，必须共地。
 *
 *  协议：一行一条，逗号分隔，结尾 \r\n。关键词用中文，功能一目了然。
 *
 *    摄像头 → 主控
 *      摄像头,就绪              摄像头初始化完成、模型已加载
 *      摄像头,错误,<码>         出错（码由摄像头端定义，1=相机初始化失败）
 *      有人,<0|1>[,置信度[,位置]] 有没有人 / 置信度 0..1 / 横向位置 0..1(可选)
 *      心跳,<ms>                摄像头心跳（上电毫秒数）
 *
 *    主控 → 摄像头
 *      开关,<0|1>               开/关推理（网页上的“摄像头识别”开关）
 *      心跳                     主控心跳
 *      重置                     让摄像头软复位推理状态
 *
 *  ★ 编码：两端源码文件都必须是 **UTF-8**（Arduino IDE 默认就是），
 *    中文关键词在线上就是普通 UTF-8 字节，strcmp 照常工作。
 *    千万别用记事本“另存为 ANSI/GBK”，否则两边的字节对不上，收不到指令。
 *    关键词统一写在这里的 KEY_xxx 宏里（摄像头端 fan_cam.ino 有同名宏），
 *    以后要改词只改这两处。
 *
 *  容错：
 *    · 超过 CAM_ONLINE_MS 没有任何报文 → 判离线
 *    · 有人,1 只在 CAM_FRESH_MS 内有效，摄像头卡住不会一直“看见人”
 *    · 未知行直接丢，不影响已解析的状态
 * ============================================================================
 */
#ifndef CAM_LINK_H
#define CAM_LINK_H

#include "fan_config.h"
#include "fan_types.h"

/* 协议关键词（必须与 fan_cam.ino 里的定义完全一致）*/
#define KEY_PERSON      "有人"
#define KEY_CAM         "摄像头"
#define KEY_READY       "就绪"
#define KEY_ERR         "错误"
#define KEY_HB          "心跳"
#define KEY_EN          "开关"
#define KEY_RESET       "重置"

namespace camlink {

static HardwareSerial port(1);

static char     line[96];
static uint8_t  lineLen   = 0;

static bool     online    = false;
static uint32_t lastRxMs  = 0;
static uint32_t lastPerMs = 0;
static bool     rawPerson = false;
static float    confVal   = 0.0f;
static float    cxVal     = -1.0f;
static uint32_t frames    = 0;
static bool     camReady  = false;

static int8_t   sentEn    = -1;       /* -1 表示还没发过 */
static uint32_t lastHbMs  = 0;
static uint32_t lastEnableMs = 0;
static bool     wantEn    = false;

/* ---------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------- */
static void begin()
{
    port.begin(CAM_BAUD, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);
    port.setTimeout(20);
    lastRxMs = millis();
}

/* ---------------------------------------------------------------------------
 * 发送
 * ------------------------------------------------------------------------- */
static void sendLine(const char *s)
{
    port.print(s);
    port.print("\r\n");
}

/* 开关推理：去重 + 限速，网页上反复点也不会把摄像头打爆 */
static void setEnable(bool en)
{
    wantEn = en;
    int8_t v = en ? 1 : 0;
    if (v == sentEn) return;
    if (millis() - lastEnableMs < CAM_ENABLE_ON_MS) return;   /* 稍后再发 */
    lastEnableMs = millis();
    sentEn = v;
    char buf[16];
    snprintf(buf, sizeof(buf), KEY_EN ",%d", (int)v);
    sendLine(buf);
    Serial.printf("[摄像头] -> %s\n", buf);
}

/* ---------------------------------------------------------------------------
 * 解一行
 * ------------------------------------------------------------------------- */
static void handleLine(char *s)
{
    lastRxMs = millis();

    /* 拆字段：最多 4 段 */
    char *f[4] = { nullptr, nullptr, nullptr, nullptr };
    uint8_t n = 0;
    char *p = s;
    while (n < 4) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    if (f[0] == nullptr) return;

    if (!strcmp(f[0], KEY_PERSON) && n >= 2) {
        rawPerson = (atoi(f[1]) != 0);
        confVal   = (n >= 3) ? atof(f[2]) : (rawPerson ? 1.0f : 0.0f);
        cxVal     = (n >= 4) ? atof(f[3]) : -1.0f;
        lastPerMs = millis();
        frames++;
    } else if (!strcmp(f[0], KEY_CAM) && n >= 2) {
        if (!strcmp(f[1], KEY_READY)) {
            camReady = true;
            sentEn   = -1;            /* 摄像头重启过，开关状态得重发一遍 */
            Serial.println(F("[摄像头] 就绪"));
        } else if (!strcmp(f[1], KEY_ERR)) {
            Serial.printf("[摄像头] 错误码 %s\n", (n >= 3 && f[2]) ? f[2] : "?");
        }
    } else if (!strcmp(f[0], KEY_HB)) {
        /* 心跳只用来维持在线状态，不做别的 */
    }
    /* 其它行忽略 */
}

/* ---------------------------------------------------------------------------
 * 周期调用：收字节、拆行、刷新 g_rt.cam
 * ------------------------------------------------------------------------- */
static void service()
{
    while (port.available()) {
        char ch = (char)port.read();
        if (ch == '\n' || ch == '\r') {
            if (lineLen > 0) {
                line[lineLen] = '\0';
                handleLine(line);
                lineLen = 0;
            }
        } else if (lineLen < sizeof(line) - 1) {
            line[lineLen++] = ch;
        } else {
            lineLen = 0;              /* 超长行直接丢，防止缓冲区被垃圾占死 */
        }
    }

    uint32_t now = millis();

    /* 主控心跳 */
    if (now - lastHbMs >= CAM_HB_MS) {
        lastHbMs = now;
        sendLine(KEY_HB);
    }

    /* 补发开关：sentEn 与期望不一致就重发（被限速挡下的那次会在这里补上）*/
    if (wantEn != (sentEn == 1)) setEnable(wantEn);

    /* 链路在线判定 */
    online = (now - lastRxMs < CAM_ONLINE_MS);

    /* 结论：在线 + 报过“有人,1” + 帧还新鲜 */
    bool p = online && rawPerson && (now - lastPerMs < CAM_FRESH_MS);

#if CAM_OFFLINE_IGNORE
    if (!online) p = true;            /* 掉线时“放行”，让红外单独工作 */
#endif

    g_rt.cam.online    = online;
    g_rt.cam.person    = p;
    g_rt.cam.rawPerson = rawPerson;
    g_rt.cam.conf      = confVal;
    g_rt.cam.cx        = cxVal;
    g_rt.cam.lastMsgMs = lastRxMs;
    g_rt.cam.frames    = frames;
}

static bool isReady()   { return camReady; }
static bool isOnline()  { return online; }

} /* namespace camlink */

#endif /* CAM_LINK_H */
