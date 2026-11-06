/**
 * ============================================================================
 *  config_loader.h  —  运行期配置（LittleFS 里的 /config.json）
 * ----------------------------------------------------------------------------
 *  目的：fan_config.h 里的参数是编译期写死的，改一次要重新编译烧录。
 *        有了这个文件，把 /config.json 传进 LittleFS，就不用重烧固件了。
 *
 *  三条硬规则（别破坏）：
 *    1. 不修改 fan_config.h —— 它保持纯净，不引入 LittleFS / JSON。
 *    2. 不产生循环包含 —— 这里只包含系统库 + fan_config.h + fan_types.h。
 *    3. 没有全局对象的构造函数 —— `static Runtime rt;` 是 Pod，零初始化，
 *       不跑任何代码。读文件、解析 JSON 全在 cfg::load() 里，
 *       必须由 setup() 主动调用（那时 LittleFS 才有意义）。
 *
 *  用法（fan_main.ino 的 setup() 里，g_cmd 默认值设好之后、net::begin() 之前）：
 *      cfg::load();                       // 读 /config.json，覆盖默认值
 *      wm.autoConnect(cfg::apSsid(), cfg::apPass());   // net_web.h 里已接好
 *
 *  ★ 哪些项「立刻生效」，哪些只是「存着备用」，见 README「运行期配置」一节。
 *    简单说：g_cmd 那四项（speed / oscSpan / heatOn / camOn）和两个 WiFi 项
 *    现在就是通的；其余参数按需求只是存进 cfg::rt，等对应模块改成读 rt 才生效
 *    （宏本身没法在运行期改，这是 C 预处理的性质）。
 * ============================================================================
 */
#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include <LittleFS.h>
#include <ArduinoJson.h>

#include "fan_config.h"
#include "fan_types.h"       /* g_cmd / FanCmd */

namespace cfg {

static constexpr const char *kJsonPath = "/config.json";

/* AP 密码最短 8 位，短了 WiFiManager 开热点会失败（而且只在连不上时才发现） */
static constexpr size_t kMinApPassLen = 8;

/* ---------------------------------------------------------------------------
 * 运行期配置
 *   全部是 Pod（无构造、无默认成员初始化），所以 `static Runtime rt;`
 *   只是把整块内存清零，不会在全局构造阶段被调用。
 *   真正的默认值在 resetDefaults() 里从 fan_config.h 灌进去。
 * ------------------------------------------------------------------------- */
struct Runtime {
    /* ---- 网络（net_web.h 的配网热点用）---- */
    const char *wifiSsid;
    const char *wifiPass;

    /* ---- 红外 / 热源 ---- */
    float    irDeltaC;         /* 相对背景的温升阈值（℃）*/
    float    irFullC;          /* 达到这个温升算 100% 置信度 */
    int      irMinHot;         /* 至少几个像素超阈值 */
    int      irConfirmN;       /* 连续 N 帧确认 */
    int      irReleaseN;       /* 连续 N 帧取消 */
    float    irDeadbandDeg;    /* 对准死区（°）*/
    float    irAlignStepDeg;   /* 单次修正最大角度（°）*/
    float    irRealignDeg;     /* 送风中跑偏多少度重新对准 */
    float    irModelConfMin;   /* man 概率阈值 */
    int      irModelArenaKb;   /* 竞技场大小（★ 编译期决定，运行期改不了）*/
    int      irModelInferMs;   /* 推理降频周期 */
    float    irModelVmax;      /* 预处理归一化上限（℃）*/
    float    irModelVmin;      /* 预处理归一化基准（℃）*/

    /* ---- 步进 ---- */
    int      stepScanRpm;      /* 扫描转速 */
    int      stepAimRpm;       /* 对准转速 */
    uint32_t stepReleaseMs;    /* 锁定多久后断电 */
    uint32_t stepPosSaveMs;    /* 角度最长多久落一次 NVS */

    /* ---- 风机 ---- */
    int      fanMinDutyPct;    /* 低于它直接截到 0 */
    uint32_t fanNaturePeriod;  /* 自然风起伏周期 */
    float    fanNatureLow;     /* 自然风最低比例 0..1 */
    uint32_t fanSoftStepMs;    /* 软启动步进间隔 */

    /* ---- 系统 / 定时 ---- */
    uint32_t statePeriodMs;
    uint32_t irPollMs;
    uint32_t irIdlePollMs;
    uint32_t uiPeriodMs;
    uint32_t powerPeriodMs;
    uint32_t targetLostMs;
    uint32_t camOnlineMs;
    uint32_t camFreshMs;

    /* ---- 元信息 ---- */
    bool     loaded;           /* true = 读到了 config.json 并解析成功 */
    uint16_t applied;          /* 本次实际覆盖了多少项 */
};

static Runtime rt;             /* 零初始化，无构造函数 */

/* WiFi 字符串要拷进自己的缓冲：JSON 解析用的 doc 是 load() 里的局部变量，
   直接把 const char* 存出去就成了悬空指针。 */
static char ssidBuf[33];       /* SSID 最长 32 + '\0' */
static char passBuf[65];       /* WPA2 最长 63 + '\0' */

/* ---------------------------------------------------------------------------
 * 默认值：全部回到 fan_config.h
 * ------------------------------------------------------------------------- */
static void resetDefaults()
{
    rt.wifiSsid = WIFI_AP_SSID;
    rt.wifiPass = WIFI_AP_PASS;

    rt.irDeltaC       = IR_DELTA_C;
    rt.irFullC        = IR_FULL_C;
    rt.irMinHot       = IR_MIN_HOT;
    rt.irConfirmN     = IR_CONFIRM_N;
    rt.irReleaseN     = IR_RELEASE_N;
    rt.irDeadbandDeg  = IR_DEADBAND_DEG;
    rt.irAlignStepDeg = IR_ALIGN_STEP_DEG;
    rt.irRealignDeg   = IR_REALIGN_DEG;
    rt.irModelConfMin = IR_MODEL_CONF_MIN;
    rt.irModelArenaKb = IR_MODEL_ARENA_KB;
    rt.irModelInferMs = IR_MODEL_INFER_MS;
    rt.irModelVmax    = IR_MODEL_VMAX;
    rt.irModelVmin    = IR_MODEL_VMIN;

    rt.stepScanRpm    = STEP_SCAN_RPM;
    rt.stepAimRpm     = STEP_AIM_RPM;
    rt.stepReleaseMs  = STEP_RELEASE_MS;
    rt.stepPosSaveMs  = STEP_POS_SAVE_MS;

    rt.fanMinDutyPct   = FAN_MIN_DUTY_PCT;
    rt.fanNaturePeriod = FAN_NATURE_PERIOD;
    rt.fanNatureLow    = FAN_NATURE_LOW;
    rt.fanSoftStepMs   = FAN_SOFT_STEP_MS;

    rt.statePeriodMs = STATE_PERIOD_MS;
    rt.irPollMs      = IR_POLL_MS;
    rt.irIdlePollMs  = IR_IDLE_POLL_MS;
    rt.uiPeriodMs    = UI_PERIOD_MS;
    rt.powerPeriodMs = POWER_PERIOD_MS;
    rt.targetLostMs  = TARGET_LOST_MS;
    rt.camOnlineMs   = CAM_ONLINE_MS;
    rt.camFreshMs    = CAM_FRESH_MS;

    rt.loaded  = false;
    rt.applied = 0;
}

/* ---------------------------------------------------------------------------
 * 配网热点用的 SSID / 密码
 *   load() 没跑过（rt 全是 0）时也不会把 nullptr 丢给 WiFiManager。
 * ------------------------------------------------------------------------- */
static const char *apSsid() { return rt.wifiSsid ? rt.wifiSsid : WIFI_AP_SSID; }
static const char *apPass() { return rt.wifiPass ? rt.wifiPass : WIFI_AP_PASS; }

/* ===========================================================================
 * 取值 + 限幅 + 日志
 *   返回值 = 这一项是否真的被写了。JSON 里没写（null）就原样返回 false，
 *   调用方不用管，默认值已经在 resetDefaults() 里灌好了。
 * ========================================================================= */
static bool ovFloat(JsonVariantConst v, const char *key, float &dst, float lo, float hi)
{
    if (v.isNull()) return false;

    float x;
    if (v.is<float>())                       x = v.as<float>();
    else if (v.is<long>())                   x = (float)v.as<long>();
    else {
        Serial.printf("[配置] %s 类型不对（要数字），这一项用默认值\n", key);
        return false;
    }

    const float raw = x;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    if (isnan(x)) {
        Serial.printf("[配置] %s 不是有效数字，这一项用默认值\n", key);
        return false;
    }

    dst = x;
    rt.applied++;
    if (x != raw) Serial.printf("[配置] 覆盖 %s = %.2f（写了 %.2f，已限制到 %.2f）\n", key, x, raw, x);
    else          Serial.printf("[配置] 覆盖 %s = %.2f\n", key, x);
    return true;
}

static bool ovInt(JsonVariantConst v, const char *key, int &dst, int lo, int hi)
{
    if (v.isNull()) return false;

    long x;
    if (v.is<long>())        x = v.as<long>();
    else if (v.is<float>())  x = lroundf(v.as<float>());   /* 写 180.0 也认 */
    else {
        Serial.printf("[配置] %s 类型不对（要整数），这一项用默认值\n", key);
        return false;
    }

    const long raw = x;
    if (x < (long)lo) x = (long)lo;
    if (x > (long)hi) x = (long)hi;

    dst = (int)x;
    rt.applied++;
    if (x != raw) Serial.printf("[配置] 覆盖 %s = %ld（写了 %ld，已限制到 %ld）\n", key, x, raw, x);
    else          Serial.printf("[配置] 覆盖 %s = %ld\n", key, x);
    return true;
}

static bool ovU32(JsonVariantConst v, const char *key, uint32_t &dst, uint32_t lo, uint32_t hi)
{
    if (v.isNull()) return false;

    long x;
    if (v.is<long>())        x = v.as<long>();
    else if (v.is<float>())  x = lroundf(v.as<float>());
    else {
        Serial.printf("[配置] %s 类型不对（要整数毫秒），这一项用默认值\n", key);
        return false;
    }

    const long raw = x;
    if (x < 0)                       x = 0;
    if (x < (long)lo)                x = (long)lo;
    if (x > (long)hi)                x = (long)hi;

    dst = (uint32_t)x;
    rt.applied++;
    if (x != raw) Serial.printf("[配置] 覆盖 %s = %ld ms（写了 %ld，已限制到 %ld）\n", key, x, raw, x);
    else          Serial.printf("[配置] 覆盖 %s = %ld ms\n", key, x);
    return true;
}

static bool ovBool(JsonVariantConst v, const char *key, bool &dst)
{
    if (v.isNull()) return false;

    if (v.is<bool>())        dst = v.as<bool>();
    else if (v.is<long>())   dst = (v.as<long>() != 0);      /* 写 1 / 0 也认 */
    else {
        Serial.printf("[配置] %s 类型不对（要 true/false），这一项用默认值\n", key);
        return false;
    }

    rt.applied++;
    Serial.printf("[配置] 覆盖 %s = %s\n", key, dst ? "true" : "false");
    return true;
}

static bool ovStr(JsonVariantConst v, const char *key, const char *&dst, char *buf, size_t bufLen)
{
    if (v.isNull()) return false;

    if (!v.is<const char *>()) {
        Serial.printf("[配置] %s 类型不对（要字符串），这一项用默认值\n", key);
        return false;
    }

    const char *s = v.as<const char *>();
    if (!s || !*s) {
        Serial.printf("[配置] %s 是空字符串，这一项用默认值\n", key);
        return false;
    }

    strncpy(buf, s, bufLen - 1);
    buf[bufLen - 1] = '\0';
    dst = buf;

    rt.applied++;
    Serial.printf("[配置] 覆盖 %s = \"%s\"\n", key, buf);
    return true;
}

/* ---------------------------------------------------------------------------
 * 用户意图（直接写 g_cmd）
 *   这几项是「立刻生效」的：状态机和网页读的就是 g_cmd。
 * ------------------------------------------------------------------------- */
static bool applyUserIntent(JsonVariantConst speed, JsonVariantConst oscSpan,
                            JsonVariantConst heatOn, JsonVariantConst camOn)
{
    bool any = false;

    int iv = (int)g_cmd.speed;
    if (ovInt(speed, "speed", iv, 0, 100)) {
        g_cmd.speed = (uint8_t)iv;
        any = true;
    }

    iv = (int)g_cmd.oscSpan;
    if (ovInt(oscSpan, "oscSpan", iv, OSC_SPAN_MIN, OSC_SPAN_MAX)) {
        g_cmd.oscSpan = (uint8_t)iv;
        any = true;
    }

    bool bv = g_cmd.heatOn;
    if (ovBool(heatOn, "heatOn", bv)) {
        g_cmd.heatOn = bv;
        any = true;
    }

    bv = g_cmd.camOn;
    if (ovBool(camOn, "camOn", bv)) {
        g_cmd.camOn = bv;
        any = true;
    }

    return any;
}

/* ---------------------------------------------------------------------------
 * 主入口：读 /config.json 覆盖默认值
 *   不传参、不返回错误码 —— 失败一律退回默认值，风扇照样能开，不能因为
 *   一个配置文件写错就变砖。
 * ------------------------------------------------------------------------- */
static void load()
{
    resetDefaults();

    Serial.println(F("[配置] 读取 /config.json（没有就用 fan_config.h 的默认值）"));

    /* LittleFS 可能还没挂载（net::begin() 在后面才跑）。
       begin(false) 不格式化：挂不上说明这个分区从没格式化过，
       那自然也谈不上有 config.json，直接走默认值。 */
    if (!LittleFS.begin(false)) {
        Serial.println(F("[配置] LittleFS 挂不上 → 全部使用默认值"));
        return;
    }

    if (!LittleFS.exists(kJsonPath)) {
        Serial.println(F("[配置] 没有 /config.json → 全部使用默认值（传一份到 LittleFS 就能用）"));
        return;
    }

    File f = LittleFS.open(kJsonPath, "r");
    if (!f) {
        Serial.println(F("[配置] /config.json 打不开 → 全部使用默认值"));
        return;
    }
    const size_t bytes = f.size();

#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;                    /* v7：弹性文档，不用估容量 */
#else
    DynamicJsonDocument doc(2048);       /* v6：字段多，给 2KB */
#endif

    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[配置] JSON 解析失败（%s）→ 全部使用默认值\n", err.c_str());
        Serial.println(F("[配置] 常见原因：多了一个逗号 / 少了引号 / 用了 // 注释（JSON 不支持注释）"));
        return;
    }
    if (!doc.is<JsonObject>()) {
        Serial.println(F("[配置] 顶层不是对象 {…} → 全部使用默认值"));
        return;
    }

    /* 统一转成 JsonObjectConst 再取值：
       JsonObjectConst::operator[] 返回的就是 JsonVariantConst，
       省掉每个调用点做类型转换（JsonVariant 到 JsonVariantConst 在 v7 里没有隐式转换）。 */
    JsonObjectConst root = doc.as<JsonObjectConst>();

    Serial.printf("[配置] 解析成功，文件 %u 字节\n", (unsigned)bytes);

    /* ---- 1. 用户意图 → g_cmd ---- */
    const bool gcmdChanged = applyUserIntent(root["speed"], root["oscSpan"],
                                             root["heatOn"], root["camOn"]);

    /* ---- 2. 网络 ---- */
    ovStr(root["wifiSsid"], "wifiSsid", rt.wifiSsid, ssidBuf, sizeof(ssidBuf));
    if (ovStr(root["wifiPass"], "wifiPass", rt.wifiPass, passBuf, sizeof(passBuf))) {
        /* 短于 8 位 WiFiManager 开热点会失败，而且只在连不上时才发现 */
        if (strlen(rt.wifiPass) < kMinApPassLen) {
            Serial.printf("[配置] wifiPass 只有 %u 位，不足 %u 位 → 退回默认密码\n",
                          (unsigned)strlen(rt.wifiPass), (unsigned)kMinApPassLen);
            rt.wifiPass = WIFI_AP_PASS;
        }
    }

    /* ---- 3. 红外 / 热源 ---- */
    ovFloat(root["irDeltaC"],       "irDeltaC",       rt.irDeltaC,       0.5f, 20.0f);
    ovFloat(root["irFullC"],        "irFullC",        rt.irFullC,        1.0f, 30.0f);
    ovInt  (root["irMinHot"],       "irMinHot",       rt.irMinHot,       1, IR_PIXELS);
    ovInt  (root["irConfirmN"],     "irConfirmN",     rt.irConfirmN,     1, 20);
    ovInt  (root["irReleaseN"],     "irReleaseN",     rt.irReleaseN,     1, 20);
    ovFloat(root["irDeadbandDeg"],  "irDeadbandDeg",  rt.irDeadbandDeg,  0.0f, 30.0f);
    ovFloat(root["irAlignStepDeg"], "irAlignStepDeg", rt.irAlignStepDeg, 1.0f, 90.0f);
    ovFloat(root["irRealignDeg"],   "irRealignDeg",   rt.irRealignDeg,   1.0f, 90.0f);
    ovFloat(root["irModelConfMin"], "irModelConfMin", rt.irModelConfMin, 0.0f, 1.0f);
    if (ovInt(root["irModelArenaKb"], "irModelArenaKb", rt.irModelArenaKb, 8, 128)) {
        Serial.println(F("[配置] ★ irModelArenaKb 是编译期的（tensorArena 数组大小），运行期改它不生效"));
    }
    ovInt  (root["irModelInferMs"], "irModelInferMs", rt.irModelInferMs, 50, 5000);
    ovFloat(root["irModelVmax"],    "irModelVmax",    rt.irModelVmax,    1.0f, 100.0f);
    ovFloat(root["irModelVmin"],    "irModelVmin",    rt.irModelVmin,    0.0f, 60.0f);

    /* vmax 必须大于 vmin，否则归一化除零、量化结果全乱 */
    if (rt.irModelVmax <= rt.irModelVmin) {
        Serial.printf("[配置] irModelVmax(%.1f) 不大于 irModelVmin(%.1f) → 两个都退回默认 %.1f / %.1f\n",
                      rt.irModelVmax, rt.irModelVmin,
                      (float)IR_MODEL_VMAX, (float)IR_MODEL_VMIN);
        rt.irModelVmax = IR_MODEL_VMAX;
        rt.irModelVmin = IR_MODEL_VMIN;
    }

    /* ---- 4. 电机 / 步进 ---- */
    ovInt(root["stepScanRpm"],   "stepScanRpm",   rt.stepScanRpm,   1, 15);
    ovInt(root["stepAimRpm"],    "stepAimRpm",    rt.stepAimRpm,    1, 15);
    ovU32(root["stepReleaseMs"], "stepReleaseMs", rt.stepReleaseMs, 0, 600000UL);
    ovU32(root["stepPosSaveMs"], "stepPosSaveMs", rt.stepPosSaveMs, 1000, 3600000UL);

    /* ---- 5. 风机 ---- */
    ovInt  (root["fanMinDutyPct"],   "fanMinDutyPct",   rt.fanMinDutyPct,   0, 50);
    ovU32  (root["fanNaturePeriod"], "fanNaturePeriod", rt.fanNaturePeriod, 500, 60000UL);
    ovFloat(root["fanNatureLow"],    "fanNatureLow",    rt.fanNatureLow,    0.10f, 1.0f);
    ovU32  (root["fanSoftStepMs"],   "fanSoftStepMs",   rt.fanSoftStepMs,   10, 1000);

    /* ---- 6. 系统 / 定时 ---- */
    ovU32(root["statePeriodMs"], "statePeriodMs", rt.statePeriodMs, 10, 1000);
    ovU32(root["irPollMs"],      "irPollMs",      rt.irPollMs,      50, 2000);
    ovU32(root["irIdlePollMs"],  "irIdlePollMs",  rt.irIdlePollMs,  200, 60000UL);
    ovU32(root["uiPeriodMs"],    "uiPeriodMs",    rt.uiPeriodMs,    50, 5000);
    ovU32(root["powerPeriodMs"], "powerPeriodMs", rt.powerPeriodMs, 200, 60000UL);
    ovU32(root["targetLostMs"],  "targetLostMs",  rt.targetLostMs,  100, 60000UL);
    ovU32(root["camOnlineMs"],   "camOnlineMs",   rt.camOnlineMs,   1000, 60000UL);
    ovU32(root["camFreshMs"],    "camFreshMs",    rt.camFreshMs,    200, 30000UL);

    rt.loaded = true;

    /* ---- 收尾 ---- */
    if (gcmdChanged) {
        /* 让主循环第一拍就按新值落地（摇头幅度要经 applyCommand 才会下发到电机）*/
        g_cmd.dirty = true;
    }

    Serial.printf("[配置] 完成：共覆盖 %u 项，其余沿用 fan_config.h 默认值\n",
                  (unsigned)rt.applied);
    Serial.printf("[配置] 生效状态：风速 %u%% | 摇头 %u° | 红外 %s | 摄像头 %s | 热点 %s\n",
                  (unsigned)g_cmd.speed, (unsigned)g_cmd.oscSpan,
                  g_cmd.heatOn ? "开" : "关", g_cmd.camOn ? "开" : "关",
                  apSsid());
}

} /* namespace cfg */

#endif /* CONFIG_LOADER_H */
