/*
 * asr_client.c - 讯飞流式听写（iat）客户端实现
 *
 * 协议：WebSocket (wss://iat-api.xfyun.cn/v2/iat)
 *  - 首帧：common.appid + business(language/domain/accent/vad_eos) + data(status=0)
 *  - 音频帧：data.status=1 + audio(16k/16bit mono PCM 的 base64)
 *  - 结束帧：data.status=2 + audio=""
 *  - 返回：增量文本（data.result.ws[].cw[].w），data.status==2 为最终结果
 *
 * 鉴权：RFC1123 date + HMAC-SHA256(APISecret) 签名 + base64，拼接到 ws url 查询参数。
 * 依赖系统时间（SNTP），时间未同步时 asr_start 返回 false。
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "cJSON.h"

#include "config.h"
#include "cloud/cloud.h"

static const char *TAG = "asr";

#define ASR_HOST        "iat-api.xfyun.cn"
#define ASR_PATH        "/v2/iat"
#define ASR_BUFFER_SIZE 4096
#define ASR_AUTH_BUF    1024

/* 语音块最大字节（PCM 16k/16bit）：64ms 上限，防止帧过大 */
#define ASR_MAX_PCM_BYTES  (ASR_SAMPLE_RATE * 2 * 64 / 1000)

typedef enum {
    ASR_IDLE = 0,
    ASR_CONNECTING,
    ASR_STREAMING,
} asr_state_t;

static esp_websocket_client_handle_t s_ws = NULL;
static asr_text_cb_t s_cb = NULL;
static void *s_ctx = NULL;
static SemaphoreHandle_t s_lock = NULL;
static asr_state_t s_state = ASR_IDLE;
static bool s_sent_first = false;
static char s_auth_uri[512];

/* ---------- 工具：URL 编码（RFC3986 unreserved 之外全部 %XX） ---------- */
static void urlencode(const char *src, char *dst, size_t dst_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 3 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

/* ---------- 工具：base64 ---------- */
static int b64(const unsigned char *in, size_t in_len, char *out, size_t out_sz)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, in, in_len) != 0) {
        return -1;
    }
    return (int)olen;
}

/* ---------- 构建带鉴权的 WebSocket URL ---------- */
static bool build_auth_uri(char *out, size_t out_sz)
{
    /* 1. RFC1123 GMT 时间 */
    time_t now = time(NULL);
    if (now < 1000000000L) { /* 未同步（<2001 年） */
        ESP_LOGW(TAG, "system time not synced yet, ASR auth unavailable");
        return false;
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    /* 2. signature_origin */
    char origin[256];
    snprintf(origin, sizeof(origin),
             "host: %s\ndate: %s\nGET %s HTTP/1.1", ASR_HOST, date, ASR_PATH);

    /* 3. HMAC-SHA256(APISecret, origin) -> base64 */
    unsigned char mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, (const unsigned char *)SECRET_ASR_API_SECRET,
                        strlen(SECRET_ASR_API_SECRET),
                        (const unsigned char *)origin, strlen(origin), mac) != 0) {
        ESP_LOGE(TAG, "hmac failed");
        return false;
    }
    char sig_b64[128];
    if (b64(mac, sizeof(mac), sig_b64, sizeof(sig_b64)) < 0) {
        return false;
    }

    /* 4. authorization_origin -> base64 */
    char auth_origin[512];
    snprintf(auth_origin, sizeof(auth_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             SECRET_ASR_API_KEY, sig_b64);
    char auth_b64[256];
    if (b64((const unsigned char *)auth_origin, strlen(auth_origin), auth_b64, sizeof(auth_b64)) < 0) {
        return false;
    }

    /* 5. url 编码并拼接 */
    char auth_enc[300], date_enc[128], host_enc[64];
    urlencode(auth_b64, auth_enc, sizeof(auth_enc));
    urlencode(date, date_enc, sizeof(date_enc));
    urlencode(ASR_HOST, host_enc, sizeof(host_enc));

    snprintf(out, out_sz,
             "wss://%s%s?authorization=%s&date=%s&host=%s",
             ASR_HOST, ASR_PATH, auth_enc, date_enc, host_enc);
    return true;
}

/* ---------- 发送 JSON 文本帧 ---------- */
static bool send_json(const char *json)
{
    if (!s_ws || s_state != ASR_STREAMING) {
        return false;
    }
    int sent = esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(3000));
    if (sent < 0) {
        ESP_LOGE(TAG, "send failed (json len=%d)", (int)strlen(json));
        return false;
    }
    return true;
}

/* ---------- 解析讯飞返回帧，回调文本 ---------- */
static void handle_result(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "invalid JSON frame");
        return;
    }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && code->valueint != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "xunfei error code=%d msg=%s",
                 code->valueint, msg && msg->valuestring ? msg->valuestring : "?");
        cJSON_Delete(root);
        return;
    }
    cJSON *data_o = cJSON_GetObjectItem(root, "data");
    if (!data_o) {
        cJSON_Delete(root);
        return;
    }
    int status = cJSON_GetObjectItem(data_o, "status") ? cJSON_GetObjectItem(data_o, "status")->valueint : 0;

    /* 拼接文本 */
    cJSON *result = cJSON_GetObjectItem(data_o, "result");
    if (result) {
        cJSON *ws = cJSON_GetObjectItem(result, "ws");
        char *buf = malloc(ASR_BUFFER_SIZE);
        if (!buf) {
            cJSON_Delete(root);
            return;
        }
        buf[0] = '\0';
        size_t o = 0;
        if (ws && cJSON_IsArray(ws)) {
            cJSON *w = NULL;
            cJSON_ArrayForEach(w, ws) {
                cJSON *cw = cJSON_GetObjectItem(w, "cw");
                if (cw && cJSON_IsArray(cw)) {
                    cJSON *item = NULL;
                    cJSON_ArrayForEach(item, cw) {
                        cJSON *txt = cJSON_GetObjectItem(item, "w");
                        if (txt && txt->valuestring) {
                            size_t n = strlen(txt->valuestring);
                            if (o + n + 1 < ASR_BUFFER_SIZE) {
                                memcpy(buf + o, txt->valuestring, n);
                                o += n;
                                buf[o] = '\0';
                            }
                        }
                    }
                }
            }
        }
        bool is_final = (status == 2);
        if (o > 0 && s_cb) {
            s_cb(buf, is_final, s_ctx);
        }
        free(buf);
    }

    /* 结束帧（无 result 且 status==2）：服务端已处理完。
     * 注意：esp_websocket_client_close() 不能从事件处理器调用！
     * 这里只标记状态，由上层（普通任务上下文）调用 asr_deinit() 关闭清理。 */
    if (status == 2 && !result) {
        ESP_LOGI(TAG, "final frame received, ready for deinit");
        s_state = ASR_IDLE;
    }
    cJSON_Delete(root);
}

/* ---------- WebSocket 事件 ---------- */
static void ws_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected, sending first frame");
        if (xSemaphoreTake(s_lock, portMAX_DELAY)) {
            if (!s_sent_first) {
                s_state = ASR_STREAMING;
                s_sent_first = true;
                char frame[512];
                snprintf(frame, sizeof(frame),
                         "{\"common\":{\"app_id\":\"%s\"},"
                         "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\",\"accent\":\"mandarin\",\"vad_eos\":5000},"
                         "\"data\":{\"status\":0,\"format\":\"audio/L16;rate=%d\",\"encoding\":\"raw\",\"audio\":\"\"}}",
                         SECRET_ASR_APPID, ASR_SAMPLE_RATE);
                send_json(frame);
            }
            xSemaphoreGive(s_lock);
        }
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 && d->data_len > 0) { /* text */
            handle_result(d->data_ptr, d->data_len);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "ws event %ld, cleanup", (long)id);
        s_state = ASR_IDLE;
        break;
    default:
        break;
    }
}

/* ---------- 公开 API ---------- */

bool asr_start(void *user_ctx, asr_text_cb_t cb)
{
    if (!cb) {
        return false;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_state != ASR_IDLE || s_ws) {
        ESP_LOGW(TAG, "asr already running");
        return false;
    }
    /* 密钥未配置（占位符） */
    if (strncmp(SECRET_ASR_APPID, "your-asr-appid", 14) == 0 ||
        strncmp(SECRET_ASR_API_KEY, "your-asr-api-key", 16) == 0) {
        ESP_LOGE(TAG, "ASR keys not configured in secrets.h");
        return false;
    }

    if (!build_auth_uri(s_auth_uri, sizeof(s_auth_uri))) {
        return false;
    }

    s_cb = cb;
    s_ctx = user_ctx;
    s_sent_first = false;
    s_state = ASR_CONNECTING;

    esp_websocket_client_config_t cfg = {
        .uri = s_auth_uri,
        .disable_auto_reconnect = true,
        .buffer_size = ASR_BUFFER_SIZE,
        .network_timeout_ms = 15000,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed");
        s_state = ASR_IDLE;
        return false;
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    ESP_LOGI(TAG, "asr started");
    return true;
}

int asr_feed(const uint8_t *pcm, size_t len)
{
    if (!s_ws || s_state != ASR_STREAMING || len == 0 || len > ASR_MAX_PCM_BYTES) {
        return -1;
    }
    /* PCM -> base64（静态缓冲，避免栈溢出——asr_feed 由 4096 栈的录音任务调用） */
    static char b64buf[ASR_MAX_PCM_BYTES * 2];
    int n = b64(pcm, len, b64buf, sizeof(b64buf));
    if (n < 0) {
        return -1;
    }
    /* JSON 帧同样用静态缓冲：ASR_MAX_PCM_BYTES*3+128 ≈ 6KB，绝不能放栈上 */
    static char frame[ASR_MAX_PCM_BYTES * 3 + 128];
    int flen = snprintf(frame, sizeof(frame),
                        "{\"data\":{\"status\":1,\"format\":\"audio/L16;rate=%d\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                        ASR_SAMPLE_RATE, b64buf);
    if (flen <= 0 || (size_t)flen >= sizeof(frame)) {
        return -1;
    }
    return send_json(frame) ? (int)len : -1;
}

bool asr_stop(void)
{
    if (!s_ws || s_state != ASR_STREAMING) {
        return false;
    }
    char frame[256];
    snprintf(frame, sizeof(frame),
             "{\"data\":{\"status\":2,\"format\":\"audio/L16;rate=%d\",\"encoding\":\"raw\",\"audio\":\"\"}}",
             ASR_SAMPLE_RATE);
    send_json(frame);
    /* 让服务端返回最终结果；handle_result 收到结束帧后自动 close */
    ESP_LOGI(TAG, "final frame sent, waiting result...");
    return true;
}

void asr_deinit(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_state = ASR_IDLE;
    s_cb = NULL;
    s_ctx = NULL;
}

bool cloud_is_configured(void)
{
    return strncmp(SECRET_ASR_APPID, "your-asr-appid", 14) != 0 &&
           strncmp(SECRET_DEEPSEEK_API_KEY, "sk-xxxx", 7) != 0;
}
