/*
 * audio.h - 音频统一接口（板卡：ES7210 录音 + ES8311 播放）
 *
 * 里程碑：
 *  M2 播放/录音打通（提示音、录音回放）
 *  M3 对接 ASR/TTS 流式数据
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 播放（ES8311） ---------- */
/* 初始化播放链路，sample_rate 如 16000 */
bool audio_play_init(int sample_rate);
/* 播放一段 PCM（16bit 单声道）。阻塞式简单实现；流式版用于 TTS。 */
bool audio_play_pcm(const int16_t *data, size_t samples);
/* 播放一次提示音（用于唤醒/成功/失败音效，M2 实现） */
bool audio_play_tone(int freq_hz, int ms);
void audio_play_stop(void);
void audio_set_volume(uint8_t vol);  /* 0-100 */
uint8_t audio_get_volume(void);

/* ---------- 录音（ES7210 + 双 MIC） ---------- */
/* PCM 数据回调（16k/16bit），返回 true 表示继续，false 停止 */
typedef bool (*audio_record_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

bool audio_record_start(int sample_rate, audio_record_cb_t cb, void *ctx);
bool audio_record_stop(void);

#ifdef __cplusplus
}
#endif
