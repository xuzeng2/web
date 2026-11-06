/**
 * ============================================================================
 *  fan_config.h  —  双模态智能定向风扇 · 全局配置
 * ----------------------------------------------------------------------------
 *  改引脚、改阈值、改扫描角度，全在这一个文件里，其它文件不许再写魔数。
 * ============================================================================
 */
#ifndef FAN_CONFIG_H
#define FAN_CONFIG_H

#include <Arduino.h>
#include <esp_arduino_version.h>

/* Arduino-ESP32 核心大版本（2.x / 3.x 的 LEDC API 不一样，用它区分） */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define FAN_CORE_V3 1
#else
  #define FAN_CORE_V3 0
#endif

/* ============================================================================
 * 一、引脚分配
 * ----------------------------------------------------------------------------
 *  这张表是整台机器唯一的“接线真相”，改线只改这里。
 *
 *  ┌────── TFT (ST7789, SPI) ────────────────────────────────────────────┐
 *  │ SCLK 12 / MOSI 11 / CS 10 / DC 9 / RST 14 / BL 13                   │
 *  │ ★ 这几个脚由 TFT_eSPI 的 User_Setup.h 固定，不在本文件里             │
 *  │   （本工程 ref/TFT_eSPI_User_Setup.h 是那份配置的副本，换电脑照着贴）│
 *  └─────────────────────────────────────────────────────────────────────┘
 *
 *  ★ 重要冲突说明（跟子测试项目不一样，别直接照抄）：
 *    子测试项目里 AMG8833 用 SDA=8 / SCL=9，但 9 号已经被 TFT 的 DC 占了。
 *    两个项目合并到一块板子上，I2C 必须换脚 —— 这里挪到 4/5。
 * ========================================================================== */

/* ---- I2C 共用总线：AMG8833(0x69) + INA219(0x40)，地址不冲突，可同总线 ---- */
#define PIN_I2C_SDA         4
#define PIN_I2C_SCL         5

/* ---- 28BYJ-48 + ULN2003 ----
 * 顺序与子测试项目 stepper_motor 完全一致：
 *     Stepper steppermotor(2048, 18, 20, 19, 21)  →  pin1..4 = 18, 20, 19, 21
 * 这里按线圈顺序 A/B/C/D 命名，保持同样的通电时序，转动方向和测试代码一致。
 *
 * ★ 注意：GPIO19 / GPIO20 在 ESP32-S3 上是 USB D- / D+。
 *   若你的板子用「原生 USB」下载和看串口，这四根线会打架，请改成下面备用组：
 *     #define PIN_STEP_A 6 / PIN_STEP_B 7 / PIN_STEP_C 15 / PIN_STEP_D 16
 *   （子测试项目已验证 18/20/19/21 能用，说明你的板子是 UART 桥芯片方案，
 *     默认就按已验证的来。）*/
#define PIN_STEP_A          18      /* IN1 */
#define PIN_STEP_B          20      /* IN2 */
#define PIN_STEP_C          19      /* IN3 */
#define PIN_STEP_D          21      /* IN4 */

/* ---- 风机 PWM（外接 MOS 管栅极，注意加 10k 下拉 + 栅极电阻）---- */
#define PIN_FAN_PWM         47

/* ---- 与 ESP32-CAM 的硬件串口 Serial1 ----
 *   S3 TX(17) → CAM RX(GPIO3)   S3 RX(16) ← CAM TX(GPIO1)
 *   两端必须共地；CAM 是 3.3V 逻辑，不要接 5V。                        */
#define PIN_CAM_TX          17
#define PIN_CAM_RX          16

/* ============================================================================
 * 二、串口与任务
 * ========================================================================== */
#define DEBUG_BAUD          115200
#define CAM_BAUD            115200
#define LOOP_STACK_SIZE     (12 * 1024)   /* 主循环栈：LVGL + WebServer 都在这跑 */

/* ============================================================================
 * 三、AMG8833 红外阵列
 * ========================================================================== */
#define IR_I2C_ADDR         0x69
#define IR_PIXELS           64
#define IR_FOV_DEG          60.0f         /* AMG8833 视场角（水平/垂直都是 60°）*/
#define IR_DEG_PER_COL      (IR_FOV_DEG / 8.0f)   /* 7.5° 一列 */

#define IR_CALIB_MS         3000          /* 开机背景校准时长（需求：3 秒）*/
#define IR_CALIB_FRAME_MS   60            /* AMG8833 输出 10Hz，60ms 取一帧足够 */

#define IR_DELTA_C          3.0f          /* 相对背景的温升阈值（℃），越高越迟钝 */
#define IR_FULL_C           8.0f          /* 达到这个温升算 100% 置信度 */
#define IR_MIN_HOT          3             /* 至少几个像素同时超过阈值才算“有人”*/
#define IR_CONFIRM_N        2             /* 连续 N 帧都超才确认（抗单帧噪声）*/
#define IR_RELEASE_N        3             /* 连续 N 帧都没超才取消 */

#define IR_DEADBAND_DEG     4.0f          /* 对准死区：误差小于它就算对准了 */
#define IR_SETTLE_MS        600           /* 电机停稳后等这么久再取角度（滤掉拖影）*/
#define IR_ALIGN_MAX_TRIES  2             /* 一次对准最多修正几次（超过就不再追，直接送风）*/
#define IR_ALIGN_STEP_DEG   30.0f         /* 单次修正的最大角度，防甩过头 */
#define IR_REALIGN_DEG      25.0f         /* 送风中目标跑偏多少度才重新对准 */
#define IR_REALIGN_ENABLE   0             /* 1 = 送风时也持续跟人（会小幅摆动，默认关）*/

/* ---- 对准方向系数 ----
 *  +1 = 视场偏右 → 电机 pos 增大（默认应该是这个）
 *  -1 = 反过来：传感器/风扇装反、或者电机线圈接反了，用这个
 *
 *  实测本机需要 -1（原来 +1 时越修越偏，两三次就甩到最偏的地方）。
 *  上电默认取这个宏，运行中还能用串口现场翻，不用重新烧：
 *      debug aim        看当前值
 *      debug aim flip   翻一面
 *      debug aim -1     直接设成反向（1 = 正向）
 */
#define IR_AIM_SIGN         (-1.0f)

/* ---- 热源识别模型（严格照 子测试项目/AMG_8833/ESP32_AI 的做法）----
 *  模型：amg_model_int8.tflite（已转成 model.h 里的 g_model 数组，拷在本目录）
 *  5 类：air / computer / light / man / onther，只有 man 算“有人”。
 *  预处理参数与示例一字不差：先减去本帧最低温，再按 VMIN..VMAX 归一化，
 *  然后按输入的 scale/zero_point 量化成 int8。别自己改这三行，改了精度就掉。
 *
 *  IR_USE_MODEL 置 0 可以退回纯阈值法（不加载模型、省 32KB RAM），
 *  模型分配失败时代码也会自动退回阈值法，不会变砖。*/
#define IR_USE_MODEL        1             /* 1 = TFLite 模型分类；0 = 只用阈值 */
#define IR_MODEL_MAN_CLASS  "man"         /* 模型里“人”这一类的名字 */
#define IR_MODEL_CONF_MIN   0.50f         /* man 的概率达到它才算有人（误报多就调高）*/
#define IR_MODEL_ARENA_KB   32            /* 张量竞技场大小，与示例一致 */
#define IR_MODEL_INFER_MS   200           /* 推理降频：多久跑一次（采样仍然 110ms）*/
#define IR_MODEL_VMAX       40.0f         /* 示例预处理：归一化上限（℃）*/
#define IR_MODEL_VMIN       15.0f         /* 示例预处理：归一化跨度基准（℃）*/

/* ============================================================================
 * 四、28BYJ-48 步进电机
 * ========================================================================== */
#define STEP_PER_REV        2048          /* 输出轴一圈步数（4 拍，32×64）*/
#define STEP_DEG_PER_STEP   (360.0f / (float)STEP_PER_REV)   /* 0.1758°/步 */
#define STEP_SCAN_RPM       8             /* 扫描转速（5~10 之间，再高易失步）*/
#define STEP_AIM_RPM        5             /* 对准时慢一点，定位更准 */
/* 扫描幅度：**左右合计**的角度，网页滑杆可调 0..180，0 = 不扫（等同摇头关）。
   下面这个是上电默认值（原来固定“左右各 55°”，即合计 110°）。 */
#define OSC_SPAN_DEFAULT    110.0f
#define OSC_SPAN_MIN        0
#define OSC_SPAN_MAX        180
#define STEP_SWEEP_SPAN_DEG OSC_SPAN_DEFAULT   /* 旧名字，保留兼容 */
#define STEP_RELEASE_MS     8000          /* 锁定 8s 后断电，防止电机烫手 */

/* 角度掉电记忆：位置变了才写 NVS，最长隔这么久写一次（NVS 有擦写寿命，
   配上“值没变就不写”，5 分钟一次大约是每天 100 次以内）。关机时另外必写一次。 */
#define STEP_POS_SAVE_MS    300000UL      /* 5 分钟 */

/* ============================================================================
 * 五、风机 PWM
 * ========================================================================== */
#define FAN_PWM_FREQ        25000         /* 25kHz，超出人耳可听范围，不啸叫 */
#define FAN_PWM_BITS        8             /* 8 位 → 0..255 */
#if FAN_CORE_V3
  #define FAN_PWM_CH        0             /* 3.x 用 pin 号当通道，这里只占位 */
#else
  #define FAN_PWM_CH        0             /* 2.x 的 LEDC 通道号 */
#endif
#define FAN_MIN_DUTY_PCT    8             /* 低于这个占空比风机转不起来，直接截到 0 */
#define FAN_NATURE_PERIOD   6000          /* 自然风起伏周期（ms）*/
#define FAN_NATURE_LOW      0.55f         /* 自然风最低到 55% */
#define FAN_SOFT_STEP_MS    40            /* 每次调占空比的间隔，做软启动 */

/* ============================================================================
 * 六、摄像头链路
 * ========================================================================== */
#define CAM_ONLINE_MS       5000          /* 多久收不到任何报文判离线 */
#define CAM_FRESH_MS        2500          /* 多久没更新就认为“无目标” */
#define CAM_HB_MS           2000          /* 主控发给 CAM 的心跳周期 */
#define CAM_ENABLE_ON_MS    40            /* 收到“摄像头,就绪”等 40ms 再发开关，避开启动瞬间 */
#define CAM_OFFLINE_IGNORE  0             /* 1 = 摄像头离线时忽略它的判定（单靠红外也能吹）
                                           * 0 = 严格：离线视为“没看到人”，防止误判  */

/* ============================================================================
 * 七、状态机 / 网页 / 定时
 * ========================================================================== */
#define STATE_PERIOD_MS     50            /* 状态机节拍 */
#define IR_POLL_MS          110           /* 红外采样周期（≈AMG 帧率）*/
#define IR_IDLE_POLL_MS     1000          /* 识别关闭时的采样周期：只为 /amg8833 调试接口刷原始温度 */
#define UI_PERIOD_MS        200           /* TFT 刷新周期（LVGL 自己会做补间）*/
#define POWER_PERIOD_MS     1000          /* INA219 采样周期 */
#define TARGET_LOST_MS      1500          /* 目标丢失多久才回到扫描（防抖）*/

#define WIFI_AP_SSID        "FanCtrl-Setup"   /* 配网热点名 */
#define WIFI_AP_PASS        "12345678"        /* 配网热点密码（>=8 位）*/
#define BOOT_WIFI_WAIT_MS   5000              /* 开机提示页最多等联网多久（连上就立刻走）*/
#define WIFI_PORTAL_TIMEOUT 300               /* 配网超时（秒），超时后关热点重试 */
#define WIFI_LINK_TIMEOUT   20                /* 连不上已保存 WiFi 的等待（秒）*/
#define WIFI_RETRY_MS       30000             /* 断线重连间隔 */
#define WIFI_HOSTNAME       "fan-s3"
#define MDNS_NAME           "fan"             /* 可通过 http://fan.local 访问 */

/* ============================================================================
 * 八、编译期自检：TFT_eSPI 配置对不对，在编译阶段就报错，别烧进去猜
 * ========================================================================== */
#include <TFT_eSPI.h>

/* ---------------------------------------------------------------------------
 *  修掉 TFT_eSPI 带来的 FS 全局别名缺失
 *  TFT_eSPI_ESP32_S3.h:152 里有 `#define FS_NO_GLOBALS`（它不想跟自己的 FS 封装撞名），
 *  副作用是 core 的 FS.h 会跳过 `using fs::File; using fs::FS;` 这几行，
 *  于是之后 WiFiManager 引入的 <WebServer.h> 里裸写 FS / File 全部报错：
 *      error: 'FS' was not declared in this scope; did you mean 'fs::FS'?
 *  在这里把宏撤掉并把别名补回来（FS.h 有 include guard，不会重复定义类）。
 * ------------------------------------------------------------------------- */
#undef FS_NO_GLOBALS
#include <FS.h>
using fs::File;
using fs::FS;
using fs::SeekMode;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;

#if !defined(ST7789_DRIVER)
  #error "TFT_eSPI 没选 ST7789：User_Setup.h 里应有 #define ST7789_DRIVER"
#endif
#if (TFT_WIDTH != 240) || (TFT_HEIGHT != 320)
  #error "TFT_eSPI 分辨率不是 240x320：User_Setup.h 里 TFT_WIDTH=240 / TFT_HEIGHT=320"
#endif
#if (TFT_MOSI != 11) || (TFT_SCLK != 12) || (TFT_CS != 10) || (TFT_DC != 9) || (TFT_RST != 14)
  #error "TFT_eSPI 引脚不对：应为 MOSI=11 SCLK=12 CS=10 DC=9 RST=14"
#endif
#if !defined(TFT_BL) || (TFT_BL != 13)
  #error "TFT_eSPI 背光脚不是 GPIO13：User_Setup.h 里应有 #define TFT_BL 13"
#endif
/* I2C 绝对不能踩到 TFT 的脚，这里直接卡死 */
#if (PIN_I2C_SDA == TFT_DC) || (PIN_I2C_SCL == TFT_DC) || \
    (PIN_I2C_SDA == TFT_CS) || (PIN_I2C_SCL == TFT_CS) || \
    (PIN_I2C_SDA == TFT_MOSI) || (PIN_I2C_SCL == TFT_SCLK) || \
    (PIN_I2C_SDA == TFT_RST) || (PIN_I2C_SCL == TFT_BL)
  #error "I2C 引脚和 TFT 冲突：SDA/SCL 不能是 9/10/11/12/13/14"
#endif

#endif /* FAN_CONFIG_H */
