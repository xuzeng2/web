/**
 * ============================================================================
 *  fan_ui.h  —  智能电风扇控制界面（LVGL v8.3 / C）
 *  240 x 320 竖屏，与 fan_ui_240x320.xml 的排版一一对应
 * ----------------------------------------------------------------------------
 *  设计要点
 *    1. 布局全部走 Flex（column / row），没有硬编码坐标
 *    2. 所有数据都在 fan_ui_data_t 里，调 fan_ui_update() 即可刷新
 *       状态 / 风速 / 5 个 LED / 拨盘进度 / 设备状态（温度·内存·电流·功率）
 *    3. 变化都走补间，不做「瞬间跳变」（对应网页里的 transition: 0.3s ease）
 *         · 拨盘弧 + 风速数字    一起缓动，数字跟着弧走
 *         · 温度 / 内存进度条     条和数字同步缓动；电流 / 功率只动数字
 *         · 5 个 LED             颜色渐变 + 逐个点亮（35ms 阶梯）
 *         · OFF / ON 文字        淡出 → 换字 → 淡入（换字那一瞬是不可见的）
 *         · 轮毂描边             开机时由灰蓝渐亮成青蓝
 *         · 电源灯               点亮后轻微呼吸（可关）
 *    4. 扁平化：无阴影、无毛玻璃，颜色统一 0x 十六进制
 *    5. 全部用整数传递，避免 MCU 上 printf 浮点：
 *         温度 temp_x10（42.6℃ = 426）、电流 current_ma（0.52A = 520）、
 *         功率 power_mw（6.24W = 6240）、内存 KB
 * ----------------------------------------------------------------------------
 *  用法（Arduino / ESP-IDF 均可）
 *
 *      #include "fan_ui.h"
 *
 *      void setup() {
 *          // ... lv_init() / 显示驱动 / 触摸驱动 ...
 *          fan_ui_t *ui = fan_ui_create(NULL);      // NULL = 当前活动屏幕
 *
 *          fan_ui_data_t d = {0};
 *          d.power  = true;
 *          d.speed  = 60;
 *          d.nature = true;
 *          d.temp_x10  = 426;                       // 42.6 ℃
 *          d.free_kb   = 168;
 *          d.total_kb  = 270;
 *          d.current_ma = 520;                      // 0.52 A
 *          d.power_mw   = 6240;                     // 6.24 W
 *          fan_ui_update(ui, &d);
 *      }
 *
 *      // 数据变化时（例如 HTTP /status 回来、按键、传感器采样）
 *      d.speed = 80;
 *      d.temp_x10 = 451;
 *      fan_ui_update(ui, &d);
 *
 *  要点：fan_ui_update() 是「全量提交」，100ms 叫一次甚至每帧叫一次都没问题
 *        —— 只有真的变了的字段才启动动画，没变的字段零重绘。
 *
 *  中文字体
 *      font_cn.c 已随本文件一起提供，把它加进工程一起编译即可
 *      （16px、bpp4、line_height=19 的简体子集，含界面全部中文 + 常用状态词）。
 *      实测占用：flash 约 14KB（位图 6.8KB + 字形描述表），RAM 16B，
 *      不用再配字体路径。
 *      若要换成自己的字体，在 #include "fan_ui.h" 之前覆盖：
 *          LV_FONT_DECLARE(my_font);
 *          #define FAN_UI_FONT_CN (&my_font)
 *          #include "fan_ui.h"
 *      缺字（要显示子集外的汉字）时，用 lv_font_conv 重新生成 font_cn.c，
 *      命令写在 font_cn.c 文件头。
 *
 *  动画开关（都在 #include "fan_ui.h" 之前定义）
 *      #define FAN_UI_ANIM 0        // 全部关掉，瞬间切换（排查卡顿/省电时用）
 *      #define FAN_UI_LED_BREATH 0  // 只关电源灯呼吸（那个动画是常驻的）
 * ============================================================================
 */
#ifndef FAN_UI_INCLUDED
#define FAN_UI_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* ----------------------------------------------------------------------------
 * 中文字体
 * ------------------------------------------------------------------------- */
#if defined(FAN_UI_FONT_CN)
/* 调用方自行指定，直接用 */
#else
/* 默认使用随包提供的 font_cn.c（16px 简体子集）。若没把 font_cn.c 加进
 * 工程，编译会在链接阶段报 undefined reference to `font_cn` —— 这是
 * 有意的，比默默显示方块好排查。 */
LV_FONT_DECLARE(font_cn);
#define FAN_UI_FONT_CN (&font_cn)
#endif

/* ----------------------------------------------------------------------------
 * 尺寸与配色（与 XML 完全一致）
 * ------------------------------------------------------------------------- */
#define FAN_SCREEN_W        240    /* 屏幕宽 */
#define FAN_SCREEN_H        320    /* 屏幕高 */

#define FAN_PAD             6      /* 页面内边距 */
#define FAN_GAP             6      /* 行间距 */
#define FAN_LED_DOT         8      /* LED 圆点直径 */
#define FAN_LED_COL         12     /* LED 之间水平间距 */
#define FAN_DIAL_SIZE       119    /* 拨盘外径（底部多了设备状态卡，从 144 收到 119） */
#define FAN_ARC_W           9      /* 拨盘弧线宽度 */
#define FAN_HUB_SIZE        48     /* 中心轮毂直径（拨盘 119 时比例和原来 56/144 接近） */
#define FAN_PILL_H          33     /* 状态胶囊高度 */

/* --- 设备状态卡：2 列 x 2 行，每个指标一格 ---
 *   格宽 = (屏宽 - 2*页面内边距 - 2*卡片描边 - 2*卡片内边距 - 列间距) / 2
 *        = (240 - 12 - 2 - 10 - 8) / 2 = 104
 *   注意：宽度是编译期算死的常量（flex_grow 在 8.3 里会和 column gap 冲突，
 *   容易溢出），改上面任何一个间距都要重算这两个数。 */
#define FAN_CELL_W          104    /* 单个指标格宽度 */
#define FAN_CELL_GAP        8      /* 两列之间的间距 */

#define FAN_LINE_H          19     /* 一行文字高度，必须等于 font_cn 的 line_height */
#define FAN_CARD_PAD        5      /* 卡片内边距 */
#define FAN_CARD_GAP        4      /* 卡片内行间距 */
#define FAN_BAR_H           4      /* 指标进度条高度 */
#define FAN_BAR_SPACE       2      /* 数值行与进度条之间的空隙 */

/* 卡片高度 = 描边*2 + 内边距*2 + 标题 + 间距 + (温度/内存 行 + 条) + 间距 + (电流/功率 行)
 *          = 2 + 10 + 19 + 4 + (19 + 2 + 4) + 4 + 19 = 83 */
#define FAN_CARD_H          83

/* 高度预算（16px 字体、行高 19 时正好铺满 320）：
 *   6 + 19(标题) + 6 + 30(LED) + 6 + 119(拨盘) + 6 + 33(胶囊) + 6 + 83(设备状态卡) + 6 = 320
 *   换更大字号时行高变大，只需调小 FAN_DIAL_SIZE 让它继续塞得下（20px 字体约改成 97）。 */

#define FAN_C_BG            0x0D1B2A
#define FAN_C_CARD          0x0A1421
#define FAN_C_LINE          0x223247
#define FAN_C_TRACK         0x24344A
#define FAN_C_BARTRK        0x1E2B3D
#define FAN_C_DOT_OFF       0x1E2B3D
#define FAN_C_DOT_LINE      0x2B3D55
#define FAN_C_HUB           0x1B2535
#define FAN_C_HUB_LINE      0x3C4F6B
#define FAN_C_RING_ON       0x4FA6C8   /* 开机时轮毂描边渐变到的青蓝色 */

#define FAN_C_TITLE         0xB9D8FF
#define FAN_C_CYAN          0x8FE0FF
#define FAN_C_LABEL         0x7A9CC0
#define FAN_C_LABEL_OFF     0x5C7EA3
#define FAN_C_KEY           0xA6C6EE
#define FAN_C_KEY2          0x96B8E0
#define FAN_C_GREEN         0x4DFF9E
#define FAN_C_BLUE          0x5FC8FF
#define FAN_C_ORANGE        0xFFB347
#define FAN_C_PURPLE        0xC77DFF

/* ----------------------------------------------------------------------------
 * 动画参数（毫秒）—— 整体手感和网页的 0.3s ease 对齐
 *   嫌某处太快/太慢，只改这里，不用碰逻辑
 * ------------------------------------------------------------------------- */
#ifndef FAN_UI_ANIM
#define FAN_UI_ANIM         1     /* 0 = 全部瞬间切换，不跑任何补间 */
#endif
#ifndef FAN_UI_LED_BREATH
#define FAN_UI_LED_BREATH   1     /* 1 = 电源灯点亮后轻微呼吸 */
#endif

#define FAN_ANIM_LED        280   /* 单个灯的颜色渐变 */
#define FAN_ANIM_LED_STEP   35    /* 多个灯依次点亮的间隔（级联感） */
#define FAN_ANIM_SPEED      380   /* 拨盘 + 风速数字 */
#define FAN_ANIM_BAR        380   /* 温度 / 内存进度条 + 数字 */
#define FAN_ANIM_RING       400   /* 轮毂描边亮起 */
#define FAN_ANIM_FADE_OUT   110   /* 状态文字淡出 */
#define FAN_ANIM_FADE_IN    200   /* 状态文字淡入（换完字之后） */
#define FAN_ANIM_BREATH     1400  /* 电源灯呼吸的一个来回 */

/* ----------------------------------------------------------------------------
 * 数据模型：界面显示的东西全在这里
 * ------------------------------------------------------------------------- */
typedef enum {
    FAN_LED_POWER = 0,     /* 电源   绿灯 */
    FAN_LED_NATURE,        /* 自然风 蓝灯 */
    FAN_LED_OSC,           /* 摇头   蓝灯 */
    FAN_LED_HEAT,          /* 热源   橙灯 */
    FAN_LED_CAM,           /* 摄像头 紫灯 */
    FAN_LED_NUM
} fan_led_t;

typedef struct {
    bool     power;        /* 风扇开关 */
    uint8_t  speed;        /* 风速 0..100 (%) */
    bool     nature;       /* 自然风模式 */
    bool     osc;          /* 左右摇头 */
    bool     heat;         /* 热源追踪 */
    bool     cam;          /* 摄像头识别 */
    int16_t  timer_min;    /* 定时关机剩余分钟，0 = 未定时 */
    int16_t  temp_x10;     /* 芯片温度 x10，如 426 = 42.6℃ */
    uint32_t free_kb;      /* 空闲内存 KB */
    uint32_t total_kb;     /* 总内存 KB（进度条量程） */
    uint16_t current_ma;   /* 电流 mA，如 520 = 0.52A（0..65.5A） */
    uint32_t power_mw;     /* 功率 mW，如 6240 = 6.24W */
    uint32_t uptime_s;     /* 运行时间（本界面未显示，留给你们扩展） */
} fan_ui_data_t;

/* 单个 LED 的补间状态：颜色从 from 混到 to，进度 k（0..255）由动画推动 */
typedef struct {
    lv_obj_t  *dot;
    lv_obj_t  *txt;
    lv_color_t dot_from;
    lv_color_t dot_to;
    lv_color_t txt_from;
    lv_color_t txt_to;
    uint8_t    k;          /* 0 = 完全是 from，255 = 完全是 to */
    uint8_t    breath;     /* 呼吸亮度，255 = 满亮（不压暗） */
    bool       on;         /* 目标状态 */
} fan_led_anim_t;

typedef struct {
    lv_obj_t      *root;
    lv_obj_t      *led_dot[FAN_LED_NUM];
    lv_obj_t      *led_txt[FAN_LED_NUM];
    lv_obj_t      *dial;          /* lv_arc */
    lv_obj_t      *hub;           /* 拨盘中心轮毂（开机时描边渐亮） */
    lv_obj_t      *big_state;     /* 拨盘中心 OFF / ON */
    lv_obj_t      *val_state;     /* 状态胶囊里的值 */
    lv_obj_t      *val_speed;     /* 风速值 */
    lv_obj_t      *val_temp;
    lv_obj_t      *val_mem;
    lv_obj_t      *val_cur;       /* 电流值 */
    lv_obj_t      *val_pw;        /* 功率值 */
    lv_obj_t      *bar_temp;
    lv_obj_t      *bar_mem;
    fan_ui_data_t  data;          /* 最近一次渲染的数据 */

    /* --- 动画状态（外部不用管） --- */
    fan_led_anim_t led[FAN_LED_NUM];
    int32_t        speed_cur;     /* 拨盘当前显示值（下次补间的起点） */
    int32_t        temp_cur;
    int32_t        mem_cur;
    int32_t        cur_ma_cur;    /* 电流当前显示值 mA */
    int32_t        pw_mw_cur;     /* 功率当前显示值 mW */
    int32_t        ring_cur;      /* 轮毂描边亮起程度 0..255 */
    bool           ready;         /* 首次 update 直接落地，不做过渡 */
} fan_ui_t;

/* ============================================================================
 * 内部工具
 * ========================================================================= */

/** 干净的透明容器：去掉主题带来的背景/边框/内边距/滚动条 */
static inline lv_obj_t *fan_ui_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static inline lv_obj_t *fan_ui_label(lv_obj_t *parent, const char *txt, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, FAN_UI_FONT_CN, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

/** 强制单行：文字超出就裁掉，绝不折行破坏布局 */
static inline void fan_ui_one_line(lv_obj_t *l)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
}

/** 卡片外观：深底 + 描边 + 圆角，无阴影 */
static inline void fan_ui_card_style(lv_obj_t *o, lv_coord_t radius)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(FAN_C_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(FAN_C_LINE), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
}

/** 设备状态卡里的一个指标格
 *    有进度条：上行「名称 —— 数值」+ 下行细条（温度 / 内存）
 *    无进度条：只有一行（电流 / 功率，瞬时量没有「满格」概念）
 *  val_out / bar_out 传 NULL 表示不需要取回。
 */
static inline lv_obj_t *fan_ui_metric(lv_obj_t *parent, const char *name,
                                      const char *val, uint32_t color,
                                      int16_t bar_max, bool show_bar,
                                      lv_obj_t **val_out, lv_obj_t **bar_out)
{
    lv_obj_t *cell = fan_ui_box(parent);
    lv_obj_set_size(cell, FAN_CELL_W,
                    show_bar ? (FAN_LINE_H + FAN_BAR_SPACE + FAN_BAR_H) : FAN_LINE_H);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cell, FAN_BAR_SPACE, 0);

    lv_obj_t *top = fan_ui_box(cell);
    lv_obj_set_size(top, LV_PCT(100), FAN_LINE_H);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    fan_ui_one_line(fan_ui_label(top, name, FAN_C_KEY2));
    lv_obj_t *v = fan_ui_label(top, val, color);
    fan_ui_one_line(v);
    if(val_out) *val_out = v;

    if(show_bar) {
        lv_obj_t *bar = lv_bar_create(cell);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LV_PCT(100), FAN_BAR_H);
        lv_bar_set_range(bar, 0, bar_max);          /* 温度 0..80.0℃；内存 0..total_kb */
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(FAN_C_BARTRK), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, FAN_BAR_H / 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, FAN_BAR_H / 2, LV_PART_INDICATOR);
        if(bar_out) *bar_out = bar;
    }
    return cell;
}

/* ============================================================================
 * 动画层
 * ----------------------------------------------------------------------------
 *  三条原则，决定了「不僵硬」也不「乱晃」：
 *    1. 每条通道一个专属 exec_cb。LVGL 用 (var, exec_cb) 认人，start 时会自动
 *       删掉同一条通道上没跑完的旧动画 —— 所以高频调用只会重启动，不会堆叠。
 *    2. 重启时从「当前正在显示的值」接着走（下面各处都先把中间值固化），
 *       所以连续更新、中途反悔都不会闪一下。
 *    3. 不用 transform_zoom。缩放/旋转在 MCU 上走的是变换渲染，开销大还容易
 *       和裁剪打架；这里的「点亮/长大」全靠颜色和数值，480MHz 的 S3 很轻松。
 * ========================================================================= */

/** 把 LED 的补间状态画到屏幕上（纯幂等，随便调） */
static inline void fan_ui_led_render(fan_led_anim_t *s)
{
    lv_color_t dot = lv_color_mix(s->dot_to, s->dot_from, s->k);
    lv_color_t txt = lv_color_mix(s->txt_to, s->txt_from, s->k);

    /* 只有亮着的灯才呼吸；压暗时往页面底色靠，不会露出黑边 */
    if(s->on && s->breath < 255) {
        dot = lv_color_mix(dot, lv_color_hex(FAN_C_BG), s->breath);
    }

    /* 描边：亮起来的过程中渐隐，视觉上像「填满」而不是「套个圈」 */
    bool border = s->on ? (s->k < 128) : (s->k >= 128);

    lv_obj_set_style_border_width(s->dot, border ? 1 : 0, 0);
    lv_obj_set_style_bg_color(s->dot, dot, 0);
    lv_obj_set_style_text_color(s->txt, txt, 0);
}

static inline void fan_ui_anim_led_cb(void *var, int32_t v)
{
    fan_led_anim_t *s = (fan_led_anim_t *)var;
    if(v <= 0)        s->k = 0;
    else if(v >= 255) s->k = 255;
    else              s->k = (uint8_t)v;
    fan_ui_led_render(s);
}

/** 点亮/熄灭一个灯：从当前显示色渐变到目标色，delay 用来做级联 */
static inline void fan_ui_led_apply(fan_ui_t *ui, int id, bool on,
                                    uint32_t delay_ms, bool animate)
{
    static const uint32_t on_color[FAN_LED_NUM] = {
        FAN_C_GREEN, FAN_C_BLUE, FAN_C_BLUE, FAN_C_ORANGE, FAN_C_PURPLE
    };
    if(ui == NULL || id < 0 || id >= FAN_LED_NUM) return;

    fan_led_anim_t *s = &ui->led[id];
    if(s->on == on) return;                    /* 目标没变，一根手指都不动 */

    /* 把「当前正在显示的中间色」固化成新起点：中途被打断也不会跳 */
    s->dot_from = lv_color_mix(s->dot_to, s->dot_from, s->k);
    s->txt_from = lv_color_mix(s->txt_to, s->txt_from, s->k);
    s->k        = 0;
    s->on       = on;

    if(on) {
        s->dot_to = lv_color_hex(on_color[id]);
        s->txt_to = lv_color_hex(FAN_C_LABEL);
    }
    else {
        s->dot_to = lv_color_hex(FAN_C_DOT_OFF);
        s->txt_to = lv_color_hex(FAN_C_LABEL_OFF);
    }

    if(!animate || !FAN_UI_ANIM || delay_ms >= FAN_ANIM_LED) {
        s->k = 255;
        fan_ui_led_render(s);
        return;
    }

    fan_ui_led_render(s);                      /* 先按起点画一帧，延迟期间保持原样 */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, FAN_ANIM_LED);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_exec_cb(&a, fan_ui_anim_led_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/** 电源灯呼吸：一个来回 1.4s，只压一点亮度，颜色不变（像通电的指示灯） */
static inline void fan_ui_anim_breath_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    if(!ui->data.power) return;                /* 关机时不呼吸，连重绘都省掉 */

    fan_led_anim_t *s = &ui->led[FAN_LED_POWER];
    if(v < 0)   v = 0;
    if(v > 255) v = 255;
    s->breath = (uint8_t)v;
    fan_ui_led_render(s);
}

static inline void fan_ui_breath_start(fan_ui_t *ui)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui);
    lv_anim_set_values(&a, 255, 200);          /* 255 满亮 → 200 略暗 */
    lv_anim_set_time(&a, FAN_ANIM_BREATH);
    lv_anim_set_playback_time(&a, FAN_ANIM_BREATH);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, fan_ui_anim_breath_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/** 拨盘：弧和数字一起走，数字读到的是中间值，所以会「滚」上去 */
static inline void fan_ui_anim_speed_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    if(v < 0)   v = 0;
    if(v > 100) v = 100;
    ui->speed_cur = v;
    lv_arc_set_value(ui->dial, (int16_t)v);
    lv_label_set_text_fmt(ui->val_speed, "%u%%", (unsigned)v);
}

/** 芯片温度：条 + 数字（0.1℃ 整数） */
static inline void fan_ui_anim_temp_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    if(v < 0)   v = 0;
    if(v > 800) v = 800;
    ui->temp_cur = v;
    lv_bar_set_value(ui->bar_temp, (lv_coord_t)v, LV_ANIM_OFF);
    lv_label_set_text_fmt(ui->val_temp, "%d.%d℃", (int)(v / 10), (int)(v % 10));
}

/** 空闲内存：条 + 数字 */
static inline void fan_ui_anim_mem_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    lv_coord_t max = lv_bar_get_max_value(ui->bar_mem);
    if(v < 0)   v = 0;
    if(v > max) v = max;
    ui->mem_cur = v;
    lv_bar_set_value(ui->bar_mem, (lv_coord_t)v, LV_ANIM_OFF);
    lv_label_set_text_fmt(ui->val_mem, "%luKB", (unsigned long)v);
}

/** 电流：mA → "0.52A"（两位小数，取到 10mA） */
static inline void fan_ui_anim_cur_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    if(v < 0) v = 0;
    ui->cur_ma_cur = v;
    lv_label_set_text_fmt(ui->val_cur, "%d.%02dA", (int)(v / 1000), (int)((v % 1000) / 10));
}

/** 功率：mW → "6.24W"（两位小数，取到 10mW） */
static inline void fan_ui_anim_pw_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    if(v < 0) v = 0;
    ui->pw_mw_cur = v;
    lv_label_set_text_fmt(ui->val_pw, "%d.%02dW", (int)(v / 1000), (int)((v % 1000) / 10));
}

/** 轮毂描边：灰蓝 → 青蓝，代表「通电」 */
static inline void fan_ui_anim_ring_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    if(v < 0)   v = 0;
    if(v > 255) v = 255;
    ui->ring_cur = v;
    lv_obj_set_style_border_color(ui->hub,
        lv_color_mix(lv_color_hex(FAN_C_RING_ON), lv_color_hex(FAN_C_HUB_LINE), (uint8_t)v), 0);
}

/** 通用补间：from → to，走 ease_out（和网页 cubic-bezier 的「快进慢出」一致） */
static inline void fan_ui_anim_run(void *var, lv_anim_exec_xcb_t cb,
                                   int32_t from, int32_t to, uint16_t ms)
{
    if(cb == NULL) return;
    if(!FAN_UI_ANIM || ms == 0 || from == to) {
        cb(var, to);                            /* 没得动就直接落地 */
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/** 状态文字按当前数据落地（ON / OFF） */
static inline void fan_ui_state_text_apply(fan_ui_t *ui)
{
    const char *t = ui->data.power ? "ON" : "OFF";
    lv_label_set_text(ui->big_state, t);
    lv_label_set_text(ui->val_state, t);
}

/** 状态文字透明度（只动字形，不动整个对象，省内存也不走图层） */
static inline void fan_ui_anim_state_fade_cb(void *var, int32_t v)
{
    fan_ui_t *ui = (fan_ui_t *)var;
    if(v < 0)   v = 0;
    if(v > 255) v = 255;
    lv_obj_set_style_text_opa(ui->big_state, (lv_opa_t)v, 0);
    lv_obj_set_style_text_opa(ui->val_state, (lv_opa_t)v, 0);
}

/* 下面 fan_ui_state_transition() 要用，先声明 */
static inline void fan_ui_anim_state_ready(lv_anim_t *a);

/** OFF ⇄ ON：淡出 → （全透明时）换字 → 淡入，中间看不到跳变 */
static inline void fan_ui_state_transition(fan_ui_t *ui)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui);
    /* 起点取当前透明度：上一次淡入没跑完就被打断，也不会突然变实 */
    lv_anim_set_values(&a, (int32_t)lv_obj_get_style_text_opa(ui->big_state, 0), 0);
    lv_anim_set_time(&a, FAN_ANIM_FADE_OUT);
    lv_anim_set_exec_cb(&a, fan_ui_anim_state_fade_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, fan_ui_anim_state_ready);
    lv_anim_start(&a);
}

static inline void fan_ui_anim_state_ready(lv_anim_t *a)
{
    fan_ui_t *ui = (fan_ui_t *)a->var;

    /* 此刻文字是全透明的，换字用户看不见 —— 宽度变化也一起「藏」掉了 */
    fan_ui_state_text_apply(ui);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, ui);
    lv_anim_set_values(&b, 0, 255);
    lv_anim_set_time(&b, FAN_ANIM_FADE_IN);
    lv_anim_set_exec_cb(&b, fan_ui_anim_state_fade_cb);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_start(&b);
}

/* ============================================================================
 * 创建界面
 * ========================================================================= */
static inline fan_ui_t *fan_ui_create(lv_obj_t *parent)
{
    if(parent == NULL) parent = lv_scr_act();

    fan_ui_t *ui = (fan_ui_t *)lv_mem_alloc(sizeof(fan_ui_t));
    if(ui == NULL) return NULL;
    lv_memset_00(ui, sizeof(fan_ui_t));

    /* ---------------- 根容器：纵向 Flex ---------------- */
    ui->root = fan_ui_box(parent);
    lv_obj_set_size(ui->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->root, lv_color_hex(FAN_C_BG), 0);
    lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui->root, FAN_PAD, 0);
    lv_obj_set_style_pad_row(ui->root, FAN_GAP, 0);
    lv_obj_set_flex_flow(ui->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui->root,
                          LV_FLEX_ALIGN_START,      /* 主轴：从上往下排 */
                          LV_FLEX_ALIGN_CENTER,     /* 交叉轴：水平居中 */
                          LV_FLEX_ALIGN_CENTER);

    /* ---------------- 1. 标题 ---------------- */
    lv_obj_t *title = fan_ui_label(ui->root, "电风扇", FAN_C_TITLE);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(title, 8, 0);
    fan_ui_one_line(title);

    /* ---------------- 2. LED Bar ---------------- */
    static const char *led_name[FAN_LED_NUM] = {"电源", "自然", "摇头", "热源", "摄像"};

    lv_obj_t *ledbar = fan_ui_box(ui->root);
    lv_obj_set_size(ledbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ledbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ledbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ledbar, FAN_LED_COL, 0);

    for(int i = 0; i < FAN_LED_NUM; i++) {
        lv_obj_t *item = fan_ui_box(ledbar);
        lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(item, 3, 0);

        ui->led_dot[i] = fan_ui_box(item);
        lv_obj_set_size(ui->led_dot[i], FAN_LED_DOT, FAN_LED_DOT);
        lv_obj_set_style_radius(ui->led_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ui->led_dot[i], lv_color_hex(FAN_C_DOT_OFF), 0);
        lv_obj_set_style_bg_opa(ui->led_dot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ui->led_dot[i], 1, 0);
        lv_obj_set_style_border_color(ui->led_dot[i], lv_color_hex(FAN_C_DOT_LINE), 0);

        ui->led_txt[i] = fan_ui_label(item, led_name[i], FAN_C_LABEL_OFF);
        fan_ui_one_line(ui->led_txt[i]);

        /* 补间状态初始化：起点 = 终点 = 熄灭色，k=0 表示「完全停在起点」 */
        fan_led_anim_t *s = &ui->led[i];
        s->dot      = ui->led_dot[i];
        s->txt      = ui->led_txt[i];
        s->dot_from = lv_color_hex(FAN_C_DOT_OFF);
        s->dot_to   = lv_color_hex(FAN_C_DOT_OFF);
        s->txt_from = lv_color_hex(FAN_C_LABEL_OFF);
        s->txt_to   = lv_color_hex(FAN_C_LABEL_OFF);
        s->k        = 0;
        s->breath   = 255;
        s->on       = false;
    }

    /* ---------------- 3. 核心视觉区：圆弧拨盘 ---------------- */
    ui->dial = lv_arc_create(ui->root);
    lv_obj_remove_style_all(ui->dial);
    lv_obj_set_size(ui->dial, FAN_DIAL_SIZE, FAN_DIAL_SIZE);
    lv_obj_set_style_bg_opa(ui->dial, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->dial, 0, 0);
    lv_obj_set_style_pad_all(ui->dial, 0, 0);
    lv_arc_set_bg_angles(ui->dial, 135, 45);          /* 270° 护网弧，缺口朝下 */
    lv_arc_set_rotation(ui->dial, 0);
    lv_arc_set_range(ui->dial, 0, 100);
    lv_arc_set_value(ui->dial, 0);
    /* 轨道 */
    lv_obj_set_style_arc_width(ui->dial, FAN_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->dial, lv_color_hex(FAN_C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ui->dial, false, LV_PART_MAIN);
    /* 进度弧 */
    lv_obj_set_style_arc_width(ui->dial, FAN_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->dial, lv_color_hex(FAN_C_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui->dial, false, LV_PART_INDICATOR);
    /* 去掉旋钮（否则 value=0 时起点会有一个小圆点） */
    lv_obj_remove_style(ui->dial, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ui->dial, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(ui->dial, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(ui->dial, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui->dial, 0, LV_PART_KNOB);

    /* 中心轮毂 */
    ui->hub = fan_ui_box(ui->dial);
    lv_obj_t *hub = ui->hub;
    lv_obj_set_size(hub, FAN_HUB_SIZE, FAN_HUB_SIZE);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(FAN_C_HUB), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hub, 2, 0);
    lv_obj_set_style_border_color(hub, lv_color_hex(FAN_C_HUB_LINE), 0);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);

    ui->big_state = fan_ui_label(hub, "OFF", FAN_C_CYAN);
    lv_obj_align(ui->big_state, LV_ALIGN_CENTER, 0, 0);

    /* ---------------- 4. 状态 / 风速 胶囊 ---------------- */
    lv_obj_t *pill = fan_ui_box(ui->root);
    fan_ui_card_style(pill, FAN_PILL_H / 2);
    lv_obj_set_size(pill, LV_PCT(100), FAN_PILL_H);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 10, 0);

    fan_ui_one_line(fan_ui_label(pill, "状态", FAN_C_KEY2));
    ui->val_state = fan_ui_label(pill, "OFF", FAN_C_CYAN);
    fan_ui_one_line(ui->val_state);

    lv_obj_t *div = fan_ui_box(pill);
    lv_obj_set_size(div, 1, 14);
    lv_obj_set_style_bg_color(div, lv_color_hex(FAN_C_LINE), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    fan_ui_one_line(fan_ui_label(pill, "风速", FAN_C_KEY2));
    ui->val_speed = fan_ui_label(pill, "0%", FAN_C_CYAN);
    fan_ui_one_line(ui->val_speed);

    /* ---------------- 5. 设备状态卡：温度 / 内存 / 电流 / 功率 ---------------- */
    lv_obj_t *stat = fan_ui_box(ui->root);
    fan_ui_card_style(stat, 10);
    lv_obj_set_size(stat, LV_PCT(100), FAN_CARD_H);
    lv_obj_set_flex_flow(stat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(stat, FAN_CARD_PAD, 0);
    lv_obj_set_style_pad_row(stat, FAN_CARD_GAP, 0);

    /* 5.1 卡片标题：左边一条青色竖线 + 「设备状态」 */
    lv_obj_t *hd = fan_ui_box(stat);
    lv_obj_set_size(hd, LV_PCT(100), FAN_LINE_H);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hd, 5, 0);

    lv_obj_t *acc = fan_ui_box(hd);
    lv_obj_set_size(acc, 3, 11);
    lv_obj_set_style_radius(acc, 2, 0);
    lv_obj_set_style_bg_color(acc, lv_color_hex(FAN_C_CYAN), 0);
    lv_obj_set_style_bg_opa(acc, LV_OPA_COVER, 0);
    fan_ui_one_line(fan_ui_label(hd, "设备状态", FAN_C_KEY));

    /* 5.2 第一行：温度 / 内存（带细进度条） */
    lv_obj_t *row1 = fan_ui_box(stat);
    lv_obj_set_size(row1, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row1, FAN_CELL_GAP, 0);
    fan_ui_metric(row1, "温度", "0.0℃", FAN_C_ORANGE, 800,
                  true, &ui->val_temp, &ui->bar_temp);
    fan_ui_metric(row1, "内存", "0KB", FAN_C_GREEN, 100,
                  true, &ui->val_mem, &ui->bar_mem);

    /* 5.3 第二行：电流 / 功率（瞬时量，没有「满格」概念，只显示数字） */
    lv_obj_t *row2 = fan_ui_box(stat);
    lv_obj_set_size(row2, LV_PCT(100), FAN_LINE_H);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row2, FAN_CELL_GAP, 0);
    fan_ui_metric(row2, "电流", "0.00A", FAN_C_BLUE, 0, false, &ui->val_cur, NULL);
    fan_ui_metric(row2, "功率", "0.00W", FAN_C_PURPLE, 0, false, &ui->val_pw, NULL);

    /* 电源灯呼吸：常驻一条无限循环动画，关机时回调自己空转，开销可忽略 */
#if FAN_UI_LED_BREATH && FAN_UI_ANIM
    fan_ui_breath_start(ui);
#endif

    return ui;
}

/* ============================================================================
 * 数据刷新：界面所有可变内容都在这里更新
 * ========================================================================= */

/** 只刷一个 LED 灯，给需要单独联动的地方用（会走渐变） */
static inline void fan_ui_set_led(fan_ui_t *ui, fan_led_t id, bool on)
{
    fan_ui_led_apply(ui, (int)id, on, 0, FAN_UI_ANIM != 0);
}

/** 全量刷新：一次把状态、风速、LED、拨盘、温度、内存全写完 */
static inline void fan_ui_update(fan_ui_t *ui, const fan_ui_data_t *d)
{
    if(ui == NULL || d == NULL) return;

    const bool first         = !ui->ready;
    const bool power_changed = (ui->data.power != d->power);
    ui->data                 = *d;      /* 先落地，动画回调里读的是新数据 */

    /* --- 1. 状态文字：只有开关变了才做「淡出→换字→淡入」 ---
     *     其余时候一个字都不碰 —— 否则中途来的数据包会把淡入淡出打断 */
    if(first) {
        fan_ui_state_text_apply(ui);
        lv_obj_set_style_text_opa(ui->big_state, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(ui->val_state, LV_OPA_COVER, 0);
    }
    else if(power_changed) {
        if(FAN_UI_ANIM) fan_ui_state_transition(ui);
        else            fan_ui_state_text_apply(ui);   /* 关动画时直接换字 */
    }

    /* --- 2. 轮毂描边跟着开关亮/暗 --- */
    fan_ui_anim_run(ui, fan_ui_anim_ring_cb,
                    ui->ring_cur, d->power ? 255 : 0, FAN_ANIM_RING);

    /* --- 3. 5 个 LED：权限跟随 power，和原网页逻辑一致 ---
     *     开机从头到尾依次亮（关机反着来），35ms 一级的阶梯感 */
    const bool led_on[FAN_LED_NUM] = {
        d->power,
        d->power && d->nature,
        d->power && d->osc,
        d->power && d->heat,
        d->power && d->cam
    };
    for(int i = 0; i < FAN_LED_NUM; i++) {
        uint32_t delay = 0;
        if(power_changed && !first) {
            int order = d->power ? i : (FAN_LED_NUM - 1 - i);
            delay = (uint32_t)order * FAN_ANIM_LED_STEP;
        }
        fan_ui_led_apply(ui, i, led_on[i], delay, !first);
    }

    /* --- 4. 拨盘进度弧：关机时归零（数字跟着弧一起滚） --- */
    int32_t speed_target = d->power ? (int32_t)d->speed : 0;
    if(speed_target > 100) speed_target = 100;
    fan_ui_anim_run(ui, fan_ui_anim_speed_cb,
                    ui->speed_cur, speed_target, FAN_ANIM_SPEED);

    /* --- 5. 芯片温度 --- */
    int32_t t = d->temp_x10;
    if(t < 0)   t = 0;
    if(t > 800) t = 800;
    fan_ui_anim_run(ui, fan_ui_anim_temp_cb, ui->temp_cur, t, FAN_ANIM_BAR);

    /* --- 6. 空闲内存 --- */
    uint32_t total = d->total_kb ? d->total_kb : 1;    /* 防除零 */
    uint32_t free_kb = d->free_kb > total ? total : d->free_kb;
    lv_bar_set_range(ui->bar_mem, 0, (lv_coord_t)total);
    if(ui->mem_cur > (int32_t)total) ui->mem_cur = (int32_t)total;   /* 量程缩了先夹住 */
    fan_ui_anim_run(ui, fan_ui_anim_mem_cb,
                    ui->mem_cur, (int32_t)free_kb, FAN_ANIM_BAR);

    /* --- 7. 电流 / 功率（mA、mW 整数传递，0.01A / 0.01W 的精度） ---
     *     这两个是瞬时量，关机时不一定为 0（有待机功耗），所以不强制归零，
     *     上报多少就显示多少；想让关机显示 0.00A，就在上报侧自己清零。 */
    fan_ui_anim_run(ui, fan_ui_anim_cur_cb,
                    ui->cur_ma_cur, (int32_t)d->current_ma, FAN_ANIM_BAR);
    fan_ui_anim_run(ui, fan_ui_anim_pw_cb,
                    ui->pw_mw_cur, (int32_t)d->power_mw, FAN_ANIM_BAR);

    ui->ready = true;
}

/** 取回当前数据副本，方便在本地改一个字段再整体刷新 */
static inline fan_ui_data_t *fan_ui_data(fan_ui_t *ui)
{
    return ui ? &ui->data : NULL;
}

/* ============================================================================
 * 可选：假数据演示（验证「数据可变」）
 *   定义 FAN_UI_DEMO 后，每 800ms 自动改一次数据，界面会跟着动：
 *       #define FAN_UI_DEMO
 *       #include "fan_ui.h"
 *   正式固件不要定义。
 * ========================================================================= */
#ifdef FAN_UI_DEMO
static inline void fan_ui_demo_cb(lv_timer_t *tm)
{
    fan_ui_t *ui = (fan_ui_t *)tm->user_data;
    if(ui == NULL) return;

    fan_ui_data_t d = ui->data;
    static uint8_t phase = 0;
    phase = (uint8_t)((phase + 1) % 8);

    d.power    = (phase % 4) != 3;
    d.speed    = (uint8_t)(25 + phase * 10);
    d.nature   = (phase & 1) != 0;
    d.osc      = (phase & 2) != 0;
    d.heat     = (phase >= 4);
    d.cam      = (phase >= 6);
    d.temp_x10 = (int16_t)(380 + phase * 9);          /* 38.0 ~ 44.3 ℃ */
    d.free_kb  = 120 + phase * 12;
    d.total_kb = 270;
    /* 电流跟着风速走（关机留一点待机电流），功率按 12V 估算 */
    if(d.power) {
        d.current_ma = (uint16_t)(240 + phase * 55);  /* 0.24 ~ 0.63 A */
        d.power_mw   = (uint32_t)d.current_ma * 12u;  /* 2.88 ~ 7.56 W */
    }
    else {
        d.current_ma = 18;                            /* 0.18 A 待机 */
        d.power_mw   = 22;
    }

    fan_ui_update(ui, &d);
}

static inline lv_timer_t *fan_ui_demo_start(fan_ui_t *ui, uint32_t period_ms)
{
    lv_timer_t *tm = lv_timer_create(fan_ui_demo_cb, period_ms ? period_ms : 800, ui);
    return tm;
}
#endif /* FAN_UI_DEMO */

#endif /* FAN_UI_INCLUDED */
