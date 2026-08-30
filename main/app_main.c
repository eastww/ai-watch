/*
 * app_main.c - AI Watch 主程序
 *
 * M1：点亮 360x360 圆形屏（LVGL 9）+ 触摸反馈。
 * M2：音频初始化 + 开机提示音；触摸触发"录音1s → 回放"自测。
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
#include "audio/audio.h"
#include "wifi/wifi_mgr.h"

static const char *TAG = "ai_watch";

static lv_obj_t *clock_label = NULL;
static lv_obj_t *hint_label = NULL;
static lv_obj_t *wifi_label = NULL;

/* ============ M2 音频自测（录音1s → 回放） ============ */
#define M2_REC_SECONDS 1
static int16_t s_rec_buf[M2_REC_SECONDS * 16000];
static volatile size_t s_rec_len = 0;
static volatile bool s_rec_done = false;

static bool m2_record_cb(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    size_t max_samples = sizeof(s_rec_buf) / sizeof(int16_t);
    if (s_rec_len + samples <= max_samples) {
        memcpy(s_rec_buf + s_rec_len, pcm, samples * sizeof(int16_t));
        s_rec_len += samples;
    }
    if (s_rec_len >= (size_t)(M2_REC_SECONDS * 16000)) {
        s_rec_done = true;
        return false; /* 停止录制 */
    }
    return true;
}

static void m2_audio_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "M2 test: recording %ds...", M2_REC_SECONDS);

    s_rec_len = 0;
    s_rec_done = false;
    if (!audio_record_start(16000, m2_record_cb, NULL)) {
        ESP_LOGE(TAG, "M2 record start failed");
        if (hint_label) {
            lv_label_set_text(hint_label, "Record FAIL\ncheck log");
        }
        vTaskDelete(NULL);
        return;
    }

    /* 等待录满或超时（最多 M2_REC_SECONDS+2 秒） */
    uint32_t waits = 0;
    while (!s_rec_done && waits < (M2_REC_SECONDS + 2) * 20) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waits++;
    }
    audio_record_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "M2 test: captured %d samples, playing back...", (int)s_rec_len);
    if (s_rec_len > 0) {
        audio_play_pcm(s_rec_buf, s_rec_len);
        if (hint_label) {
            lv_label_set_text(hint_label, "Playback OK!\nM2 audio pass");
        }
    } else {
        if (hint_label) {
            lv_label_set_text(hint_label, "No audio\ncheck MIC");
        }
    }
    vTaskDelete(NULL);
}

/* ============ M3: WiFi 状态显示 ============ */

/* WiFi 状态变化回调（事件任务上下文调用；更新 LVGL 需持锁） */
static void wifi_state_cb(wifi_mgr_state_t state, void *ctx)
{
    (void)ctx;
    const char *txt = NULL;
    switch (state) {
    case WIFI_MGR_STATE_CONNECTING:    txt = "WiFi: connecting"; break;
    case WIFI_MGR_STATE_CONNECTED:     txt = "WiFi: connected";  break;
    case WIFI_MGR_STATE_DISCONNECTED:  txt = "WiFi: lost";       break;
    default:                           txt = "WiFi: off";        break;
    }
    ESP_LOGI(TAG, "%s (ip=%s)", txt, wifi_mgr_get_ip_str());
    if (!wifi_label) {
        return;
    }
    bsp_display_lock(-1);
    if (state == WIFI_MGR_STATE_CONNECTED) {
        char buf[48];
        snprintf(buf, sizeof(buf), "WiFi: %s", wifi_mgr_get_ip_str());
        lv_label_set_text(wifi_label, buf);
        lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x00E676), 0);
    } else {
        lv_label_set_text(wifi_label, txt);
        lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xFF5252), 0);
    }
    bsp_display_unlock();
}

/* ============ UI ============ */

/* 屏幕触摸事件回调（LVGL 事件机制；不要覆盖 BSP 的 indev read_cb） */
static void screen_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Touch clicked! Starting M2 audio self-test...");
        if (hint_label) {
            lv_label_set_text(hint_label, "Recording 1s...");
        }
        xTaskCreate(m2_audio_test_task, "m2_audio", 4096, NULL, 5, NULL);
    }
}

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
    lv_label_set_text(hint_label, "Touch to test\nAudio M2");
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, 40);

    /* M3: WiFi 状态（顶部） */
    wifi_label = lv_label_create(scr);
    lv_label_set_text(wifi_label, "WiFi: ...");
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(wifi_label, LV_ALIGN_TOP_MID, 0, 8);

    lv_timer_create(ui_status_update, 1000, clock_label);
}

/* ============ 入口 ============ */

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

    /* 3. M2：初始化音频 */
    if (!audio_play_init(16000)) {
        ESP_LOGE(TAG, "M2 audio init failed");
    } else {
        ESP_LOGI(TAG, "M2 audio ready");
    }

    /* 4. 创建 UI（持锁操作 LVGL） */
    bsp_display_lock(-1);
    ui_main_screen_create();
    bsp_display_unlock();
    ESP_LOGI(TAG, "UI created");

    /* 5. 播放开机提示音（验证扬声器） */
    audio_play_tone(660, 100);
    vTaskDelay(pdMS_TO_TICKS(120));
    audio_play_tone(990, 150);
    ESP_LOGI(TAG, "M2 welcome tone played");

    /* 6. M3：初始化 WiFi（异步连接；注册状态回调更新屏幕） */
    wifi_mgr_register_cb(wifi_state_cb, NULL);
    if (wifi_mgr_init()) {
        ESP_LOGI(TAG, "M3 WiFi starting...");
    } else {
        ESP_LOGW(TAG, "M3 WiFi not configured (set SSID/password in secrets.h)");
    }

    ESP_LOGI(TAG, "AI Watch ready! Touch screen to test audio...");

    /* 7. 主任务仅做周期性状态日志；LVGL 刷新由 esp_lv_adapter worker 驱动 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        ESP_LOGI(TAG, "System running...");
    }
}