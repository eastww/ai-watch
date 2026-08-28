/*
 * cloud.h - 云服务统一入口（ASR / LLM / TTS）
 *
 * 模块化设计，便于替换服务商：
 *   asr_client.c  - 讯飞/阿里流式听写
 *   llm_client.c  - DeepSeek chat/completions
 *   tts_client.c  - 阿里云/讯飞语音合成（返回 16k PCM 直通播放）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 语音识别（云端 ASR） ---------- */
typedef void (*asr_text_cb_t)(const char *text, bool is_final, void *ctx);

/* 开启一轮流式识别：传入 16k/16bit PCM，结束调用 asr_stop */
bool asr_start(void *user_ctx, asr_text_cb_t cb);
int  asr_feed(const uint8_t *pcm, size_t len);   /* 喂 PCM 数据 */
bool asr_stop(void);                             /* 结束并等待最终结果 */

/* ---------- 大模型（LLM，见 llm_client.h） ---------- */

/* ---------- 语音合成（云端 TTS） ---------- */
typedef void (*tts_pcm_cb_t)(const uint8_t *pcm, size_t len, void *ctx);

/* 合成文本为音频，逐段回调 PCM（16k/16bit mono） */
bool tts_speak(const char *text, tts_pcm_cb_t cb, void *ctx, int timeout_ms);

/* 供 UI 查询云服务就绪状态 */
bool cloud_is_configured(void);

#ifdef __cplusplus
}
#endif
