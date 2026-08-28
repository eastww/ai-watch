/*
 * app_state.c - 应用状态机实现
 *
 * 状态：IDLE → WAKE → LISTEN → THINK → SPEAK → IDLE
 * UI 通过注册回调感知状态切换并渲染对应界面/动画。
 */
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "app_state.h"

static const char *TAG = "app_state";

static app_state_t s_state = APP_STATE_IDLE;
static app_state_change_cb_t s_cb = NULL;
static SemaphoreHandle_t s_mutex = NULL;

static const char *state_name(app_state_t s)
{
    switch (s) {
    case APP_STATE_IDLE:   return "IDLE";
    case APP_STATE_WAKE:   return "WAKE";
    case APP_STATE_LISTEN: return "LISTEN";
    case APP_STATE_THINK:  return "THINK";
    case APP_STATE_SPEAK:  return "SPEAK";
    default:               return "?";
    }
}

void app_state_init(app_state_change_cb_t cb)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    s_cb = cb;
    s_state = APP_STATE_IDLE;
    ESP_LOGI(TAG, "state machine ready (IDLE)");
}

app_state_t app_state_get(void)
{
    app_state_t ret;
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY)) {
        ret = s_state;
        xSemaphoreGive(s_mutex);
        return ret;
    }
    return s_state;
}

void app_state_set(app_state_t state)
{
    if (state >= APP_STATE_MAX) {
        return;
    }
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY)) {
        if (s_state != state) {
            ESP_LOGI(TAG, "state: %s -> %s", state_name(s_state), state_name(state));
            s_state = state;
            xSemaphoreGive(s_mutex);
            if (s_cb) {
                s_cb(state);
            }
            return;
        }
        xSemaphoreGive(s_mutex);
    }
}