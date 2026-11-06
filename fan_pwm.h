/**
 * ============================================================================
 *  fan_pwm.h  —  风机 PWM 输出
 * ----------------------------------------------------------------------------
 *  硬件：GPIO47 出一路 25kHz PWM，接 MOS 管栅极（栅极串 100Ω、对地并 10k 下拉）。
 *        风机单独供电，别从板子 3.3V 取电。
 *
 *  API 只有三个动词，状态机好写：
 *      on(pct)   吹，百分比
 *      off()     停
 *      service() 主循环里叫，负责软启动和自然风起伏
 *
 *  软启动：占空比按 FAN_SOFT_STEP_MS 一档往上爬，避免 MOS 管和电源被冲击，
 *          听感上也不会“啪”一下起来。
 * ============================================================================
 */
#ifndef FAN_PWM_H
#define FAN_PWM_H

#include "fan_config.h"
#include "fan_types.h"

namespace fanpwm {

static uint8_t  targetPct  = 0;      /* 想要的风速 */
static uint8_t  curPct     = 0;      /* 当前实际输出（软启动中间值）*/
static bool     natureOn   = false;
static uint32_t lastRampMs = 0;
static uint32_t lastLfoMs  = 0;
static float    lfoPhase   = 0.0f;

/* ---------------------------------------------------------------------------
 * 底层写占空比
 * ------------------------------------------------------------------------- */
static void writeDuty(uint8_t pct)
{
    if (pct > 100) pct = 100;

    /* 低于启动阈值直接归零：8% 以下 5V 风扇转不起来，只会嗡嗡响发热 */
    uint8_t out = (pct < FAN_MIN_DUTY_PCT) ? 0 : pct;

#if FAN_CORE_V3
    uint32_t duty = ((uint32_t)out * 255U) / 100U;
    ledcWrite(PIN_FAN_PWM, duty);
#else
    uint32_t duty = ((uint32_t)out * ((1U << FAN_PWM_BITS) - 1U)) / 100U;
    ledcWrite(FAN_PWM_CH, duty);
#endif
    g_rt.fanPct  = out;
    g_rt.blowing = (out > 0);
}

/* ---------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------- */
static void begin()
{
#if FAN_CORE_V3
    ledcAttach(PIN_FAN_PWM, FAN_PWM_FREQ, FAN_PWM_BITS);
#else
    ledcSetup(FAN_PWM_CH, FAN_PWM_FREQ, FAN_PWM_BITS);
    ledcAttachPin(PIN_FAN_PWM, FAN_PWM_CH);
#endif
    targetPct = 0;
    curPct    = 0;
    writeDuty(0);
}

/* ---------------------------------------------------------------------------
 * 动作
 * ------------------------------------------------------------------------- */
static void on(uint8_t pct)
{
    targetPct = (pct > 100) ? 100 : pct;
}

static void off()
{
    targetPct = 0;
}

/* 自然风开关（跟随网页上的“自然风”）*/
static void setNature(bool en)
{
    if (natureOn != en) {
        natureOn = en;
        lfoPhase = 0.0f;
    }
}

/* ---------------------------------------------------------------------------
 * 周期调用：软启动 + 自然风
 * ------------------------------------------------------------------------- */
static void service()
{
    uint32_t now = millis();

    /* --- 自然风：6 秒一个来回的正弦，最低到 55% --- */
    float effTarget = (float)targetPct;
    if (natureOn && targetPct > 0) {
        if (now - lastLfoMs >= 50) {
            uint32_t dt = now - lastLfoMs;
            lastLfoMs = now;
            /* 相位步进 = 2π · dt / 周期 */
            lfoPhase += 6.28318f * (float)dt / (float)FAN_NATURE_PERIOD;
            if (lfoPhase > 6.28318f) lfoPhase -= 6.28318f;
        }
        float wave = (sinf(lfoPhase) + 1.0f) * 0.5f;             /* 0..1 */
        effTarget *= FAN_NATURE_LOW + (1.0f - FAN_NATURE_LOW) * wave;
    }

    /* --- 软启动：每 FAN_SOFT_STEP_MS 靠近目标 1~2 档 --- */
    if (now - lastRampMs >= FAN_SOFT_STEP_MS) {
        lastRampMs = now;
        int16_t t = (int16_t)(effTarget + 0.5f);
        if (curPct < t) {
            uint8_t stepUp = (t - curPct > 20) ? 2 : 1;
            curPct = (uint8_t)min((int)t, (int)curPct + stepUp);
        } else if (curPct > t) {
            uint8_t stepDn = (curPct - t > 20) ? 2 : 1;
            curPct = (uint8_t)max((int)t, (int)curPct - stepDn);
        }
        writeDuty(curPct);
    }
}

static uint8_t percent() { return g_rt.fanPct; }

} /* namespace fanpwm */

#endif /* FAN_PWM_H */
