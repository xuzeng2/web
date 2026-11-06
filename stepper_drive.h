/**
 * stepper_drive.h — ESP32-S3 / ULN2003 / 28BYJ-48
 * Non-blocking 8-phase half-step driver with acceleration/deceleration.
 * Retains public APIs: begin, service, stop, hold, startSweep, aimBy,
 * isMoving, isSweeping, angleDeg, settledFor and mode/pos variables.
 *
 * IMPORTANT: STEP_PER_REV and STEP_DEG_PER_STEP in fan_config.h are assumed
 * to describe the ORIGINAL 4-phase/full-step setup (e.g. 2048 and 360/2048).
 * This driver doubles the step count internally; do NOT also double those
 * values in fan_config.h. GPIO wiring is unchanged from the original header.
 *
 * Define STEPMOT_SCAN_RPM / STEPMOT_AIM_RPM externally if required.
 * GPIO19/20 are native USB pins on ESP32-S3. If native USB is used,
 * reassign PIN_STEP_A/B/C/D in fan_config.h to appropriate free pins.
 */
#ifndef STEPPER_DRIVE_H
#define STEPPER_DRIVE_H

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "fan_config.h"
#include "fan_types.h"

#ifndef STEPMOT_SCAN_RPM
#define STEPMOT_SCAN_RPM 8.0f
#endif
#ifndef STEPMOT_AIM_RPM
#define STEPMOT_AIM_RPM 10.0f
#endif
#ifndef STEPMOT_ACCEL_STEPS_S2
#define STEPMOT_ACCEL_STEPS_S2 500.0f
#endif
#ifndef STEPMOT_START_STEPS_S
#define STEPMOT_START_STEPS_S 110.0f
#endif

namespace stepmot {

enum StepMode { SM_STOP = 0, SM_SWEEP, SM_AIM, SM_HOLD };

// Matches the original A/B/C/D order from fan_config.h.
// Original four phases were 0011 -> 1001 -> 1100 -> 0110.
static const uint8_t SEQ[8][4] = {
    {0,0,1,1}, {0,0,0,1}, {1,0,0,1}, {1,0,0,0},
    {1,1,0,0}, {0,1,0,0}, {0,1,1,0}, {0,0,1,0}
};
static const uint8_t PINS[4] = {PIN_STEP_A, PIN_STEP_B, PIN_STEP_C, PIN_STEP_D};

// Backward compatibility: fan_config.h still carries FULL-step counts.
static constexpr long HALF_STEPS_PER_REV = (long)STEP_PER_REV * 2L;
static constexpr float DEG_PER_HALF_STEP = (float)STEP_DEG_PER_STEP * 0.5f;
static_assert(STEP_PER_REV > 0, "STEP_PER_REV must be positive");

static int8_t mode = SM_STOP;
static int8_t dir = +1;
static int8_t phase = 0;
static long pos = 0;
static long sweepLo = 0;
static long sweepHi = 0;
static long aimTarget = 0;
static uint16_t rpm = 8;
static uint32_t lastStepUs = 0;
static uint32_t lastMoveMs = 0;
static uint32_t holdStartMs = 0;
static bool energized = false;
static float currentStepsPerSec = 0.0f;

/* ---- 位置掉电记忆 ----
 *  pos 是相对“中位”（pos = 0）的半步数。28BYJ-48 有 1:64 减速箱，断电后静摩擦
 *  足以大致保持朝向，所以重启时按上次记下的 pos 继续算角度是站得住的；知道
 *  自己在哪，才能“先走回中位再往两边扫”。
 *
 *  NVS 有擦写寿命，所以只在值真的变了、且隔了 STEP_POS_SAVE_MS 才写一次；
 *  关机（进 M_OFF）时无条件写一次。
 * ------------------------------------------------------------------------- */
static Preferences prefs;
static constexpr const char *NVS_NS  = "fan";
static constexpr const char *NVS_POS = "mpos";

/* “NVS 里从来没存过”的哨兵值。用它区分两种情况：
     · 首次上电（用户已经手动把风扇头摆到中间）→ 当前位置就是中位，pos = 0
     · 之前存过 → 按存的角度接着算（断电后机械角度不会变） */
static constexpr int32_t POS_UNKNOWN = INT32_MIN;

/* 读上次存的角度；没存过返回 POS_UNKNOWN */
static int32_t loadPos()
{
    prefs.begin(NVS_NS, true);
    int32_t v = prefs.getInt(NVS_POS, POS_UNKNOWN);
    prefs.end();
    return v;
}

/* 存当前角度；和 NVS 里一样就不写，省擦写次数 */
static void savePosNow()
{
    prefs.begin(NVS_NS, false);
    if (prefs.getInt(NVS_POS, POS_UNKNOWN) != (int32_t)pos)
        prefs.putInt(NVS_POS, (int32_t)pos);
    prefs.end();
}

/* NVS 里记着的角度（度），调角度用 */
static float loadPosDeg() { return (float)loadPos() * DEG_PER_HALF_STEP; }

static float maxStepsPerSec() {
    const float targetRpm = (mode == SM_SWEEP) ? STEPMOT_SCAN_RPM : STEPMOT_AIM_RPM;
    return (float)HALF_STEPS_PER_REV * targetRpm / 60.0f;
}

static void applyPhase(int8_t ph) {
    const uint8_t *s = SEQ[(uint8_t)ph & 7U];
    for (int i = 0; i < 4; ++i) digitalWrite(PINS[i], s[i] ? HIGH : LOW);
}

static void releaseCoils() {
    for (int i = 0; i < 4; ++i) digitalWrite(PINS[i], LOW);
    energized = false;
}

/* 扫描幅度（左右合计，单位度）。0 = 半步都不走；网页滑杆 0..180 直接调它。
   范围变了不去改 pos：电机实际在哪就是在哪，SM_SWEEP 的换向逻辑下一拍会把它带回窗口内。 */
static void setSweepSpan(float totalDeg) {
    if (totalDeg < 0.0f)   totalDeg = 0.0f;
    if (totalDeg > 180.0f) totalDeg = 180.0f;
    const long half = (long)((totalDeg * 0.5f) / DEG_PER_HALF_STEP);
    sweepLo = -half;
    sweepHi = +half;
    if (mode == SM_SWEEP) {
        if (pos > sweepHi)      dir = -1;
        else if (pos < sweepLo) dir = +1;
    }
}

static float sweepSpanDeg() { return (float)(sweepHi - sweepLo) * DEG_PER_HALF_STEP; }

static void begin() {
    for (int i = 0; i < 4; ++i) {
        pinMode(PINS[i], OUTPUT);
        digitalWrite(PINS[i], LOW);
    }
    setSweepSpan(OSC_SPAN_DEFAULT);

    /* ---- 角度：首次上电以“当前位置”为中位，之后按记下的度数继续 ----
       用户的用法是：上电前手动把风扇头摆到正中间，所以第一次上电的当前位置
       就是他想要的中位，pos 记 0 并立刻写进 NVS；断电后机械角度不会变，
       下次就从这个度数接着算，开摇头时先走回 0 再往两边扫。 */
    const long lim = (long)((OSC_SPAN_MAX / 2.0f) / DEG_PER_HALF_STEP);   /* ±90° */
    int32_t saved = loadPos();

    if (saved == POS_UNKNOWN) {
        pos = 0;
        Serial.println(F("[电机] 首次上电：当前位置记为中位 0°（已写入 NVS）"));
        savePosNow();
    } else if (saved < -lim || saved > lim) {
        pos = 0;
        Serial.printf("[电机] NVS 里的角度 %+.1f° 不合理，按中位处理\n",
                      (float)saved * DEG_PER_HALF_STEP);
        savePosNow();
    } else {
        pos = (long)saved;
        Serial.printf("[电机] 恢复上次角度 %+.1f°（断电后位置没变，接着算）\n",
                      (float)pos * DEG_PER_HALF_STEP);
    }

    dir = +1;
    phase = 0;
    aimTarget = pos;
    mode = SM_STOP;
    rpm = (uint16_t)STEPMOT_SCAN_RPM;
    currentStepsPerSec = 0.0f;
    lastStepUs = micros();
    lastMoveMs = millis();
    holdStartMs = 0;
    energized = false;
}

static void doStep(int8_t d) {
    phase = (int8_t)((phase + d + 8) & 7);
    pos += d;
    applyPhase(phase);
    energized = true;
    lastMoveMs = millis();
}

// Kept for users of the previous helper API.
static uint32_t intervalUs() {
    float speed = currentStepsPerSec > 0.0f ? currentStepsPerSec : STEPMOT_START_STEPS_S;
    return (uint32_t)(1000000.0f / speed);
}

static void stop() {
    mode = SM_STOP;
    currentStepsPerSec = 0.0f;
    releaseCoils();
}

static void hold() {
    if (mode == SM_HOLD) return;
    mode = SM_HOLD;
    holdStartMs = millis();
    currentStepsPerSec = 0.0f;
    rpm = (uint16_t)STEPMOT_AIM_RPM;
}

static void startSweep() {
    if (mode != SM_SWEEP) {
        currentStepsPerSec = 0.0f;
        lastStepUs = micros();
    }
    rpm = (uint16_t)STEPMOT_SCAN_RPM;
    /* 从中位出发时固定先往正方向，保证“中间 → 右边 → 左边”这个对称节奏 */
    if (pos == 0)       dir = +1;
    if (pos >= sweepHi) dir = -1;
    if (pos <= sweepLo) dir = +1;
    mode = SM_SWEEP;
}

static void aimBy(float deg, float limitDeg) {
    if (deg > limitDeg) deg = limitDeg;
    if (deg < -limitDeg) deg = -limitDeg;
    const long steps = (long)(deg / DEG_PER_HALF_STEP);
    if (steps == 0) { hold(); return; }
    aimTarget = pos + steps;
    rpm = (uint16_t)STEPMOT_AIM_RPM;
    currentStepsPerSec = 0.0f;
    lastStepUs = micros();
    mode = SM_AIM;
}

// Exactly one half step at most per service() call. Caller must call often
// enough (<~1.5ms at 10RPM) for the requested speed to be attainable.
static void service() {
    if (mode == SM_STOP) return;
    if (mode == SM_HOLD) {
        if (energized && (uint32_t)(millis() - holdStartMs) >= STEP_RELEASE_MS)
            releaseCoils();
        return;
    }

    if (mode == SM_AIM && pos == aimTarget) { hold(); return; }

    const uint32_t now = micros();
    if ((uint32_t)(now - lastStepUs) < intervalUs()) return;
    lastStepUs = now;

    int8_t nextDir = dir;
    long remaining;
    if (mode == SM_AIM) {
        nextDir = (pos < aimTarget) ? +1 : -1;
        remaining = labs(aimTarget - pos);
    } else {
        nextDir = dir;
        remaining = (dir > 0) ? (sweepHi - pos) : (pos - sweepLo);
        if (remaining <= 0) {
            dir = (int8_t)-dir;
            nextDir = dir;
            remaining = (dir > 0) ? (sweepHi - pos) : (pos - sweepLo);
            currentStepsPerSec = 0.0f;
        }
    }

    const float acceleration = STEPMOT_ACCEL_STEPS_S2;
    const float startSpeed = STEPMOT_START_STEPS_S;
    const float maxSpeed = maxStepsPerSec();
    float speed = currentStepsPerSec;
    if (speed < startSpeed) speed = startSpeed;
    // Brake before the end of the move, or a sweep turnaround.
    const float brakingSteps = (speed * speed - startSpeed * startSpeed) / (2.0f * acceleration);
    if ((float)remaining <= brakingSteps + 1.0f) {
        speed = sqrtf(fmaxf(startSpeed * startSpeed, speed * speed - 2.0f * acceleration));
    } else {
        speed = sqrtf(speed * speed + 2.0f * acceleration);
        if (speed > maxSpeed) speed = maxSpeed;
    }
    currentStepsPerSec = speed;
    doStep(nextDir);
    if (mode == SM_AIM && pos == aimTarget) hold();
}

static bool isMoving() { return mode == SM_SWEEP || mode == SM_AIM; }
static bool isSweeping() { return mode == SM_SWEEP; }
/* 线圈是否通电（转着或锁定都算）。功率估算用得到：锁定也在吃电流。 */
static bool isEnergized() { return energized; }
static float angleDeg() { return (float)pos * DEG_PER_HALF_STEP; }
static long  posSteps() { return pos; }

/* 是否停在中位（pos = 0 且已经不动） */
static bool atCenter() { return pos == 0 && !isMoving(); }

/* 走回中位。速度用“对准”那一档（STEPMOT_AIM_RPM），慢慢走不丢步。
   幂等：已经在归中路上就什么都不做，别把加速重新归零。 */
static void goCenter()
{
    if (pos == 0) { hold(); return; }
    if (mode == SM_AIM && aimTarget == 0) return;
    aimTarget = 0;
    rpm = (uint16_t)STEPMOT_AIM_RPM;
    currentStepsPerSec = 0.0f;
    lastStepUs = micros();
    mode = SM_AIM;
}

/* 把“当前位置”直接定义成中位并写进 NVS。
   机械被人手碰歪、或者换了安装角度后，把风扇头摆正再敲这条校正。 */
static void resetCenter()
{
    pos = 0;
    aimTarget = 0;
    savePosNow();
}

static bool settledFor(uint32_t ms) {
    return !isMoving() && (uint32_t)(millis() - lastMoveMs) >= ms;
}

} // namespace stepmot
#endif // STEPPER_DRIVE_H
