/*
 * app_main.c - AI Watch 主程序
 *
 * M1：点亮 360x360 圆形屏（LVGL 9）+ 触摸反馈。
 *
 * ⚠️ 关键：不要手动调用 lv_timer_handler()！
 * BSP 的 bsp_display_start() 内部会启动 esp_lv_adapter 的 worker 任务，
 * 它负责循环驱动 LVGL 刷新。业务代码只需用 bsp_display_lock()/unlock()
 * 保护所有 LVGL API 调用（LVGL 非线程安全）。
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

static lv_obj_t *clock_label = NULL;
static lv_obj_t *hint_label = NULL;

/* 每秒刷新时钟 */
static void ui_status_update(lv_timer_t *timer)
{
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(timer);
    uint32_t sec = esp_timer_get_time() / 1000000ULL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60),
             (unsigned long)(sec % 60));
    lv_label_set_text(label, buf);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
}

/* 屏幕触摸事件回调（LVGL 事件机制；不要覆盖 BSP 的 indev read_cb） */
static void screen_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Touch clicked!");
        if (hint_label) {
            lv_label_set_text(hint_label, "Touch OK!\nGreat job!");
        }
    }
}

/* 创建主界面 */
static void ui_main_screen_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    /* 绑定屏幕触摸事件 */
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "AI WATCH");
    lv_obj_set_style_text_color(title, lv_color_hex(0x40C4FF), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    clock_label = lv_label_create(scr);
    lv_label_set_text(clock_label, "00:00:00");
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_14, 0);
    lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, -10);

    hint_label = lv_label_create(scr);
    lv_label_set_text(hint_label, "Touch to test\nWorking...");
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, 40);

    lv_timer_create(ui_status_update, 1000, clock_label);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting AI Watch...");

    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 显示 + 触摸（BSP 内部启动 LVGL worker 任务） */
    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "Display init failed!");
        return;
    }
    ESP_LOGI(TAG, "Display initialized");

    lv_indev_t *tp = bsp_display_get_input_dev();
    if (!tp) {
        ESP_LOGE(TAG, "Touch init failed!");
        return;
    }
    ESP_LOGI(TAG, "Touch device found (BSP read_cb active)");

    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Backlight on");

    /* 4. 创建 UI（持锁操作 LVGL） */
    bsp_display_lock(-1);
    ui_main_screen_create();
    bsp_display_unlock();
    ESP_LOGI(TAG, "UI created");

    ESP_LOGI(TAG, "AI Watch ready! Touch screen to see response...");

    /* 5. 主任务仅做周期性状态日志；LVGL 刷新由 esp_lv_adapter worker 驱动 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        ESP_LOGI(TAG, "System running...");
    }
}