/*
 * audio.c - 音频模块实现（M2）
 *
 * 硬件链路（板卡）：
 *   播放：ESP32-S3 I2S(TX) -> ES8311 codec -> PA(GPIO9) -> 扬声器
 *   录音：双MIC -> ES7210(回声消除) -> I2S(RX) -> ESP32-S3
 *
 * 注意：ES8311 与 ES7210 共用同一条 I2S 总线（半双工），
 *       播放与录音不能同时进行（依次 open/close）。
 *
 * 兼容性说明：codec 设备 open 时使用的格式必须与 Waveshare 官方 demo
 * 一致（32bit / 双声道 / 16000Hz），否则 ES8311/ES7210 无法正确收发
 * I2S 数据流。本模块内部使用 32bit/双声道 作为 I2S 传输格式，对外
 * API（audio_play_pcm / 录音回调）保持 16bit 单声道 PCM 语义。
 */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

#include "bsp/esp32_s3_touch_lcd_1_85B.h"
#include "audio/audio.h"

static const char *TAG = "audio";

#define AUDIO_SAMPLE_RATE 16000

/* I2S 传输格式：32bit / 双声道（与 Waveshare 官方 demo 一致） */
#define CODEC_BITS        32
#define CODEC_CHANNELS    2
#define CODEC_BYTES       (CODEC_BITS / 8)

static esp_codec_dev_handle_t s_spk_dev = NULL;
static esp_codec_dev_handle_t s_mic_dev = NULL;
static uint8_t s_volume = 80;
static int s_sample_rate = AUDIO_SAMPLE_RATE;

/* ============ 内部工具 ============ */

static esp_codec_dev_handle_t speaker_dev(void)
{
    if (!s_spk_dev) {
        /* BSP 内部会自行 init I2C + I2S（默认配置 22050Hz） */
        s_spk_dev = bsp_audio_codec_speaker_init();
        ESP_LOGI(TAG, "speaker codec init: %s", s_spk_dev ? "OK" : "FAIL");
    }
    return s_spk_dev;
}

static esp_codec_dev_handle_t mic_dev(void)
{
    if (!s_mic_dev) {
        /* BSP 内部会自行 init I2C + I2S（默认配置 22050Hz） */
        s_mic_dev = bsp_audio_codec_microphone_init();
        ESP_LOGI(TAG, "mic codec init: %s", s_mic_dev ? "OK" : "FAIL");
    }
    return s_mic_dev;
}

/* 填充 codec open 所需的 fs：固定 32bit 双声道，采样率动态 */
static esp_codec_dev_sample_info_t codec_fs(int sample_rate)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = CODEC_CHANNELS,
        .bits_per_sample = CODEC_BITS,
    };
    return fs;
}

/* ============ 播放（ES8311） ============ */

bool audio_play_init(int sample_rate)
{
    if (sample_rate > 0) {
        s_sample_rate = sample_rate;
    }
    esp_codec_dev_handle_t dev = speaker_dev();
    if (!dev) {
        return false;
    }
    ESP_LOGI(TAG, "audio init OK (%d Hz, vol=%d)", s_sample_rate, s_volume);
    return true;
}

bool audio_play_pcm(const int16_t *data, size_t samples)
{
    esp_codec_dev_handle_t dev = speaker_dev();
    if (!dev || !data) {
        return false;
    }

    esp_codec_dev_sample_info_t fs = codec_fs(s_sample_rate);
    if (esp_codec_dev_open(dev, &fs) != 0) {
        ESP_LOGE(TAG, "codec open failed");
        return false;
    }

    /* 必须在 open 之后设置音量！codec 未 open 时 set_out_vol 会失败且不保存音量 */
    esp_codec_dev_set_out_vol(dev, s_volume);

    /* 16bit mono -> 32bit stereo：每个样本放到 int32 高 16 位，L/R 相同 */
    size_t frames = samples * CODEC_CHANNELS;
    int32_t *sbuf = malloc(frames * sizeof(int32_t));
    if (!sbuf) {
        ESP_LOGE(TAG, "play malloc failed");
        esp_codec_dev_close(dev);
        return false;
    }
    for (size_t i = 0; i < samples; i++) {
        int32_t v = ((int32_t)data[i]) << 16;
        sbuf[i * CODEC_CHANNELS]     = v;
        sbuf[i * CODEC_CHANNELS + 1] = v;
    }

    size_t bytes = frames * sizeof(int32_t);
    /* 注意：esp_codec_dev_write 返回状态码(0=成功)，不是字节数 */
    int ret = esp_codec_dev_write(dev, sbuf, bytes);
    free(sbuf);

    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec write FAILED ret=%d", ret);
        esp_codec_dev_close(dev);
        return false;
    }

    /* 关键：i2s_channel_write 只是把数据写入 DMA 缓冲即返回，不等播放完成。
     * 若立即 esp_codec_dev_close()，会立刻 mute + 关闭 PA + suspend codec，
     * 音频还没播出来就被掐断（表现为完全无声）。
     * 因此要等待播放时长（+余量）后再 close，让 DMA 排空、扬声器播完。 */
    uint32_t play_ms = (uint32_t)(samples * 1000 / s_sample_rate);
    vTaskDelay(pdMS_TO_TICKS(play_ms + 100));

    esp_codec_dev_close(dev);
    ESP_LOGI(TAG, "playback done: %u samples (%u ms)", (unsigned)samples, (unsigned)play_ms);
    return true;
}

/* 播放一个正弦提示音（用于唤醒/成功/失败音效） */
bool audio_play_tone(int freq_hz, int ms)
{
    if (freq_hz <= 0 || ms <= 0) {
        return false;
    }

    size_t samples = (size_t)(s_sample_rate * ms / 1000);
    int16_t *buf = malloc(samples * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "tone malloc failed");
        return false;
    }

    /* 幅度 40%（避免爆音），正弦波 */
    const float amp = 0.4f * 32767.0f;
    for (size_t i = 0; i < samples; i++) {
        buf[i] = (int16_t)(amp * sinf(2.0f * (float)M_PI * freq_hz * (float)i / s_sample_rate));
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

    esp_codec_dev_sample_info_t fs = codec_fs(s_sample_rate);
    if (esp_codec_dev_open(dev, &fs) != 0) {
        ESP_LOGE(TAG, "mic codec open failed");
        rc->running = false;
        vTaskDelete(NULL);
        return;
    }

    /* 每块 20ms：32bit/双声道交织，则每块帧数 = rate/50 */
    size_t ch_frames = (size_t)(s_sample_rate / 50);
    int32_t *rbuf = malloc(ch_frames * CODEC_CHANNELS * sizeof(int32_t));
    int16_t *mono = malloc(ch_frames * sizeof(int16_t));
    if (!rbuf || !mono) {
        ESP_LOGE(TAG, "record malloc failed");
        free(rbuf);
        free(mono);
        esp_codec_dev_close(dev);
        rc->running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "recording started...");
    int err_cnt = 0;

    while (rc->running) {
        /* 注意：esp_codec_dev_read 成功时返回 0 (ESP_CODEC_DEV_OK)，不是字节数！
         * 数据直接填入 rbuf（请求多少读多少，I2S 阻塞读）。 */
        int ret = esp_codec_dev_read(dev, rbuf, ch_frames * CODEC_CHANNELS * sizeof(int32_t));
        if (ret == ESP_CODEC_DEV_OK) {
            size_t got = ch_frames;
            for (size_t i = 0; i < got; i++) {
                /* 左声道 24bit 右对齐 -> int16 单声道
                 * ES7210 是 24bit ADC，数据右对齐在 32bit 槽的低 24 位，
                 * 用 >>8 取有效 16bit。之前误用 >>16 取高字节（几乎为0），
                 * 导致音量被砍掉约 250 倍（表现为"几乎无声"）。 */
                mono[i] = (int16_t)(rbuf[i * CODEC_CHANNELS] >> 8);
            }
            if (!rc->cb(mono, got, rc->ctx)) {
                ESP_LOGI(TAG, "record callback requested stop");
                break;
            }
        } else {
            if (++err_cnt <= 5) {
                ESP_LOGW(TAG, "codec read err=%d", ret);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    free(rbuf);
    free(mono);
    esp_codec_dev_close(dev);
    rc->running = false;
    ESP_LOGI(TAG, "recording stopped");
    vTaskDelete(NULL);
}

bool audio_record_start(int sample_rate, audio_record_cb_t cb, void *ctx)
{
    if (sample_rate > 0) {
        s_sample_rate = sample_rate;
    }
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