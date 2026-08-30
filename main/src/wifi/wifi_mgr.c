/*
 * wifi_mgr.c - WiFi 连接管理实现（STA 模式）
 *
 * 事件驱动：WIFI_EVENT_STA_START/STOP/DISCONNECTED + IP_EVENT_STA_GOT_IP。
 * 断线自动重连（带重试次数限制，超限进入 DISCONNECTED 供 UI 提示）。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "config.h"
#include "wifi/wifi_mgr.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

#define MAX_CB_NUM 4
typedef struct {
    wifi_mgr_state_cb_t cb;
    void *ctx;
} wifi_cb_t;

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif = NULL;
static wifi_mgr_state_t s_state = WIFI_MGR_STATE_DOWN;
static int s_retry_num = 0;
static char s_ip_str[16] = "0.0.0.0";

/* 回调注册表 */
static wifi_cb_t s_cbs[MAX_CB_NUM];
static int s_cb_cnt = 0;

static void set_state(wifi_mgr_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    ESP_LOGI(TAG, "state -> %s", st == WIFI_MGR_STATE_CONNECTED ? "CONNECTED" :
                                   st == WIFI_MGR_STATE_CONNECTING ? "CONNECTING" :
                                   st == WIFI_MGR_STATE_DISCONNECTED ? "DISCONNECTED" : "DOWN");
    for (int i = 0; i < s_cb_cnt; i++) {
        if (s_cbs[i].cb) {
            s_cbs[i].cb(st, s_cbs[i].ctx);
        }
    }
}

void wifi_mgr_register_cb(wifi_mgr_state_cb_t cb, void *ctx)
{
    if (!cb || s_cb_cnt >= MAX_CB_NUM) {
        return;
    }
    s_cbs[s_cb_cnt].cb = cb;
    s_cbs[s_cb_cnt].ctx = ctx;
    s_cb_cnt++;
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    return s_state;
}

bool wifi_mgr_is_connected(void)
{
    return s_state == WIFI_MGR_STATE_CONNECTED;
}

char *wifi_mgr_get_ip_str(void)
{
    return s_ip_str;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        set_state(WIFI_MGR_STATE_CONNECTING);
        ESP_LOGI(TAG, "STA start, connecting to '%s'...", WIFI_SSID);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == WIFI_MGR_STATE_CONNECTING || s_state == WIFI_MGR_STATE_CONNECTED) {
            ESP_LOGW(TAG, "disconnected, retrying %d/%d", s_retry_num + 1, WIFI_MAX_RETRY);
            if (s_retry_num < WIFI_MAX_RETRY) {
                s_retry_num++;
                esp_wifi_connect();
                set_state(WIFI_MGR_STATE_CONNECTING);
            } else {
                s_retry_num = 0;
                set_state(WIFI_MGR_STATE_DISCONNECTED);
                xEventGroupSetBits(s_evt, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "got ip: %s", s_ip_str);
        s_retry_num = 0;
        set_state(WIFI_MGR_STATE_CONNECTED);
        xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
    }
}

void wifi_mgr_disconnect(void)
{
    if (s_state == WIFI_MGR_STATE_DOWN) {
        return;
    }
    esp_wifi_disconnect();
    set_state(WIFI_MGR_STATE_DOWN);
}

void wifi_mgr_reconnect(void)
{
    s_retry_num = 0;
    esp_wifi_connect();
    set_state(WIFI_MGR_STATE_CONNECTING);
}

bool wifi_mgr_init(void)
{
    /* SSID 未配置（模板默认值）则不上电 WiFi */
    if (strcmp(WIFI_SSID, "your_ssid") == 0 || strlen(WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "WiFi SSID not configured, skip");
        return false;
    }

    if (!s_evt) {
        s_evt = xEventGroupCreate();
    }
    /* 若 NVS 未初始化，做一次（幂等） */
    static bool nvs_ready = false;
    if (!nvs_ready) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        nvs_ready = true;
    }

    /* netif + 事件循环只需初始化一次 */
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed %d", ret);
        return false;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed %d", ret);
        return false;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    assert(s_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi init failed %d", ret);
        return false;
    }

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    /* 允许超过 32 字符的 SSID/密码 */
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi mgr ready");
    return true;
}