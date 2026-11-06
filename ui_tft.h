/**
 * ============================================================================
 *  ui_tft.h  —  ST7789 240x320 + LVGL 8.4 状态屏
 * ----------------------------------------------------------------------------
 *  界面本体来自子测试项目的 fan_ui.h（那份是验证过的，原样搬过来不动），
 *  本文件只负责三件事：
 *      1. 把屏幕/LVGL 跑起来（引脚、SPI 端口、刷新回调都在这里）
 *      2. 把 g_cmd / g_rt / 功率数据 拼成 fan_ui_data_t，定期推给界面
 *      3. 提供一个盖在最上层的文字浮层（开机校准、配网提示用）
 *
 *  ★ TFT_eSPI 的引脚和驱动不在代码里，在库里：
 *      Arduino/libraries/TFT_eSPI/User_Setup.h
 *      本工程的 ref/TFT_eSPI_User_Setup.h 是那份配置的备份，换电脑时照着贴。
 *    fan_config.h 顶部有编译期校验，引脚/型号不对会直接编译报错。
 *
 *  ★ SPI 端口必须定义 USE_HSPI_PORT（已在 User_Setup.h 里）：
 *      core 3.x 里若不定义，SPI_PORT 会算成 0，寄存器地址变成 0x10，
 *      第一次 tft.init() 就 StoreProhibited 重启。这是实机踩过的坑。
 * ============================================================================
 */
#ifndef UI_TFT_H
#define UI_TFT_H

#ifndef LV_CONF_INCLUDE_SIMPLE
  #define LV_CONF_INCLUDE_SIMPLE      /* 让 LVGL 用 sketch 目录里的 lv_conf.h */
#endif

#include <TFT_eSPI.h>
#include <lvgl.h>

#include "fan_config.h"
#include "fan_types.h"
#include "fan_ui.h"
#include "power_monitor.h"

/* ---------------------------------------------------------------------------
 * 编译期自检（和子测试项目一致，出问题锁在编译阶段）
 * ------------------------------------------------------------------------- */
#if !defined(USE_HSPI_PORT) && !defined(USE_FSPI_PORT)
  #error "TFT_eSPI 必须定义 USE_HSPI_PORT（推荐）或 USE_FSPI_PORT，否则 SPI_PORT=0，上电即崩"
#endif
#if LVGL_VERSION_MAJOR != 8
  #error "本工程按 LVGL 8.x 编写，请把 lvgl 装成 8.4.0"
#endif
#if LV_COLOR_DEPTH != 16
  #error "LV_COLOR_DEPTH 必须为 16（ST7789 是 RGB565）"
#endif
#if LV_COLOR_16_SWAP != 0
  #error "LV_COLOR_16_SWAP 必须为 0：颜色交换在 flush 里用 pushColors(...,true) 做"
#endif
#if !defined(FAN_LV_CONF_V84)
  #error "lv_conf.h 没生效：sketch 目录下要有本工程的 lv_conf.h"
#endif

namespace ui {

#define UI_W 240
#define UI_H 320

static TFT_eSPI           tft;
static lv_disp_draw_buf_t drawBuf;
static lv_color_t         drawBufMem[UI_W * 40];      /* 40 行 ≈ 19KB */
static lv_disp_drv_t      dispDrv;
static fan_ui_t          *fanUi = nullptr;

static lv_obj_t *ovlBox   = nullptr;
static lv_obj_t *ovlLabel = nullptr;

static uint32_t lastTickMs    = 0;
static uint32_t lastRefreshMs = 0;

/* ---------------------------------------------------------------------------
 * LVGL → 屏幕
 *   pushColors(..., true) 每个 uint16 高字节先发，正是 ST7789 要的 RGB565 顺序。
 *   改成 false 会变成彩色噪点。
 * ------------------------------------------------------------------------- */
static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, (int32_t)w, (int32_t)h);
    tft.pushColors((uint16_t *)px_map, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(drv);
}

/* 中文字体自检：缺字时 LVGL 不报错只画空白，开机主动查一次 */
static bool fontOk()
{
    const uint32_t probe[] = { 0x7535, 0x98CE, 0x6E29, 0x2103, 0x8BBE, 0x5907, 0x72B6, 0x6001 };
    lv_font_glyph_dsc_t dsc;
    for (uint32_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        if (!lv_font_get_glyph_dsc(&font_cn, &dsc, probe[i], 0)) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * 浮层：盖在最上层的一行大字（校准进度、配网提示）
 * ------------------------------------------------------------------------- */
static void overlayMake()
{
    if (ovlBox) return;
    ovlBox = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ovlBox);
    lv_obj_set_size(ovlBox, UI_W, UI_H);
    lv_obj_set_style_bg_color(ovlBox, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_bg_opa(ovlBox, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ovlBox, LV_OBJ_FLAG_SCROLLABLE);

    ovlLabel = lv_label_create(ovlBox);
    lv_obj_set_style_text_font(ovlLabel, &font_cn, 0);
    lv_obj_set_style_text_color(ovlLabel, lv_color_hex(0x8FE0FF), 0);
    lv_obj_set_style_text_align(ovlLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ovlLabel, "");
    lv_obj_center(ovlLabel);
}

static void overlayText(const char *txt)
{
    overlayMake();
    lv_label_set_text(ovlLabel, txt);
    lv_obj_center(ovlLabel);
    lv_timer_handler();          /* 立刻画一帧，别等下一个刷新周期 */
}

static void overlayHide()
{
    if (!ovlBox) return;
    lv_obj_del(ovlBox);
    ovlBox = nullptr;
    ovlLabel = nullptr;
}

/* ---------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------- */
static bool begin()
{
    tft.init();
    tft.setRotation(0);                 /* 0 = 竖屏 240x320 */
    tft.invertDisplay(true);            /* 颜色反了就改 false */
    tft.fillScreen(TFT_BLACK);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

    lv_init();
    lv_disp_draw_buf_init(&drawBuf, drawBufMem, NULL, UI_W * 40);

    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res  = UI_W;
    dispDrv.ver_res  = UI_H;
    dispDrv.flush_cb = flushCb;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    Serial.printf("[屏幕] 中文字体自检：%s\n", fontOk() ? "正常" : "缺字!");

    fanUi = fan_ui_create(NULL);
    if (fanUi == NULL) {
        Serial.println(F("[屏幕] LVGL 内存不足，界面创建失败"));
        return false;
    }
    lastTickMs = millis();
    return true;
}

/* ---------------------------------------------------------------------------
 * 心跳：必须每个 loop 都叫（LVGL 8 的 tick 要自己喂）
 * ------------------------------------------------------------------------- */
static void tick()
{
    uint32_t now = millis();
    lv_tick_inc(now - lastTickMs);
    lastTickMs = now;
    lv_timer_handler();
}

/* ---------------------------------------------------------------------------
 * 数据刷新：把全局状态拼成 fan_ui_data_t 推过去
 *   fan_ui_update() 是全量提交，只有真的变了的字段才起动画，100ms 叫一次都不亏。
 * ------------------------------------------------------------------------- */
static void refresh()
{
    if (!fanUi) return;
    if (millis() - lastRefreshMs < UI_PERIOD_MS) return;
    lastRefreshMs = millis();

    fan_ui_data_t d;
    d.power     = g_rt.blowing;                 /* 屏幕显示“真正在吹”的状态 */
    d.speed     = g_rt.fanPct;
    d.nature    = g_cmd.nature;
    d.osc       = g_cmd.osc;
    d.heat      = g_cmd.heatOn;
    d.cam       = g_cmd.camOn;
    d.timer_min = (int16_t)g_cmd.timerMin;
    d.temp_x10  = (int16_t)(temperatureRead() * 10.0f);
    d.free_kb   = ESP.getFreeHeap() / 1024;
    d.total_kb  = ESP.getHeapSize() / 1024;
    d.current_ma = (uint16_t)(power::pdata.currentMa < 0 ? 0 : power::pdata.currentMa);
    d.power_mw   = (uint32_t)(power::pdata.powerMw < 0 ? 0 : power::pdata.powerMw);
    d.uptime_s   = millis() / 1000;

    fan_ui_update(fanUi, &d);
}

} /* namespace ui */

#endif /* UI_TFT_H */
