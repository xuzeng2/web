/**
 * ============================================================================
 *  fan_types.h  —  全局数据结构
 * ----------------------------------------------------------------------------
 *  所有模块共享的就两个东西：
 *      g_cmd  …… 用户意图（网页下发的），只有网页能改
 *      g_rt   …… 运行时事实（传感器、电机、模式），只有各模块自己写
 *  分开的好处：调 UI 的时候一眼能看出“这是设置”还是“这是当前状态”。
 * ============================================================================
 */
#ifndef FAN_TYPES_H
#define FAN_TYPES_H

#include <Arduino.h>

/* ---------------------------------------------------------------------------
 * 系统模式
 * ------------------------------------------------------------------------- */
enum SysMode {
    M_BOOT = 0,     /* 上电：等红外校准 */
    M_OFF,          /* 关机 */
    M_MANUAL,       /* 手动：双模态都没开，像普通风扇一样按 speed 常吹 */
    M_SWEEP,        /* 扫描：电机来回摆，等目标 */
    M_ALIGN,        /* 对准：已发现目标，把风扇转到目标方向 */
    M_BLOW          /* 送风：锁定目标，风机开 */
};

/* 模式的中文名：只给串口日志用（JSON 里仍用上面的英文，方便脚本判断） */
static const char *const MODE_NAME_CN[] = {
    "启动", "关机", "手动", "扫描", "对准", "送风"
};

/* 模式的中文名，串口日志和 TFT 都用它 */
static const char *const MODE_NAME[] = {
    "BOOT", "OFF", "MANUAL", "SWEEP", "ALIGN", "BLOW"
};

/* ---------------------------------------------------------------------------
 * 用户意图（网页 → 主控）
 * ------------------------------------------------------------------------- */
struct FanCmd {
    bool     power;        /* 总开关 */
    uint8_t  speed;        /* 风速 0..100 */
    bool     nature;       /* 自然风 */
    bool     osc;          /* 自动扫描/摇头 */
    uint8_t  oscSpan;      /* 摇头幅度（左右合计 0..180°），0 = 不扫 */
    bool     heatOn;       /* 红外识别启用 */
    bool     camOn;        /* 摄像头识别启用 */
    uint32_t timerMin;     /* 定时剩余分钟，0 = 不定时 */
    bool     dirty;        /* 有新指令待落地（set 之后由主循环消费）*/
};

/* ---------------------------------------------------------------------------
 * 运行时事实
 * ------------------------------------------------------------------------- */
struct IrResult {
    bool     person;       /* 去抖之后的结论 */
    bool     rawPerson;    /* 单帧原始结论 */
    uint8_t  hot;          /* 超阈值像素个数（模型模式下仅作调试参考）*/
    float    maxDelta;     /* 最大温升（℃）*/
    float    conf;         /* 置信度 0..1 */
    float    angleDeg;     /* 目标相对传感器光轴的水平偏角（+右 -左）*/
    /* 热点在 8x8 视场里的归一化质心（0..1）。0.5/0.5 = 正中央。
       网页画红点直接用它，不再自己编位置。没有热点时保持 0.5/0.5。 */
    float    xNorm;
    float    yNorm;
    /* ---- TFLite 模型的输出（模型未就绪时 cls = -1、clsName = nullptr）---- */
    int8_t      cls;       /* 类别下标 0..4 */
    const char *clsName;   /* 类别名 air/computer/light/man/onther */
    float       clsConf;   /* 该类别概率 0..1 */
    bool        byModel;   /* true = 这一帧的结论来自模型；false = 退回阈值法 */
};

struct CamResult {
    bool     online;       /* 链路是否活着 */
    bool     person;       /* 去抖 + 超时之后的结论 */
    bool     rawPerson;    /* 最近一帧的原始结论 */
    float    conf;
    float    cx;           /* 目标在画面里的横向位置 0..1（可选，-1 表示没给）*/
    uint32_t lastMsgMs;
    uint32_t frames;       /* 累计收到多少帧 PER */
};

struct FanRt {
    SysMode  mode;
    /* 红外 */
    IrResult ir;
    float    bgMean;           /* 背景平均温度（需求里那个 8x8 均值）*/
    bool     calibrated;
    /* 摄像头 */
    CamResult cam;
    /* 电机 */
    float    motorAngleDeg;    /* 相对开机位置的转角 */
    bool     motorMoving;
    /* 风机 */
    uint8_t  fanPct;           /* 当前实际占空比 */
    bool     blowing;          /* 是否在吹（PWM > 0）*/
    /* 目标 */
    uint32_t lastSeenMs;       /* 最后一次确认有目标 */
    uint32_t lockSinceMs;      /* 锁定开始时刻 */
    uint8_t  alignTries;
    uint32_t alignWaitMs;      /* 对准阶段的等待起点 */
    /* 系统 */
    uint32_t timerDeadlineMs;  /* millis() 时间轴上的关机时刻，0 = 无 */
    uint32_t uptimeMs;
    uint32_t loopMaxUs;        /* 单次 loop 最长耗时，排查卡顿用 */
    bool     dbgAmg;           /* 串口 "debug amg8833 true" 打开后 /amg8833 才可用 */
};

/* 全局实例：定义在 fan_main.ino 里（不用 inline 变量，兼容 core 2.x 的 gnu++11）*/
extern FanCmd g_cmd;
extern FanRt  g_rt;

#endif /* FAN_TYPES_H */
