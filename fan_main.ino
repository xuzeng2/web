/**
 * ============================================================================
 *  fan_main.ino  —  双模态智能定向风扇 · 主控（ESP32-S3）
 * ============================================================================
 *
 *  一句话说清这台机器在干什么：
 *      步进电机带着风扇 + 两组传感器左右扫描；红外看到“人”（摄像头也开了的话
 *      两边都得看到），就把风扇转过去对准他、停下来吹；人走了继续扫。
 *      全程可以在手机网页上控制和观察，屏幕上实时显示状态。
 *
 *  ---------------------------------------------------------------------------
 *  模式状态机
 *  ---------------------------------------------------------------------------
 *      BOOT ──校准完成──▶ SWEEP ──发现目标──▶ ALIGN ──对准──▶ BLOW
 *                           ▲                                  │
 *                           └─────────目标消失 1.5s─────────────┘
 *      手动模式（红外、摄像头都没开）：
 *           OFF ⇄ MANUAL    按网页上的风速常吹，电机只做扫描
 *
 *  ---------------------------------------------------------------------------
 *  双模态判定规则（需求 3、4）
 *  ---------------------------------------------------------------------------
 *      只开红外   → 红外看到人才吹
 *      只开摄像头 → 摄像头看到人才吹
 *      两个都开   → 必须「红外 && 摄像头」同时满足才吹（更抗误判）
 *      都不开     → 手动模式，按设定风速常吹
 *
 *  ---------------------------------------------------------------------------
 *  引脚（详见 fan_config.h）
 *  ---------------------------------------------------------------------------
 *      TFT  ST7789   SCLK12 MOSI11 CS10 DC9 RST14 BL13   （由 User_Setup.h 固定）
 *      I2C  SDA4 SCL5        AMG8833(0x69) + INA219(0x40)
 *      步进 18 / 20 / 19 / 21 → ULN2003 IN1..IN4
 *      风机 PWM 47 → MOS 栅极
 *      串口1 TX17→CAM RX   RX16←CAM TX（与 ESP32-CAM 通信）
 *
 *  ---------------------------------------------------------------------------
 *  库（库管理器自己装，版本别乱升）
 *  ---------------------------------------------------------------------------
 *      lvgl 8.4.0            TFT_eSPI 2.5.43        Adafruit AMG88xx
 *      ArduinoJson 6/7 均可  WiFiManager 2.0.x       （INA219 现在不装也能编译）
 * ============================================================================
 */

/* 必须最先定义：让 LVGL 用 sketch 目录里的 lv_conf.h（8.x 配置）*/
#define LV_CONF_INCLUDE_SIMPLE

#include <Arduino.h>

#include "fan_config.h"
#include "fan_types.h"
#include "config_loader.h"        /* 运行期配置：/config.json 覆盖 fan_config.h 的默认值 */

#include "thermal_sensor.h"
#include "cam_link.h"
#include "stepper_drive.h"
#include "fan_pwm.h"
#include "power_monitor.h"
#include "ui_tft.h"
#include "net_web.h"
#include "version.h"
/* 主循环吃 LVGL + WebServer，默认 8KB 栈不够用 */
SET_LOOP_TASK_STACK_SIZE(LOOP_STACK_SIZE);

/* ============================================================================
 * 全局状态（唯一的两个实例，声明在 fan_types.h）
 * ========================================================================== */
FanCmd g_cmd;
FanRt  g_rt;

/* ============================================================================
 * 本地变量
 * ========================================================================== */
static uint32_t lastStateMs   = 0;
static uint32_t lastPowerMs   = 0;
static uint32_t lastReportMs  = 0;
static uint32_t appliedTimerMin = 0xFFFFFFFF;
static uint8_t  appliedOscSpan = 0xFF;      /* 0xFF = 还没下发过 */
static bool     pendingVersionCheck = false;  /* debug update 排的队，loop 里执行 */
static bool     pendingModelDownload = false; /* debug model update 排的队，loop 里执行 */

/* ============================================================================
 * 工具
 * ========================================================================== */
/* 串口日志用的中文模式名 */
static const char *modeNameCn(SysMode m)
{
    return (m <= M_BLOW) ? MODE_NAME_CN[m] : "?";
}

/* 摇头是否真的要扫：开关开着、且幅度不是 0。幅度 0 在网页语义里就等于“关”。 */
static bool sweepWanted()
{
    return g_cmd.osc && g_cmd.oscSpan > 0;
}

/* 开摇头：先走回中位（pos = 0），到了中位再往两边扫。
   —— 需求：每次开摇头都从中间出发，左右各 span/2，转动方式不变。
   幂等，可以每拍调：归中途中什么都不做，到中位才开始扫。 */
static void startSweepCentered()
{
    if (stepmot::isSweeping()) return;
    if (stepmot::isMoving())   return;             /* 还在归中，等它走完 */
    if (!stepmot::atCenter()) {
        Serial.printf("[电机] 先归中：当前 %+.1f° → 0°\n", stepmot::angleDeg());
        stepmot::goCenter();
        return;
    }
    Serial.printf("[电机] 开始摇头：中位出发，左右各 %.0f°\n", stepmot::sweepSpanDeg() * 0.5f);
    stepmot::startSweep();
}

/* 位置掉电记忆的兜底保存：只有“隔了 STEP_POS_SAVE_MS 且位置真的变了”才写 NVS。
   真正断电前是没机会执行的，所以靠它把损失控制在几分钟的移动之内。 */
static void autosavePos()
{
    static uint32_t lastMs  = 0;
    static long     lastPos = 0;

    if (millis() - lastMs < STEP_POS_SAVE_MS) return;
    lastMs = millis();

    long p = stepmot::posSteps();
    if (labs(p - lastPos) < (long)(2.0f / (STEP_DEG_PER_STEP * 0.5f))) return;  /* 差不到 2° 不写 */
    lastPos = p;
    stepmot::savePosNow();
}

/* 模式切换：所有“进这个模式要做什么”都集中在这里，别散在各处 */
static void enterMode(SysMode m)
{
    if (g_rt.mode == m) return;

    Serial.printf("[模式] %s → %s\n", modeNameCn(g_rt.mode), modeNameCn(m));
    g_rt.mode = m;

    /* 每次切换模式都把当前角度记一下（断电后角度不变，下次上电接着算）。
       savePosNow() 在“值和 NVS 里一样”时完全不写 flash，所以模式来回切也不会磨损 NVS。 */
    if (m != M_BOOT) stepmot::savePosNow();

    switch (m) {
    case M_BOOT:
        stepmot::stop();
        fanpwm::off();
        break;

    case M_OFF:
        stepmot::stop();              /* 断电，电机不发热 */
        fanpwm::off();
        g_rt.lastSeenMs = millis();
        break;

    case M_MANUAL:
        /* 普通风扇：按网页风速常吹，电机只负责扫描（从中间往两边） */
        if (sweepWanted()) startSweepCentered();
        else               stepmot::hold();
        fanpwm::on(g_cmd.speed);
        break;

    case M_SWEEP:
        fanpwm::off();
        if (sweepWanted()) startSweepCentered();
        else               stepmot::hold();
        break;

    case M_ALIGN:
        fanpwm::off();                /* 边转边吹没意义，先停风再对准 */
        stepmot::hold();
        g_rt.alignTries  = 0;
        g_rt.alignWaitMs = millis();
        break;

    case M_BLOW:
        stepmot::hold();              /* 停住，对着目标 */
        fanpwm::on(g_cmd.speed);
        g_rt.lockSinceMs = millis();
        break;
    }
}

/* 双模态判定：返回“此刻是否满足送风条件” */
static bool presenceNow()
{
    bool anyEnabled = false;
    bool allSatisfied = true;

    if (g_cmd.heatOn) {
        anyEnabled = true;
        if (!g_rt.ir.person) allSatisfied = false;
    }
    if (g_cmd.camOn) {
        anyEnabled = true;
        if (!g_rt.cam.person) allSatisfied = false;
    }
    return anyEnabled && allSatisfied;
}

/* ============================================================================
 * 指令落地（网页下发 → 硬件），在 loop 里做，不在 HTTP 回调里做
 * ========================================================================== */
static void applyCommand()
{
    g_cmd.dirty = false;

    /* --- 定时：只有“值变了”才重置倒计时，否则网页每发一次包就续命一次 --- */
    if (g_cmd.timerMin != appliedTimerMin) {
        appliedTimerMin = g_cmd.timerMin;
        g_rt.timerDeadlineMs = g_cmd.timerMin
                             ? (millis() + g_cmd.timerMin * 60000UL)
                             : 0;
        if (g_cmd.timerMin) {
            Serial.printf("[定时] 定时 %u 分钟后关机\n", g_cmd.timerMin);
        } else {
            Serial.println(F("[定时] 取消定时"));
        }
    }

    /* --- 关机顺便把定时和摄像头一起收掉 --- */
    if (!g_cmd.power) {
        g_cmd.timerMin       = 0;
        appliedTimerMin      = 0;
        g_rt.timerDeadlineMs = 0;
    }

    fanpwm::setNature(g_cmd.nature);
    /* 摄像头只在“开了识别 + 风扇总开关开着”时才让 CAM 跑推理（省电、少发热）*/
    camlink::setEnable(g_cmd.camOn && g_cmd.power);

    /* --- 摇头幅度：值变了才重设扫描窗口（0 = 不扫，等同摇头关） --- */
    if (g_cmd.oscSpan != appliedOscSpan) {
        appliedOscSpan = g_cmd.oscSpan;
        stepmot::setSweepSpan((float)appliedOscSpan);
        if (appliedOscSpan == 0) {
            Serial.println(F("[电机] 摇摆范围 0° → 不扫描"));
        } else {
            Serial.printf("[电机] 摇摆范围 %u°（左右各 %.0f°）\n",
                          (unsigned)appliedOscSpan, appliedOscSpan / 2.0f);
        }
    }
}

/* ============================================================================
 * 串口调试指令（一行一条，大小写都认，多空格没关系）
 *   debug amg8833 true    → 打开 GET /amg8833（返回 8x8 原始温度 JSON）
 *   debug amg8833 false   → 关闭该接口，之后访问返回 404
 *   debug aim [flip|1|-1] → 看/翻转对准方向
 *   debug pos [reset|save]→ 看角度 / 把当前位置记为中位 / 立即存 NVS
 *   debug model           → 看模型状态（来源、大小、是否已加载）
 *   debug model update    → 联网下载 /amg_model_int8.tflite 并热重载
 *   debug model reload    → 只重读 LittleFS 里的模型文件并重载（不联网）
 *   debug update          → 联网检查固件版本
 * ========================================================================== */
static void strLower(char *s)
{
    for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

static bool wordIsTrue(const char *w)
{
    return !strcmp(w, "true") || !strcmp(w, "1") || !strcmp(w, "on") || !strcmp(w, "yes");
}

static bool wordIsFalse(const char *w)
{
    /* "fasle" 是常见手滑，顺手认一下 */
    return !strcmp(w, "false") || !strcmp(w, "fasle")
        || !strcmp(w, "0") || !strcmp(w, "off") || !strcmp(w, "no");
}

static void printDebugHelp()
{
    Serial.println(F("— 调试指令 —"));
    Serial.println(F("  debug amg8833 true    打开 /amg8833 原始温度接口"));
    Serial.println(F("  debug amg8833 false   关闭该接口"));
    Serial.println(F("  debug aim             看对准方向系数（+1 正向 / -1 反向）"));
    Serial.println(F("  debug aim flip        翻转对准方向（转向反了就敲它）"));
    Serial.println(F("  debug aim 1 / -1      直接指定方向"));
    Serial.println(F("  debug pos             看当前角度 / NVS 里记的角度"));
    Serial.println(F("  debug pos reset       把当前位置记为中位（摆正后校正用）"));
    Serial.println(F("  debug pos save        立即把当前角度写入 NVS"));
    Serial.println(F("  debug update          联网检查固件版本（下一拍执行）"));
    Serial.println(F("  debug model           看模型状态（来源/大小/是否已加载）"));
    Serial.println(F("  debug model update    联网下载最新模型并热重载"));
    Serial.println(F("  debug model reload    只重读本地模型文件（不联网）"));
}

static void handleSerialLine(char *line)
{
    strLower(line);

    char *save = nullptr;
    char *a = strtok_r(line, " \t", &save);
    if (!a) return;

    if (strcmp(a, "debug") != 0) {
        Serial.printf("[指令] 不认识：%s\n", a);
        printDebugHelp();
        return;
    }

    char *b = strtok_r(nullptr, " \t", &save);
    char *c = strtok_r(nullptr, " \t", &save);
    if (!b) { printDebugHelp(); return; }

    /* ---- debug amg8833 true|false ---- */
    if (strcmp(b, "amg8833") == 0 || strcmp(b, "amg") == 0) {
        if (!c) {
            Serial.printf("[调试] amg8833 原始数据接口：%s\n", g_rt.dbgAmg ? "已开启" : "已关闭");
            return;
        }
        if (wordIsTrue(c)) {
            g_rt.dbgAmg = true;
            Serial.println(F("[调试] amg8833 原始数据接口已开启"));
            if (net::wifiOk()) {
                Serial.printf("[调试] 浏览器打开 http://%s/amg8833\n",
                              WiFi.localIP().toString().c_str());
            } else {
                Serial.println(F("[调试] 网络还没连上，连上后再访问 http://<设备IP>/amg8833"));
            }
        } else if (wordIsFalse(c)) {
            g_rt.dbgAmg = false;
            Serial.println(F("[调试] amg8833 原始数据接口已关闭（访问会返回 404）"));
        } else {
            Serial.printf("[指令] 只认 true / false，收到的是：%s\n", c);
        }
        return;
    }

    /* ---- debug aim [flip | 1 | -1] ----
       对准转向反了就敲 flip，立刻生效，不用重烧固件。 */
    if (strcmp(b, "aim") == 0) {
        if (!c) {
            Serial.printf("[调试] 对准方向系数 %+.0f（%s）\n",
                          thermal::getAimSign(),
                          thermal::getAimSign() > 0.0f ? "正向" : "反向");
            return;
        }
        if (strcmp(c, "flip") == 0 || strcmp(c, "toggle") == 0) {
            thermal::flipAimSign();
        } else if (strcmp(c, "1") == 0 || strcmp(c, "+1") == 0) {
            thermal::setAimSign(+1.0f);
        } else if (strcmp(c, "-1") == 0) {
            thermal::setAimSign(-1.0f);
        } else {
            Serial.printf("[指令] aim 只认 flip / 1 / -1，收到的是：%s\n", c);
            return;
        }
        Serial.println(F("[对准] 下次对准就按新方向走（正在送风的话，等下一次发现目标生效）"));
        return;
    }

    /* ---- debug pos [reset | save] ----
       角度靠 NVS 记忆（断电不变）。机械被人碰歪了就摆正再敲 reset。 */
    if (strcmp(b, "pos") == 0) {
        if (c && strcmp(c, "reset") == 0) {
            stepmot::resetCenter();
            Serial.println(F("[电机] 当前位置已记为中位（0°）并写入 NVS"));
        } else if (c && strcmp(c, "save") == 0) {
            stepmot::savePosNow();
            Serial.printf("[电机] 当前角度 %+.1f° 已写入 NVS\n", stepmot::angleDeg());
        } else {
            Serial.printf("[电机] 当前角度 %+.1f°（%ld 步）| NVS 里记着 %+.1f°\n",
                          stepmot::angleDeg(), stepmot::posSteps(), stepmot::loadPosDeg());
            Serial.println(F("[电机] 可敲：debug pos reset（当前位置记为中位）| debug pos save（立即存）"));
        }
        return;
    }
    /* ---- debug model [update | reload] ----
       不带参数 = 看状态（当前跑的是 LittleFS 那份还是内置那份）。
       update   = 联网下载 /amg_model_int8.tflite 并热重载（要先连上 WiFi）。
       reload   = 不联网，只重新读一次 LittleFS 里的文件并重载（测本地替换用）。
       下载是阻塞的（几百毫秒~几秒），所以 update 只排队，真正执行在 loop() 里；
       reload 只读 20 多 KB flash + 分配张量，几十毫秒，直接执行。 */
    if (strcmp(b, "model") == 0) {
        if (!c) {
            thermal::printModelStatus();
            Serial.println(F("[模型] 可敲：debug model update（联网下载并重载）| debug model reload（只重载本地文件）"));
            return;
        }

        if (strcmp(c, "reload") == 0) {
            Serial.println(F("[模型] 重新读取 LittleFS 并重载（不联网）"));
            if (thermal::reloadModel()) Serial.println(F("[模型] 重载完成"));
            else                        Serial.println(F("[模型] 重载失败，已退回阈值法"));
            return;
        }

        if (strcmp(c, "update") == 0 || strcmp(c, "download") == 0 || strcmp(c, "get") == 0) {
            if (!net::wifiOk()) {
                Serial.println(F("[模型] 网络未连接，先连上 WiFi 再下（或者用 debug model reload 测本地文件）"));
                return;
            }
            if (pendingModelDownload) {
                Serial.println(F("[模型] 已经在排队了，等这一拍跑完"));
                return;
            }
            pendingModelDownload = true;
            Serial.println(F("[模型] 已排队，下一拍开始下载…"));
            return;
        }

        Serial.printf("[指令] model 只认 update / reload，收到的是：%s\n", c);
        return;
    }

    /* ---- debug update ----
       fwver::check() 内部是同步 HTTPClient::GET()，联网状态下也要几百毫秒到 1~2 秒。
       直接在串口回调里调，会把这一拍里的 LVGL 刷新、步进、WebServer 全卡住
       （正在扫描时能看见电机顿一下）。所以这里只排队 + 先判 WiFi，
       真正执行放到 loop() 的固定位置，阻塞发生在哪儿心里有数。 */
    if (strcmp(b, "update") == 0) {
        if (!net::wifiOk()) {
            Serial.println(F("[版本] 网络未连接，先连上 WiFi 再试"));
            return;
        }
        if (pendingVersionCheck) {
            Serial.println(F("[版本] 已经在排队了，等这一拍跑完"));
            return;
        }
        pendingVersionCheck = true;
        Serial.println(F("[版本] 已排队，下一拍检查…"));
        return;
    }

    Serial.printf("[指令] 不支持的调试项：%s\n", b);
    printDebugHelp();
}

/* 主循环里每圈叫一次，没数据时只有一次 available() 判断，开销可忽略 */
static void pollSerial()
{
    static char    buf[72];
    static uint8_t len = 0;

    while (Serial.available()) {
        int ch = Serial.read();
        if (ch < 0) break;
        if (ch == '\r') continue;
        if (ch == '\n') {
            buf[len] = '\0';
            if (len) handleSerialLine(buf);
            len = 0;
            continue;
        }
        if (len < sizeof(buf) - 1) buf[len++] = (char)ch;
        /* 收太长的行直接丢掉，避免把栈写穿 */
        else len = 0;
    }
}

/* ============================================================================
 * 状态机（每 STATE_PERIOD_MS 一拍）
 * ========================================================================== */
static void stateTick()
{
    /* ---- 角度兜底保存（内部自己限速，5 分钟一次且位置变了才写）---- */
    autosavePos();

    /* ---- 总开关 ---- */
    if (!g_cmd.power) {
        if (g_rt.mode != M_OFF) enterMode(M_OFF);
        return;
    }

    const bool anySense = (g_cmd.heatOn || g_cmd.camOn);

    /* ---- 摇头开关必须实时生效 ----
       enterMode() 只在一个模式「被切进来」的那一瞬间读 g_cmd.osc，
       网页中途把摇头关掉时，正在扫描的电机不会自己停（这就是“关了还在动”）。
       这里每拍兜一次底：摇头关了就把扫描停掉，停在当前角度。
       自动模式里“发现人之后的对准”是产品功能，不受摇头开关影响。 */
    if (!sweepWanted() && stepmot::isSweeping()) {
        stepmot::hold();
        Serial.println(F("[电机] 摇头关闭 → 停在当前角度"));
    }
    /* 反过来：摇头重新打开（或幅度从 0 拉回正数），且正处于“找人”模式，就把扫描接上。
       注意这里是“先归中再扫”：startSweepCentered() 会先走回 pos=0。 */
    if (sweepWanted() && g_rt.mode == M_SWEEP && !stepmot::isSweeping()) {
        startSweepCentered();
    }

    /* ---- 上电/关机后的第一拍：选一个起步模式 ---- */
    if (g_rt.mode == M_OFF || g_rt.mode == M_BOOT) {
        enterMode(anySense ? M_SWEEP : M_MANUAL);
        return;
    }

    /* ---- 手动模式：双模态都关，就是一台普通风扇 ---- */
    if (!anySense) {
        if (g_rt.mode != M_MANUAL) enterMode(M_MANUAL);
        else if (g_cmd.speed == 0) fanpwm::off();
        else                       fanpwm::on(g_cmd.speed);
        /* 扫描开关实时跟随网页：同样是先归中再扫 */
        if (sweepWanted() && !stepmot::isSweeping()) startSweepCentered();
        return;
    }

    /* ---- 自动模式 ---- */
    const bool present = presenceNow();
    if (present) g_rt.lastSeenMs = millis();

    const bool lost = (millis() - g_rt.lastSeenMs > TARGET_LOST_MS);
    if (lost) {
        if (g_rt.mode != M_SWEEP) enterMode(M_SWEEP);
        return;
    }

    switch (g_rt.mode) {
    case M_SWEEP:
        /* 刚发现目标：能测方位就先对准，否则直接吹 */
        if (g_cmd.heatOn) enterMode(M_ALIGN);
        else              enterMode(M_BLOW);
        break;

    case M_ALIGN: {
        /* 对准途中网页把热源追踪关掉了：没有方位数据可依据，直接送风别再等 */
        if (!g_cmd.heatOn) { enterMode(M_BLOW); break; }

        /* 电机停稳 + 传感器热平衡之后再取角度，动着的读数会拖影 */
        if (millis() - g_rt.alignWaitMs < IR_SETTLE_MS) break;
        if (!stepmot::settledFor(IR_SETTLE_MS))        break;

        float err  = thermal::angleDeg();
        float aerr = fabsf(err);

        /* 两种收工：误差进死区，或者修正次数用完了（最大 IR_ALIGN_MAX_TRIES 次，别一直甩） */
        if (aerr <= IR_DEADBAND_DEG) {
            Serial.printf("[对准] 已对准（剩余偏差 %+.1f°），转送风\n", err);
            enterMode(M_BLOW);
        } else if (g_rt.alignTries >= IR_ALIGN_MAX_TRIES) {
            Serial.printf("[对准] 已修正 %u 次，仍偏 %+.1f°，不再追，直接送风\n",
                          (unsigned)g_rt.alignTries, err);
            enterMode(M_BLOW);
        } else {
            /* 单次最多挪 IR_ALIGN_STEP_DEG，防一下子甩过头；方向由 thermal::aimSign 决定 */
            stepmot::aimBy(err, IR_ALIGN_STEP_DEG);
            g_rt.alignTries++;
            g_rt.alignWaitMs = millis();
            Serial.printf("[对准] 偏差 %+.1f°，第 %u/%u 次修正（%s %.1f°）\n",
                          err, (unsigned)g_rt.alignTries, (unsigned)IR_ALIGN_MAX_TRIES,
                          err > 0 ? "正转" : "反转",
                          fabsf(err) > IR_ALIGN_STEP_DEG ? IR_ALIGN_STEP_DEG : fabsf(err));
        }
        break;
    }

    case M_BLOW:
        /* 风速在吹的过程中被网页改了，立刻生效 */
        if (g_cmd.speed == 0) fanpwm::off();
        else                  fanpwm::on(g_cmd.speed);

#if IR_REALIGN_ENABLE
        /* 可选：人走动了就重新对准（默认关，因为送风本身会扰动红外画面）*/
        if (g_cmd.heatOn && (millis() - g_rt.lockSinceMs > 2000)
            && stepmot::settledFor(800)
            && fabsf(thermal::angleDeg()) > IR_REALIGN_DEG) {
            enterMode(M_ALIGN);
        }
#endif
        break;

    default:
        break;
    }
}

/* ============================================================================
 * 开机
 * ========================================================================== */
/* 校准进度回调：屏幕上那行字 */
static void calibProgress(uint8_t pct)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "AMG CALIBRATING\n%d%%", (int)pct);
    ui::overlayText(buf);
}

/* ============================================================================
 * 开机网络提示（booting 之后、进 UI 之前）
 * ----------------------------------------------------------------------------
 *  有 IP → 显示“已连网 + 开 http://<IP>/ or http://fan.local/”，两个入口都给；
 *  没连上 → 显示“请连网 + 热点名/密码/192.168.4.1”，引导去配网。
 *  提示停留 3 秒后自动进入 UI，期间照常跑 LVGL，屏幕不会僵住。
 *
 *  ★ 文案只用了字库里有的字：font_cn 是 83 个汉字的子集，
 *    “配 / 置 / 络 / 浏 / 览 / 器 / 点 / 密 / 码”都不在里面，
 *    所以写成“请连网”而不是“请配置网络”——缺的字 LVGL 会画成空白。
 * ========================================================================== */
static void bootNetOverlay()
{
    /* 已保存过 WiFi：给它一点时间去连；连上了就立刻往下走。
       注意 wifiOk() 现在读的是实时状态（WiFi.status()），不再看 service() 里的缓存标志，
       所以这里判断是准的 —— 以前这一段永远读到 false，连上了还报“请连网”。
       循环里还要 net::pump()：WiFiManager 在非阻塞模式下得靠它推进。 */
    ui::overlayText("连网中");
    uint32_t t0 = millis();
    while (!net::wifiOk() && !net::portalOn() && (millis() - t0 < BOOT_WIFI_WAIT_MS)) {
        net::pump();
        ui::tick();
        delay(10);
    }

    /* 提示停留 3 秒。停留期间联网状态一变就立刻改文案 ——
       有些路由器 DHCP 慢，2~3 秒才拿到 IP，不能先斩后奏地写“请连网”。 */
    char     buf[160];
    int8_t   lastSt = -1;             /* -1 未定，0 未连，1 已连 */
    uint32_t t1 = millis();
    do {
        int8_t st = net::wifiOk() ? 1 : 0;
        if (st != lastSt) {
            lastSt = st;
            if (st) {
                String ip = WiFi.localIP().toString();
                /* 两个入口都给出来：IP 一定可用；fan.local 走 mDNS（Windows 需装 Bonjour，
                   安卓/iOS 原生支持）。字库没有「或」，所以用 ASCII 的 or —— 它在 0x20-0x7E 里。 */
                snprintf(buf, sizeof(buf), "已连网\n开 http://%s/\nor http://%s.local/",
                         ip.c_str(), MDNS_NAME);
                Serial.printf("[网页] 浏览器打开 http://%s/ 或 http://%s.local/\n",
                              ip.c_str(), MDNS_NAME);
            } else {
                snprintf(buf, sizeof(buf), "请连网\nWiFi %s\nPWD %s\nhttp://192.168.4.1",
                         cfg::apSsid(), cfg::apPass());
                /* 串口说清楚到底在哪种状态：热点已经开了，还是还在连、等待超时才开 */
                if (net::portalOn()) {
                    Serial.printf("[网页] 配网热点已开：%s / %s → http://192.168.4.1\n",
                                  cfg::apSsid(), cfg::apPass());
                } else {
                    Serial.printf("[网页] 还没连上：%d 秒内连不上会自动开热点 %s / %s\n",
                                  (int)WIFI_LINK_TIMEOUT, cfg::apSsid(), cfg::apPass());
                }
            }
            ui::overlayText(buf);
        }
        net::pump();
        ui::tick();
        delay(10);
    } while (millis() - t1 < 3000);
}

void setup()
{
    Serial.begin(DEBUG_BAUD);
    delay(200);

    /* ---- 默认状态：关机，等网页/按键来开 ---- */
    memset(&g_cmd, 0, sizeof(g_cmd));
    memset(&g_rt, 0, sizeof(g_rt));
    g_cmd.power  = false;
    g_cmd.speed  = 40;
    g_cmd.osc    = true;
    g_cmd.oscSpan = (uint8_t)OSC_SPAN_DEFAULT;
    g_cmd.heatOn = false;
    g_cmd.camOn  = false;
    g_rt.mode    = M_BOOT;
    g_rt.lastSeenMs = millis();

    Serial.println();
    Serial.println(F("=============================================="));
    Serial.println(F("  双模态智能定向风扇 · ESP32-S3 主控"));
    Serial.printf ("  内核 %d.%d.%d | 界面库 LVGL %d.%d.%d | 屏驱 TFT_eSPI %s\n",
                   ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH,
                   LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
                   TFT_ESPI_VERSION);
    Serial.printf ("  红外 I2C %d/%d | 步进 %d %d %d %d | 风机 %d | 摄像头串口 %d/%d\n",
                   PIN_I2C_SDA, PIN_I2C_SCL,
                   PIN_STEP_A, PIN_STEP_B, PIN_STEP_C, PIN_STEP_D,
                   PIN_FAN_PWM, PIN_CAM_TX, PIN_CAM_RX);
    Serial.printf ("  红外识别：%s（类别 %s 才判定有人，阈值 %.2f）\n",
                   IR_USE_MODEL ? "TFLite 模型 · 按需加载（开热源追踪才加载并推理）" : "阈值法",
                   IR_MODEL_MAN_CLASS, IR_MODEL_CONF_MIN);
    Serial.println(F("  串口指令：debug amg8833 true|false（原始温度接口）、debug aim flip（翻转对准方向）、"));
    Serial.println(F("            debug model（模型状态 / update 下载 / reload 重载）、debug update（查固件版本）"));
    Serial.println(F("=============================================="));

    /* ---- 0.5 运行期配置 ----
       读 LittleFS 里的 /config.json，覆盖上面刚设好的 g_cmd 默认值。
       位置很讲究：必须在 net::begin() 之前 —— 配网热点的 SSID/密码要用 cfg 里的值。
       这时 LittleFS 还没被 net::begin() 挂载也没关系，cfg::load() 自己会 begin(false)；
       挂不上（分区没格式化过）就全部走宏默认值，不会因此卡住启动。 */
    cfg::load();

    /* ---- 1. 执行机构先就位：上电即安全（风机 0、电机断电）---- */
    stepmot::begin();
    fanpwm::begin();
    /*param*/
    if (psramFound()) {
        Serial.printf("PSRAM 已就绪! 大小: %d MB\n", ESP.getPsramSize() / (1024 * 1024));
    } else {
        Serial.println("PSRAM 未找到，设置错了！");
    }
    /* ---- 2. 屏幕 ---- */
    if (!ui::begin()) {
        Serial.println(F("[屏幕] 屏幕初始化失败，继续跑（只是没显示）"));
    }
    ui::overlayText("BOOTING...");

    /* ---- 3. 红外：I2C + 校准 3 秒（需求：开机前 3 秒取 8x8 平均作为背景）---- */
    bool irOk = thermal::begin();
    if (!irOk) {
        Serial.println(F("[红外] 没找到 AMG8833，检查 4/5 脚与供电（红外功能不可用）"));
        ui::overlayText("AMG8833 NOT FOUND");
        delay(1500);
    } else {
        Serial.printf("[红外] 就绪，开始 %.1fs 背景校准…\n", IR_CALIB_MS / 1000.0f);
        if (!thermal::calibrate(IR_CALIB_MS, calibProgress)) {
            Serial.println(F("[红外] 校准失败（一帧都没读到）"));
            ui::overlayText("CALIBRATION FAILED");
            delay(1200);
        } else {
            Serial.printf("[红外] 背景温度 %.2f ℃（校准完成）\n", g_rt.bgMean);
            char b[48];
            snprintf(b, sizeof(b), "BACKGROUND\n%.2f C", g_rt.bgMean);
            ui::overlayText(b);
            delay(700);
        }
    }

    /* ---- 4. 摄像头链路 + 功率模块 ---- */
    camlink::begin();
    power::powerInit();

    /* ---- 5. 联网（非阻塞：配好热点立刻返回，不影响上面的硬件）---- */
    /* 先把 WiFi/lwIP 栈拉起来。core 3.x 的 lwIP core-lock 信号量是 tcpip_init() 时创建的，
       在那之前任何 socket 调用都会拿着 NULL 句柄进 xQueueSemaphoreTake → assert 重启。 */
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    net::begin();

    /* ---- 6. 开机提示：网页地址 / 请连网，3 秒后进 UI ---- */
    bootNetOverlay();
    ui::overlayHide();

    Serial.printf("[系统] 启动完成 | 空闲内存 %u 字节 | 背景温度 %.2f℃\n",
                  (unsigned)ESP.getFreeHeap(), g_rt.bgMean);
    Serial.println(F("[系统] 串口可用指令：debug amg8833 true|false、debug aim flip、debug model [update|reload]、debug update"));
}

/* ============================================================================
 * HTTP 服务延迟启动
 *   net::begin() 里只注册路由、不 begin()（原因见 net_web.h 那段注释）：
 *     - lwIP 没初始化时 socket 会崩；
 *     - 配网阶段端口 80 归 WiFiManager 的配网页。
 *   等 WiFi 真的拿到 IP，再由这里开始监听。
 * ========================================================================== */
static void netStartServerOnce()
{
    static bool started = false;
    if (started || !net::wifiOk()) return;

    net::server.begin();
    started = true;
    Serial.printf("[网页] HTTP 已启动 → http://%s/  (mDNS: http://%s.local)\n",
                  WiFi.localIP().toString().c_str(), MDNS_NAME);
}

/* ============================================================================
 * 主循环
 *   不写任何 delay 超过 1ms 的东西：LVGL 要 5ms 内的刷新节拍，
 *   WebServer 要尽快返回，步进电机要稳定节拍 —— 全在这里靠时间片轮转。
 * ========================================================================== */
void loop()
{
    uint32_t t0 = micros();

    /* ---- 1. 屏幕（LVGL 心跳+渲染） ---- */
    ui::tick();

    /* ---- 1.5 串口调试指令（debug amg8833 true|false）---- */
    pollSerial();

    /* ---- 2. 各外设服务：都是非阻塞的，叫一次走一小步 ---- */
    camlink::service();
    stepmot::service();
    fanpwm::service();
    netStartServerOnce();        /* WiFi 一连上就把 80 端口的服务拉起来 */
    net::service();

    /* ---- 3. 红外采样（内部限速 IR_POLL_MS）---- */
    thermal::poll();

    /* ---- 4. 网页指令落地 ---- */
    if (g_cmd.dirty) applyCommand();

    /* ---- 4.5 排队的版本检查（debug update 触发）----
       放在这里而不是串口回调里：阻塞点固定在 loop 的同一处，
       前面屏幕/电机/网页都刚服务过，卡一下也不至于把哪一项拖坏。
       跑完补一次 ui::tick()，让 LVGL 把这段时间的节拍一次补上，画面不会定格。 */
    if (pendingVersionCheck) {
        pendingVersionCheck = false;
        fwver::check();
        ui::tick();
    }

    /* ---- 4.6 排队的手动模型下载（debug model update 触发）----
       下载完立刻热重载：不重载的话文件换了内存里还是旧模型，推理不会有任何变化。 */
    if (pendingModelDownload) {
        pendingModelDownload = false;
        Serial.println(F("[模型] 开始下载…"));
        if (fwver::downloadAndReplaceFile(fwver::kDownload, "/amg_model_int8.tflite")) {
            Serial.println(F("[模型] 下载成功，正在热重载"));
            thermal::reloadModel();
        } else {
            Serial.println(F("[模型] 下载或替换失败，当前模型保持不变"));
        }
        ui::tick();
    }

    /* ---- 5. 状态机 ---- */
    if (millis() - lastStateMs >= STATE_PERIOD_MS) {
        lastStateMs = millis();
        stateTick();
    }

    /* ---- 6. 功率检测（预留，目前返回假数据）---- */
    if (millis() - lastPowerMs >= POWER_PERIOD_MS) {
        lastPowerMs = millis();
        power::updatePowerData();
    }

    /* ---- 7. 定时关机 ---- */
    if (g_rt.timerDeadlineMs && millis() >= g_rt.timerDeadlineMs) {
        Serial.println(F("[定时] 定时到，关机"));
        g_rt.timerDeadlineMs = 0;
        g_cmd.timerMin       = 0;
        appliedTimerMin      = 0;
        g_cmd.power          = false;
        /* 这里不走 applyCommand（不是网页下发的），CAM 得自己关一下，别关机了还在跑推理 */
        camlink::setEnable(false);
    }

    /* ---- 8. 运行时快照 + 刷屏 ---- */
    g_rt.motorAngleDeg = stepmot::angleDeg();
    g_rt.motorMoving   = stepmot::isMoving();
    g_rt.uptimeMs      = millis();
    ui::refresh();

    /* loop 耗时统计：一旦 >20ms，网页会开始卡，说明哪里阻塞了 */
    uint32_t dt = micros() - t0;
    if (dt > g_rt.loopMaxUs) g_rt.loopMaxUs = dt;

    /* ---- 9. 每 5 秒一行串口状态，调机器人用 ---- */
    if (millis() - lastReportMs >= 5000) {
        lastReportMs = millis();
        Serial.printf("[%6lus] %s | 红外：%s[%s %.2f] 温升%.1f℃ 热区%u点 偏角%+.0f°"
                      " | 摄像头：%s(%s) 帧%lu | 转角%+.0f° | 风速%u%% | %s | 空闲内存%u 字节\n",
                      (unsigned long)(millis() / 1000),
                      modeNameCn(g_rt.mode),
                      !thermal::trackingWanted() ? "停用"
                                                 : (g_rt.ir.person ? "有人" : "无人"),
                      thermal::className(), g_rt.ir.clsConf,
                      g_rt.ir.maxDelta, (unsigned)g_rt.ir.hot, g_rt.ir.angleDeg,
                      g_rt.cam.person ? "有人" : "无人",
                      g_rt.cam.online ? "在线" : "离线",
                      (unsigned long)g_rt.cam.frames,
                      g_rt.motorAngleDeg,
                      (unsigned)g_rt.fanPct,
                      net::wifiOk() ? "网络已连接" : (net::portalOn() ? "热点配网中" : "网络未连"),
                      (unsigned)ESP.getFreeHeap());
        g_rt.loopMaxUs = 0;
    }

    delay(1);   /* 让出 CPU 给 WiFi 协议栈，1ms 足够 */
}
