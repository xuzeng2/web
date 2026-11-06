/**
 * ============================================================================
 *  power_monitor.h  —  INA219 功率检测（未接模块时按功能估算）
 * ----------------------------------------------------------------------------
 *  需求：只用预留接口 + 函数框架。
 *
 *  现在没接 INA219，所以数据是**估算**的：把常驻功耗当底，再按当前开着的功能
 *  （风机档位、步进通电、摄像头、红外）一段段加上去，最后叠一个慢速小波动，
 *  让人看不出是假的、也不会一秒一个数乱跳。
 *
 *  接上模块后要做的事，都写在下面 TODO 里（含现成代码，取消注释即可用），
 *  估算逻辑整段删掉即可，`pdata` 的结构不用改。
 *
 *  ★ 接线（别接错，INA219 串在电源正极上）：
 *        适配器 +  →  INA219 VIN+       INA219 VIN-  →  风机/MOS 电源正极
 *        INA219 VCC → 3.3V   GND → GND   SDA/SCL → 与 AMG8833 同一根 I2C 总线
 *     地址：0x40（默认，A0/A1 都不接）
 *     注意：裸 INA219 模块的“分流电阻”是 0.1Ω / 2W，走 1A 时压降 0.1V、功耗 0.1W，
 *           接 12V 风机没问题；接大电流（>2A）前先确认模块的量程。
 * ============================================================================
 */
#ifndef POWER_MONITOR_H
#define POWER_MONITOR_H

#include <Wire.h>

#include "fan_config.h"
#include "fan_types.h"
#include "stepper_drive.h"     /* 功率估算要看步进是否通电（stepmot::isEnergized）*/

/* 0 = 按功能估算（假数据）；1 = 真的去读 INA219 */
#define POWER_USE_INA219 0

#if POWER_USE_INA219
  #include <Adafruit_INA219.h>
  static Adafruit_INA219 ina219(0x40);
#endif

namespace power {

struct PowerData {
    float busV;        /* 母线电压 V */
    float shuntMv;     /* 分流电阻压降 mV */
    float currentMa;   /* 电流 mA */
    float powerMw;     /* 功率 mW */
    bool  valid;       /* true = 真实读数；false = 估算值/读取失败 */
};

/* 初值就是“关机待机”的估算：主板 + 屏 + WiFi + 传感器 */
static PowerData pdata = { 12.00f, 12.0f, 120.0f, 1440.0f, false };

/* ---------------------------------------------------------------------------
 * 估算用的参数（12V 母线侧，单位 mA）
 *   都是按常见模块估的量级，不是实测。以后接上 INA219，直接把实测值填进来
 *   就能让估算更准；要彻底换真值，把 updatePowerData() 里的估算段删掉。
 * ------------------------------------------------------------------------- */
static constexpr float BUS_V         = 12.0f;   /* 母线电压 */
static constexpr float MA_BASE       = 120.0f;  /* 主板 + 屏 + WiFi + 传感器常驻 */
static constexpr float MA_FAN_MAX    = 950.0f;  /* 风机满档（12V 小风扇约 10W 级）*/
static constexpr float MA_MOTOR_RUN  = 110.0f;  /* 28BYJ-48 转动（经 ULN2003）*/
static constexpr float MA_MOTOR_HOLD = 85.0f;   /* 锁定通电（不转，但线圈仍带电）*/
static constexpr float MA_CAM        = 140.0f;  /* ESP32-CAM 开识别 */
static constexpr float MA_IR         = 30.0f;   /* AMG8833 + 模型推理 */

/* 波动：3~6 秒换一次系数，幅度 ±5%，肉眼像负载轻微起伏 */
static constexpr uint32_t JITTER_MIN_MS = 3000;
static constexpr uint32_t JITTER_MAX_MS = 6000;
static constexpr float    JITTER_SPAN   = 0.05f;

static uint32_t jitterNextMs = 0;      /* 下次换系数的时间点 */
static float    jitter       = 1.0f;   /* 当前波动系数（1±JITTER_SPAN）*/

/* 按“现在开着什么”把功耗加起来 */
static float estimateMa()
{
    float ma = MA_BASE;

    /* 风机：档位越高端流涨得越快，按 1.7 次方近似（低档比线性更省） */
    if (g_cmd.power && g_rt.fanPct > 0) {
        float k = (float)g_rt.fanPct / 100.0f;
        ma += MA_FAN_MAX * powf(k, 1.7f);
    }

    /* 步进：转着 / 锁定通电都耗电；stop() 之后线圈断电，不耗 */
    if (stepmot::isEnergized()) {
        ma += stepmot::isMoving() ? MA_MOTOR_RUN : MA_MOTOR_HOLD;
    }

    /* 摄像头、红外：和它们各自的使能条件保持一致（开了功能 + 没关机才跑）*/
    if (g_cmd.power && g_cmd.camOn) ma += MA_CAM;
    if (g_cmd.power && g_cmd.heatOn) ma += MA_IR;

    return ma;
}

/* ---------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------- */
static void powerInit()
{
#if POWER_USE_INA219
    /* TODO(INA219)：正式启用时把 POWER_USE_INA219 改成 1，并把这里注释打开。
     *
     *   if (!ina219.begin(&Wire)) {
     *       Serial.println(F("[功率] 未找到，功率数据将保持估算值"));
     *       pdata.valid = false;
     *       return;
     *   }
     *   // 常见模块是 0.1Ω / 2A：量程按实际负载选，风机启动瞬间电流会冲高
     *   ina219.setCalibration_32V_2A();
     */
    Serial.println(F("[功率] POWER_USE_INA219 = 0，功率数据为估算值"));
#else
    pdata.currentMa = MA_BASE;
    pdata.busV      = BUS_V;
    pdata.shuntMv   = MA_BASE * 0.1f;
    pdata.powerMw   = pdata.busV * pdata.currentMa;
    pdata.valid     = false;
    jitterNextMs    = millis() + 1000;
    Serial.println(F("[功率] 未接 INA219 → 按功能估算（风机/步进/摄像头/红外逐项累加）"));
#endif
}

/* ---------------------------------------------------------------------------
 * 主循环周期调用（POWER_PERIOD_MS）
 * ------------------------------------------------------------------------- */
static void updatePowerData()
{
    /* TODO(INA219)：真实读数就这几行，启用后替换掉下面的估算。
     *
     *   float busV   = ina219.getBusVoltage_V();
     *   float shuntMv = ina219.getShuntVoltage_mV();
     *   float curMa  = ina219.getCurrent_mA();
     *   float pwMw   = ina219.getPower_mW();
     *   if (isnan(curMa) || isnan(pwMw)) { pdata.valid = false; return; }
     *   pdata.busV      = busV;
     *   pdata.shuntMv   = shuntMv;
     *   pdata.currentMa = curMa;
     *   pdata.powerMw   = pwMw;      // 也可以自己算：busV * curMa
     *   pdata.valid     = true;
     *
     * TODO(可选)：
     *   · 加 5 点滑动平均，TFT 上的数字不会跳
     *   · 风机启动瞬间（前 300ms）电流尖峰大，可跳过不采
     *   · 做过流保护：currentMa > 阈值 就 fanpwm::off() 并在网页报警
     */

    uint32_t now = millis();

    /* ---- 慢速波动：3~6 秒换一次系数，一次只抖 ±5% ---- */
    if ((int32_t)(now - jitterNextMs) >= 0) {
        int   r   = (int)random(-100, 101);                     /* -100..100 */
        jitter    = 1.0f + (float)r / 100.0f * JITTER_SPAN;
        jitterNextMs = now + (uint32_t)random((long)JITTER_MIN_MS,
                                             (long)JITTER_MAX_MS + 1L);
    }

    float target = estimateMa() * jitter;

    /* ---- 平滑跟随：功能一开一关电流是“爬”过去的，不是瞬跳 ---- */
    if (pdata.currentMa <= 0.0f) pdata.currentMa = target;
    else                         pdata.currentMa += (target - pdata.currentMa) * 0.35f;
    if (pdata.currentMa < 0.0f) pdata.currentMa = 0.0f;

    /* ---- 电压 / 分流压降 / 功率都由电流推出来，三个数互相自洽 ---- */
    float amps = pdata.currentMa / 1000.0f;
    pdata.busV    = BUS_V - amps * 0.15f;          /* 带载轻微压降，约 0.15V/A */
    pdata.shuntMv = pdata.currentMa * 0.1f;        /* 0.1Ω 分流：1A = 100mV */
    pdata.powerMw = pdata.busV * pdata.currentMa;
    pdata.valid   = false;                         /* 仍是估算，不是实测 */
}

} /* namespace power */

#endif /* POWER_MONITOR_H */
