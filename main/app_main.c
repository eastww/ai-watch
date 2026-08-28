/*
 * app_main.c - 智能沟通助理 入口
 *
 * 里程碑：
 *  M1 ✅ 屏幕 + 触摸 + LVGL 基础 UI
 *  M2  音频：ES8311 播放 / ES7210 录音
 *  M3  语音链路：WiFi + ASR + TTS
 *  M4  智能对话：接入 DeepSeek
 *  M5  本地唤醒词：ESP-SR
 *  M6  打磨：省电 / 抬手亮屏 / 错误处理
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "bsp/esp32_s3_touch_lcd_1_85B.h"

static const char *TAG = "ai_watch";

/* ============ UI（M1 占位，后续拆到 src/ui/） ============ */

static void ui_status_update(lv_timer_t *timer)
{
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(timer);
    /* 时钟 / 电量占位：M1 只显示运行时间 */
    uint32_t sec = esp_timer_get_time() / 1000000ULL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60),
             (unsigned long)(sec % 60));
    lv_label_set_text(label, buf);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
}

static void ui_main_screen_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "AI WATCH");
    lv_obj_set_style_text_color(title, lv_color_hex(0x40C4FF), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *clock = lv_label_create(scr);
    lv_label_set_text(clock, "00:00:00");
    lv_obj_set_style_text_font(clock, &lv_font_montserrat_14, 0);
    lv_obj_align(clock, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Touch to wake\nM1: display + touch OK");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    /* 每秒刷新时钟 */
    lv_timer_create(ui_status_update, 1000, clock);
}

/* ============ 入口 ============ */

void app_main(void)
{
    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 显示 + 触摸（BSP 内部完成 LVGL 初始化） */
    lv_display_t *disp = bsp_display_start();
    assert(disp);
    lv_indev_t *tp = bsp_display_get_input_dev();
    assert(tp);
    bsp_display_backlight_on();

    /* 3. UI */
    ui_main_screen_create();

    /* 4. LVGL 任务循环 */
    ESP_LOGI(TAG, "ai-watch running (M1)");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}
