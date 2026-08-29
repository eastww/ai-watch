/*
 * audio.c - 音频模块实现（M2）
 *
 * 硬件链路（板卡）：
 *   播放：ESP32-S3 I2S(TX) -> ES8311 codec -> PA(GPIO9) -> 扬声器
 *   录音：双MIC -> ES7210(回声消除) -> I2S(RX) -> ESP32-S3
 *
 * 注意：ES8311 与 ES7210 共用同一条 I2S 总线（半双工），
 *       播放与录音不能同时进行（依次 open/close）。
 */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

#include "bsp/esp32_s3_touch_lcd_1_85B.h"
#include "audio/audio.h"

static const char *TAG = "audio";

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS        16
#define AUDIO_CHANNELS    1

static esp_codec_dev_handle_t s_spk_dev = NULL;
static esp_codec_dev_handle_t s_mic_dev = NULL;
static uint8_t s_volume = 80;

/* ============ 内部工具 ============ */

static esp_codec_dev_handle_t speaker_dev(void)
{
    if (!s_spk_dev) {
        s_spk_dev = bsp_audio_codec_speaker_init();
        ESP_LOGI(TAG, "speaker codec init: %s", s_spk_dev ? "OK" : "FAIL");
    }
    return s_spk_dev;
}

static esp_codec_dev_handle_t mic_dev(void)
{
    if (!s_mic_dev) {
        s_mic_dev = bsp_audio_codec_microphone_init();
        ESP_LOGI(TAG, "mic codec init: %s", s_mic_dev ? "OK" : "FAIL");
    }
    return s_mic_dev;
}

/* ============ 播放（ES8311） ============ */

bool audio_play_init(int sample_rate)
{
    /* 配置 I2S（16kHz / 16bit / 单声道 / 全双工）。两个 codec 共享此总线。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate > 0 ? sample_rate : AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_2,
            .bclk = GPIO_NUM_48,
            .ws   = GPIO_NUM_38,
            .dout = GPIO_NUM_47,
            .din  = GPIO_NUM_39,
        },
    };

    esp_err_t ret = bsp_audio_init(&std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_codec_dev_handle_t dev = speaker_dev();
    if (!dev) {
        return false;
    }

    esp_codec_dev_set_out_vol(dev, s_volume);
    ESP_LOGI(TAG, "audio init OK (%d Hz)", sample_rate > 0 ? sample_rate : AUDIO_SAMPLE_RATE);
    return true;
}

bool audio_play_pcm(const int16_t *data, size_t samples)
{
    esp_codec_dev_handle_t dev = speaker_dev();
    if (!dev) {
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
    };

    if (esp_codec_dev_open(dev, &fs) != 0) {
        ESP_LOGE(TAG, "codec open failed");
        return false;
    }

    /* I2S 支持按字节数写入，samples 是样本数(16bit => 2字节/样本) */
    size_t bytes = samples * sizeof(int16_t);
    /* esp_codec_dev_write 需要 void*，去掉 const（内部不修改数据） */
    int ret = esp_codec_dev_write(dev, (void *)data, bytes);
    esp_codec_dev_close(dev);

    return ret == (int)bytes;
}

/* 播放一个正弦提示音（用于唤醒/成功/失败音效） */
bool audio_play_tone(int freq_hz, int ms)
{
    if (freq_hz <= 0 || ms <= 0) {
        return false;
    }

    size_t samples = (size_t)(AUDIO_SAMPLE_RATE * ms / 1000);
    int16_t *buf = malloc(samples * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "tone malloc failed");
        return false;
    }

    /* 幅度 40%（避免爆音），正弦波 */
    const float amp = 0.4f * 32767.0f;
    for (size_t i = 0; i < samples; i++) {
        buf[i] = (int16_t)(amp * sinf(2.0f * (float)M_PI * freq_hz * (float)i / AUDIO_SAMPLE_RATE));
    }

    bool ok = audio_play_pcm(buf, samples);
    free(buf);
    return ok;
}

void audio_play_stop(void)
{
    if (s_spk_dev) {
        esp_codec_dev_close(s_spk_dev);
    }
}

void audio_set_volume(uint8_t vol)
{
    s_volume = vol > 100 ? 100 : vol;
    if (s_spk_dev) {
        esp_codec_dev_set_out_vol(s_spk_dev, s_volume);
    }
}

uint8_t audio_get_volume(void)
{
    return s_volume;
}

/* ============ 录音（ES7210 + 双 MIC） ============ */

/* 录音任务上下文 */
typedef struct {
    audio_record_cb_t cb;
    void *ctx;
    bool running;
} record_ctx_t;

static record_ctx_t s_rec;

static void record_task(void *arg)
{
    record_ctx_t *rc = (record_ctx_t *)arg;
    esp_codec_dev_handle_t dev = mic_dev();
    if (!dev) {
        rc->running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
    };

    if (esp_codec_dev_open(dev, &fs) != 0) {
        ESP_LOGE(TAG, "mic codec open failed");
        rc->running = false;
        vTaskDelete(NULL);
        return;
    }

    /* 每块 20ms */
    int16_t buf[320];
    ESP_LOGI(TAG, "recording started...");

    while (rc->running) {
        int n = esp_codec_dev_read(dev, buf, sizeof(buf));
        if (n > 0) {
            size_t got = n / sizeof(int16_t);
            if (!rc->cb(buf, got, rc->ctx)) {
                ESP_LOGI(TAG, "record callback requested stop");
                break;
            }
        }
    }

    esp_codec_dev_close(dev);
    rc->running = false;
    ESP_LOGI(TAG, "recording stopped");
    vTaskDelete(NULL);
}

bool audio_record_start(int sample_rate, audio_record_cb_t cb, void *ctx)
{
    (void)sample_rate;
    if (!cb || s_rec.running) {
        return false;
    }

    s_rec.cb = cb;
    s_rec.ctx = ctx;
    s_rec.running = true;

    BaseType_t ret = xTaskCreate(record_task, "audio_rec", 4096, &s_rec, 5, NULL);
    if (ret != pdPASS) {
        s_rec.running = false;
        return false;
    }
    return true;
}

bool audio_record_stop(void)
{
    /* 由回调返回 false 或此处置位停止 */
    s_rec.running = false;
    return true;
}