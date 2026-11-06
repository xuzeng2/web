/**
 * ============================================================================
 *  thermal_sensor.h  —  AMG8833 红外阵列：背景校准 + 模型识别 + 方位解算
 * ----------------------------------------------------------------------------
 *  识别部分严格照 子测试项目/AMG_8833/ESP32_AI 的做法（这点很关键，别改）：
 *    · 模型：amg_model_int8.tflite → model.h 里的 g_model（5 类 int8 量化模型）
 *    · 类别：air / computer / light / man / onther，只有 man 算“有人”
 *    · 预处理：本帧最低温归一化 → VMIN..VMAX → 按输入 scale/zero_point 量化
 *      （顺序、常数、clamp 范围全部与示例一致）
 *    · 推理：MicroInterpreter + 32KB 静态竞技场，算子集与示例相同
 *
 *  主控这边额外做的三件事：
 *    1. 开机 3 秒把 64 个像素分别求平均 → bgPixel[64]，8x8 全体均值 → bgMean
 *       （需求要求；模型输入不用背景，但方位解算要用）
 *    2. 方位：模型只给类别、不给方向，方向仍然用“温升加权质心”算
 *       → 8 列摊 60°，每列 7.5°，供步进电机对准
 *    3. 去抖 + 降频：连续 N 帧才确认 / 才取消；推理按 IR_MODEL_INFER_MS 跑，
 *       采样仍按 IR_POLL_MS，避免推理把主循环（LVGL/步进）拖住
 *
 *  降级策略：
 *    模型加载/分配失败 → 自动退回纯阈值法（温升 > IR_DELTA_C 的热点数），
 *    功能不至于整块失效，串口会打一行警告说明走的是哪条路。
 * ============================================================================
 */
#ifndef THERMAL_SENSOR_H
#define THERMAL_SENSOR_H

#include <Wire.h>
#include <Adafruit_AMG88xx.h>

#include "fan_config.h"
#include "fan_types.h"

#if IR_USE_MODEL
/* ★ 别 include <TFLiteMicro_ArduinoESP32S3.h>（库根目录那个总头）。
   它会把库内另一套 flatbuffers 头带进 include 链，与本目录示例用的
   tensorflow/lite/schema/schema_generated.h 版本对不上，直接卡在
   static_assert "Non-compatible flatbuffers version included"。
   只 include 下面这三个子头即可 —— 实测 arduino-cli 照样能认出这个
   预编译库（.a）并加上 -l 参数，链接不缺符号。 */
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model.h"        /* const unsigned char g_model[] / g_model_len */
#endif

namespace thermal {

static Adafruit_AMG88xx amg;
static float     pix[IR_PIXELS];        /* 当前帧 */
static float     bgPixel[IR_PIXELS];    /* 每像素背景 */
static bool      ready      = false;
static uint32_t  lastFrameMs = 0;
static uint32_t  lastInferMs = 0;
static uint8_t   hitCnt     = 0;        /* 连续命中帧数 */
static uint8_t   missCnt    = 0;        /* 连续未命中帧数 */
static bool      wasTracking = false;   /* 上一拍是否在跑识别（用来捕捉开关沿）*/

/* 对准方向系数（±1）：默认取 IR_AIM_SIGN，运行时可以由串口 "debug aim flip" 现场翻转。
   放成变量而不是直接用宏，是因为“装反了”这件事只能上机才知道，
   让用户拆机改接线不如让他敲一行串口。 */
static float     aimSign    = IR_AIM_SIGN;

/* ===========================================================================
 * 一、模型（与示例 ESP32_AI 一一对应）
 * ========================================================================= */
/* 类别表无条件定义：detectFrame() 里要把下标翻成名字，
   IR_USE_MODEL=0 时虽然永远拿不到有效下标，但符号得在。 */
static constexpr int kNumClasses = 5;
static const char *const kClassNames[kNumClasses] = {
    "air", "computer", "light", "man", "onther"
};

#if IR_USE_MODEL
static constexpr int kTensorArenaSize = IR_MODEL_ARENA_KB * 1024;

static const tflite::Model        *model       = nullptr;
static tflite::MicroInterpreter   *interpreter = nullptr;
static TfLiteTensor               *input       = nullptr;
static TfLiteTensor               *output      = nullptr;
static uint8_t                     tensorArena[kTensorArenaSize];

static bool   modelOk    = false;
static bool   modelTried = false;       /* 懒加载：只尝试初始化一次 */
static int8_t manIdx     = -1;          /* “人”在输出里的下标，启动时算一次 */
static int8_t lastCls    = -1;          /* 最近一次推理的类别 */
static float  lastClsConf = 0.0f;
static uint32_t inferCount = 0;         /* 累计推理次数，调试用 */
#endif

/* ---------------------------------------------------------------------------
 * 模型初始化
 * ------------------------------------------------------------------------- */
static bool modelInit()
{
#if IR_USE_MODEL
    modelLoad();
    model = tflite::GetModel(g_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.printf("[红外] 模型 schema 不匹配：%lu != %d\n",
                      (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    /* 算子集与示例完全相同（少一个都不行，多一个白占 flash）
       resolver 是静态的，只能注册一次：重载模型时会再进这个函数，
       重复 Add 会把 10 个槽位撑满，后面的 Add 静默失败。 */
    static tflite::MicroMutableOpResolver<10> resolver;
    static bool opsReady = false;
    if (!opsReady) {
        resolver.AddConv2D();
        resolver.AddMaxPool2D();
        resolver.AddReshape();
        resolver.AddFullyConnected();
        resolver.AddSoftmax();
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddShape();
        resolver.AddStridedSlice();
        resolver.AddPack();
        opsReady = true;
    }

    /* ★ 解释器必须每次重建，不能是函数局部 static：
       它内部握着 model 指针，重载模型时旧缓冲会被 free，
       而局部 static 只构造一次、不会跟着换 —— 复用它就是 use-after-free
       （表现：重载后推理结果不变，或者直接 StoreProhibited 重启）。
       所以改成 new，重建前先把旧的 delete 掉。 */
    if (interpreter) {
        delete interpreter;
        interpreter = nullptr;
    }
    input  = nullptr;
    output = nullptr;

    interpreter = new tflite::MicroInterpreter(model, resolver, tensorArena, kTensorArenaSize);
    if (interpreter == nullptr) {
        Serial.println(F("[红外] 解释器创建失败（内存不足）"));
        return false;
    }

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        Serial.println(F("[红外] 模型张量分配失败（竞技场不够？改 IR_MODEL_ARENA_KB）"));
        delete interpreter;
        interpreter = nullptr;
        return false;
    }

    input  = interpreter->input(0);
    output = interpreter->output(0);

    /* 输入必须是 64 个 int8（8x8 温度矩阵）；尺寸不对就宁可不用模型，
       否则 runModel() 里按 64 写会越界，那种崩法很难查。*/
    int inElems = 1;
    for (int i = 0; i < input->dims->size; i++) inElems *= input->dims->data[i];
    if (inElems != IR_PIXELS || input->type != kTfLiteInt8) {
        Serial.printf("[红外] 模型输入不匹配：需要 %d 个 int8，实际 %d 个 type=%d\n",
                      IR_PIXELS, inElems, (int)input->type);
        delete interpreter;
        interpreter = nullptr;
        input  = nullptr;
        output = nullptr;
        return false;
    }

    /* 找“人”的下标：示例是 kClassNames[best] == "man"，这里预先把下标算出来 */
    for (int i = 0; i < kNumClasses; i++) {
        if (!strcasecmp(kClassNames[i], IR_MODEL_MAN_CLASS)) { manIdx = (int8_t)i; break; }
    }
    if (manIdx < 0) {
        Serial.printf("[红外] 模型里找不到类别 %s，识别不可用\n", IR_MODEL_MAN_CLASS);
        delete interpreter;
        interpreter = nullptr;
        input  = nullptr;
        output = nullptr;
        return false;
    }

    modelOk = true;
    Serial.printf("[红外] 模型就绪 | 输入：缩放 %.4f 零点 %d | 输出：缩放 %.4f 零点 %d | 人物类 %d | 竞技场 %dKB\n",
                  input->params.scale, input->params.zero_point,
                  output->params.scale, output->params.zero_point,
                  (int)manIdx, IR_MODEL_ARENA_KB);
    return true;
#else
    return false;
#endif
}

/* ---------------------------------------------------------------------------
 * 按需加载模型
 *   需求：没开红外识别的时候不要打开红外模型 —— 所以开机不加载，
 *   第一次真正需要（网页打开热源追踪）时才 GetModel / AllocateTensors。
 *   只尝试一次：失败就走阈值兜底，别每次开关都再折腾一遍。
 * ------------------------------------------------------------------------- */
#if IR_USE_MODEL
static bool ensureModel()
{
    if (modelTried) return modelOk;
    modelTried = true;
    if (modelInit()) return true;
    Serial.println(F("[红外] 模型不可用 → 退回纯阈值法（功能仍在，误判会多一些）"));
    return false;
}

/* ---------------------------------------------------------------------------
 * 跑一次推理：返回类别下标与概率
 *   预处理逐行照抄示例，唯一区别只是像素数组是成员变量 pix 而不是局部 pixels
 * ------------------------------------------------------------------------- */
static bool runModel(int8_t *outCls, float *outConf)
{
    if (!modelOk || (input == nullptr) || (output == nullptr)) return false;

    /* ---- 预处理（与示例一致）---- */
    const float VMAX = IR_MODEL_VMAX, VMIN = IR_MODEL_VMIN;
    float fmin = pix[0];
    for (int i = 1; i < IR_PIXELS; i++) if (pix[i] < fmin) fmin = pix[i];

    const float   in_scale = input->params.scale;
    const int32_t in_zp    = input->params.zero_point;

    for (int i = 0; i < IR_PIXELS; i++) {
        float v = pix[i] - fmin;
        v = v / (VMAX - VMIN);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        int32_t q = (int32_t)roundf(v / in_scale) + in_zp;
        if (q < -128) q = -128;
        if (q > 127)  q = 127;
        input->data.int8[i] = (int8_t)q;
    }

    /* ---- 推理 ---- */
    uint32_t t0 = micros();
    if (interpreter->Invoke() != kTfLiteOk) {
        if (!(inferCount % 50)) Serial.println(F("[红外] 推理失败（模型调用返回错误）"));
        return false;
    }
    uint32_t dtUs = micros() - t0;
    inferCount++;
    if ((inferCount % 50) == 1) {
        Serial.printf("[红外] 单次推理 %.1f 毫秒\n", dtUs / 1000.0f);
    }

    /* ---- 反量化 + 取最大 ---- */
    int   out_n    = output->dims->data[output->dims->size - 1];
    int32_t out_zp = output->params.zero_point;
    float   out_sc = output->params.scale;

    int   best   = 0;
    float best_p = -1.0f;
    for (int i = 0; i < out_n && i < kNumClasses; i++) {
        float p = (float)((int32_t)output->data.int8[i] - out_zp) * out_sc;
        if (p > best_p) { best_p = p; best = i; }
    }

    *outCls  = (int8_t)best;
    *outConf = best_p;
    return true;
}
#endif

/* ---------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------- */
static bool begin()
{
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    Wire.setTimeOut(50);
#endif
    if (!amg.begin(IR_I2C_ADDR, &Wire)) {
        ready = false;
        return false;
    }
    amg.setMovingAverageMode(false);    /* 我们自己去做去抖，别让硬件再糊一层 */
    delay(120);                          /* 首帧需要 ~10ms 出数，多给点余量 */
    ready = true;

#if IR_USE_MODEL
    /* 这里故意不加载模型：等网页真正打开「热源追踪」时再懒加载（见 ensureModel）。 */
    Serial.println(F("[红外] 模型未加载（打开热源追踪时才加载）"));
#else
    Serial.println(F("[红外] 已禁用模型（IR_USE_MODEL=0）→ 纯阈值法"));
#endif
    return true;
}

/* ---------------------------------------------------------------------------
 * 背景校准
 *   progress 回调用于刷新 TFT 上的进度（0..100），不需要就传 NULL
 * ------------------------------------------------------------------------- */
static bool calibrate(uint32_t durationMs, void (*progress)(uint8_t))
{
    if (!ready) return false;

    for (int i = 0; i < IR_PIXELS; i++) bgPixel[i] = 0.0f;

    uint32_t frames = 0;
    uint32_t t0 = millis();

    while (millis() - t0 < durationMs) {
        amg.readPixels(pix);
        for (int i = 0; i < IR_PIXELS; i++) bgPixel[i] += pix[i];
        frames++;

        if (progress) {
            uint32_t el = millis() - t0;
            progress((uint8_t)((el * 100UL) / durationMs));
        }
        delay(IR_CALIB_FRAME_MS);
    }

    if (frames == 0) return false;

    float sum = 0.0f;
    for (int i = 0; i < IR_PIXELS; i++) {
        bgPixel[i] /= (float)frames;
        sum += bgPixel[i];
    }
    g_rt.bgMean     = sum / (float)IR_PIXELS;   /* 需求要求的 8x8 平均值 */
    g_rt.calibrated = true;
    return true;
}

/* ---------------------------------------------------------------------------
 * 单帧检测（不做去抖）
 *   haveModel/cls/clsConf：模型的结论；“有没有人”由它决定
 *   方位：模型不管方向，仍然用相对背景的温升质心算
 * ------------------------------------------------------------------------- */
static IrResult detectFrame(bool haveModel, int8_t cls, float clsConf)
{
    IrResult r;
    r.person    = false;
    r.rawPerson = false;
    r.hot       = 0;
    r.maxDelta  = 0.0f;
    r.conf      = 0.0f;
    r.angleDeg  = 0.0f;
    r.xNorm     = 0.5f;     /* 没热点就报正中央，网页那边会按 person 决定藏不藏 */
    r.yNorm     = 0.5f;
    r.cls       = haveModel ? cls : (int8_t)-1;
    r.clsName   = (haveModel && cls >= 0 && cls < kNumClasses) ? kClassNames[cls] : nullptr;
    r.clsConf   = haveModel ? clsConf : 0.0f;
    r.byModel   = haveModel;

    float wsum = 0.0f;      /* 温升权重和 */
    float csum = 0.0f;      /* 列坐标加权和 */
    float rsum = 0.0f;      /* 行坐标加权和 */

    for (int i = 0; i < IR_PIXELS; i++) {
        float d = pix[i] - bgPixel[i];
        if (d > r.maxDelta) r.maxDelta = d;
        if (d >= IR_DELTA_C) {
            r.hot++;
            float w = d - IR_DELTA_C + 0.5f;   /* 越热权重越大，+0.5 防 0 权重 */
            wsum += w;
            csum += w * ((float)(i % 8) - 3.5f);
            rsum += w * ((float)(i / 8) - 3.5f);
        }
    }

    /* ---- “有没有人”：模型说了算；模型没起来才用阈值 ---- */
    if (haveModel) {
        r.rawPerson = (cls == manIdx) && (clsConf >= IR_MODEL_CONF_MIN);
        r.conf      = (cls == manIdx) ? clsConf : 0.0f;
    } else {
        r.rawPerson = (r.hot >= IR_MIN_HOT);
        float c = (r.maxDelta - IR_DELTA_C) / (IR_FULL_C - IR_DELTA_C);
        if (c < 0.0f) c = 0.0f;
        if (c > 1.0f) c = 1.0f;
        r.conf = r.rawPerson ? c : 0.0f;
    }

    /* 方位：质心所在的列 → 角度。8 列铺满 60°，每列 7.5° */
    if (wsum > 0.0f) {
        float colOffset = csum / wsum;                 /* -3.5 .. +3.5 */
        float rowOffset = rsum / wsum;
        r.angleDeg = colOffset * IR_DEG_PER_COL * aimSign;
        /* 给网页画点用：列/行中心归一化到 0..1（不做左右镜像，视场原样映射） */
        r.xNorm = (colOffset + 4.0f) / 8.0f;
        r.yNorm = (rowOffset + 4.0f) / 8.0f;
    }
    return r;
}

/* ---------------------------------------------------------------------------
 * 帧数据是否可信
 *   Adafruit_AMG88xx 2.0.x 的 readPixels() 返回 void，没有失败返回值，
 *   所以用“数值是否在人能活的范围内”兜底：I2C 抖一下读出的越界值直接丢掉。
 * ------------------------------------------------------------------------- */
static bool frameUsable()
{
    for (int i = 0; i < IR_PIXELS; i++) {
        if (isnan(pix[i])) return false;
        if (pix[i] < -20.0f || pix[i] > 200.0f) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * 当前该不该跑红外识别
 *   网页「热源追踪」开着、且风扇没关机，才算真正要识别。
 *   关机时也停 —— 全机关掉还占着 CPU 推理没有意义。
 * ------------------------------------------------------------------------- */
static bool trackingWanted()
{
    return g_cmd.heatOn && g_cmd.power;
}

/* 把结论清成“无人”。关掉识别后必须清：
   否则残留的 person=true 会让状态机以为人还在，双模态判定也会读到旧值。 */
static void clearResult()
{
    if (!g_rt.ir.person && !g_rt.ir.rawPerson && g_rt.ir.hot == 0
        && g_rt.ir.clsName == nullptr) return;

    g_rt.ir.person    = false;
    g_rt.ir.rawPerson = false;
    g_rt.ir.hot       = 0;
    g_rt.ir.maxDelta  = 0.0f;
    g_rt.ir.conf      = 0.0f;
    g_rt.ir.angleDeg  = 0.0f;
    g_rt.ir.xNorm     = 0.5f;
    g_rt.ir.yNorm     = 0.5f;
    g_rt.ir.cls       = -1;
    g_rt.ir.clsName   = nullptr;
    g_rt.ir.clsConf   = 0.0f;
    g_rt.ir.byModel   = false;
    hitCnt = missCnt = 0;
}

/* ---------------------------------------------------------------------------
 * 周期调用：读一帧 + 推理（降频）+ 去抖，结果落到 g_rt.ir
 *   ★ 没开热源追踪时：不加载模型、不推理、不判决，结论直接清零。
 *     只有串口打开了 /amg8833 调试接口时，才按 IR_IDLE_POLL_MS 慢速读一帧
 *     给那个接口用（读的是原始温度，和模型无关）。
 * ------------------------------------------------------------------------- */
static void poll()
{
    if (!ready) return;

    /* ---- 识别没开：整条链路都停 ---- */
    if (!trackingWanted()) {
        if (wasTracking) {
            wasTracking = false;
            clearResult();
            Serial.println(F("[红外] 识别已关闭 → 模型停跑、结论清零"));
        }
        if (g_rt.dbgAmg && (millis() - lastFrameMs >= IR_IDLE_POLL_MS)) {
            lastFrameMs = millis();
            amg.readPixels(pix);        /* 只为调试接口刷新原始温度 */
        }
        return;
    }

    /* ---- 刚打开识别：重置去抖与模型缓存，第一帧就重新推理 ---- */
    if (!wasTracking) {
        wasTracking = true;
        hitCnt = missCnt = 0;
#if IR_USE_MODEL
        lastCls     = -1;
        lastInferMs = 0;
#endif
        Serial.println(F("[红外] 识别已开启 → 加载并运行模型"));
    }

#if IR_USE_MODEL
    ensureModel();                      /* 第一次进来时才真正加载（只试一次）*/
#endif

    if (millis() - lastFrameMs < IR_POLL_MS) return;
    lastFrameMs = millis();

    amg.readPixels(pix);
    /* 坏帧：不清结论，只是这一帧不参与判决，避免 I2C 抖动导致风扇乱开关 */
    if (!frameUsable()) return;

    /* ---- 推理：按 IR_MODEL_INFER_MS 降频，两次之间沿用上次结论 ----
       单次推理在 S3 上约几十毫秒，如果每 110ms 都跑会挤压步进和刷屏的节拍。*/
    bool   haveModel = false;
    int8_t cls       = -1;
    float  clsConf   = 0.0f;

#if IR_USE_MODEL
    if (modelOk) {
        if (millis() - lastInferMs >= IR_MODEL_INFER_MS) {
            lastInferMs = millis();
            int8_t c; float p;
            if (runModel(&c, &p)) { lastCls = c; lastClsConf = p; }
        }
        if (lastCls >= 0) {
            haveModel = true;
            cls       = lastCls;
            clsConf   = lastClsConf;
        }
    }
#endif

    IrResult r = detectFrame(haveModel, cls, clsConf);

    if (r.rawPerson) {
        missCnt = 0;
        if (hitCnt < 255) hitCnt++;
        if (hitCnt >= IR_CONFIRM_N) r.person = true;
    } else {
        hitCnt = 0;
        if (missCnt < 255) missCnt++;
        if (missCnt < IR_RELEASE_N) r.person = g_rt.ir.person;   /* 释放也去抖 */
    }

    g_rt.ir = r;
}

/* ---------------------------------------------------------------------------
 * 对外小工具
 * ------------------------------------------------------------------------- */
static bool  isReady()    { return ready; }
static bool  modelReady() { 
#if IR_USE_MODEL
    return modelOk;
#else
    return false;
#endif
}
static float bg()         { return g_rt.bgMean; }
/* 最近一帧的方位角；需要“电机停稳后再读”的地方自己判 motor 状态 */
static float angleDeg()   { return g_rt.ir.angleDeg; }

/* ---- 对准方向系数：±1，只认符号 ----
   改了之后下一帧采样就会用新符号，不用重启。 */
static float getAimSign()          { return aimSign; }
static void  setAimSign(float s)   {
    aimSign = (s < 0.0f) ? -1.0f : 1.0f;
    Serial.printf("[对准] 方向系数改为 %+.0f（%s）\n",
                  aimSign, aimSign > 0.0f ? "正向" : "反向");
}
static void  flipAimSign()         { setAimSign(-aimSign); }
/* 最近一次模型类别名（模型没起来返回 "-"），日志和屏幕用 */
static const char *className()
{
    return g_rt.ir.clsName ? g_rt.ir.clsName : "-";
}

/* 把 8x8 打成一屏 ASCII，串口调位置用（很直观，调试时救过命）*/
static void dumpAscii()
{
    if (!ready) return;
    Serial.println(F("---- 红外阵列 AMG8833（单位 ℃，括号内为相对背景的温升）----"));
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int i = r * 8 + c;
            Serial.printf("%5.1f(%+.1f) ", pix[i], pix[i] - bgPixel[i]);
        }
        Serial.println();
    }
    Serial.printf("模型：%s (%.2f)  %s\n", className(), g_rt.ir.clsConf,
                  g_rt.ir.byModel ? "AI 判定" : "阈值兜底");
}

/* ---------------------------------------------------------------------------
 * 强制重新加载模型（下载完新模型后调用）
 *   顺序很关键：先销毁解释器 → 再换模型缓冲 → 再重建解释器。
 *   反过来（先换缓冲）旧解释器会握着已经 free 掉的 model 指针。
 * ------------------------------------------------------------------------- */
static bool reloadModel()
{
#if IR_USE_MODEL
    Serial.println(F("[红外] 开始重新加载模型..."));

    /* 1. 先销毁解释器。它是 new 出来的，必须 delete；
          只把指针置空会漏内存，而且那个对象还握着旧 model 指针。 */
    if (interpreter) {
        delete interpreter;
        interpreter = nullptr;
    }
    input       = nullptr;
    output      = nullptr;
    modelOk     = false;
    modelTried  = false;
    lastCls     = -1;
    lastClsConf = 0.0f;
    manIdx      = -1;

    /* 2. 重新从 LittleFS 读文件。旧模型只在新缓冲校验通过后才释放，
          文件下坏了也还能继续用当前这份。 */
    if (!modelLoad(true)) {
        Serial.println(F("[红外] 新模型文件不可用 → 沿用当前模型"));
    }

    /* 3. 重建解释器并重新分配张量 */
    if (modelInit()) {
        modelTried = true;          /* 已经重建好了，别让 ensureModel() 再来一遍 */
        Serial.println(F("[红外] 模型重新加载成功，已恢复运行"));
        return true;
    }

    modelTried = false;             /* 失败就允许下次 poll 再试 */
    Serial.println(F("[红外] 重新加载失败，退回阈值法"));
    return false;
#else
    Serial.println(F("[红外] IR_USE_MODEL=0，不支持重载模型"));
    return false;
#endif
}

/* ---------------------------------------------------------------------------
 * 打印模型状态（串口 debug model 用）
 *   判断“现在跑的是哪一份模型”只看 g_model_dyn：
 *     非空 → LittleFS 里那份（可能已被在线更新过）
 *     为空 → 编进固件的内置数组
 * ------------------------------------------------------------------------- */
static void printModelStatus()
{
#if IR_USE_MODEL
    Serial.println(F("— 红外模型状态 —"));
    Serial.printf("  来源：%s\n",
                  g_model_dyn ? "LittleFS /amg_model_int8.tflite"
                              : "内置固件数组（没下过 / 下载失败）");
    Serial.printf("  大小：%u 字节（内置 %u 字节）\n",
                  g_model_len, (unsigned)g_model_builtin_len);

    LittleFS.begin(false);              /* 幂等：已挂载就直接返回，不格式化 */
    File f = LittleFS.open("/amg_model_int8.tflite", "r");
    if (f) {
        Serial.printf("  文件：存在，%u 字节\n", (unsigned)f.size());
        f.close();
    } else {
        Serial.println(F("  文件：不存在（走内置模型）"));
    }

    if (interpreter) {
        Serial.printf("  解释器：已就绪（“人”下标 %d，竞技场 %d KB）\n",
                      (int)manIdx, IR_MODEL_ARENA_KB);
        Serial.printf("  最近推理：%s 置信度 %.2f | 累计 %lu 次\n",
                      lastCls >= 0 ? kClassNames[lastCls] : "（还没跑过）",
                      lastClsConf, (unsigned long)inferCount);
    } else if (modelTried) {
        Serial.println(F("  解释器：初始化失败（走阈值兜底）"));
    } else {
        Serial.println(F("  解释器：未加载（打开热源追踪时才加载）"));
    }

    Serial.printf("  识别开关：%s（%s）\n",
                  trackingWanted() ? "开启" : "关闭",
                  trackingWanted() ? "正在按 IR_POLL_MS 采样" : "不采样、不推理");
#else
    Serial.println(F("[模型] IR_USE_MODEL=0：固件没编进模型，纯阈值法"));
#endif
}

} /* namespace thermal */

#endif /* THERMAL_SENSOR_H */
