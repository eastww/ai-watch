/*
 * app_state.h - 应用状态机（M4 里程碑引入完整闭环）
 *
 * 状态：IDLE → WAKE(唤醒) → LISTEN(录音/ASR) → THINK(LLM) → SPEAK(TTS) → IDLE
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STATE_IDLE = 0,   /* 待机：显示时钟，等待唤醒词/触摸 */
    APP_STATE_WAKE,       /* 被唤醒：亮屏 + 提示音 + 聆听动画 */
    APP_STATE_LISTEN,     /* 录音中：采集 MIC → 流式 ASR */
    APP_STATE_THINK,      /* 等待 LLM 回答（思考动画） */
    APP_STATE_SPEAK,      /* 播报中：TTS 音频播放 */
    APP_STATE_MAX
} app_state_t;

/* 状态切换回调（供 UI 更新界面） */
typedef void (*app_state_change_cb_t)(app_state_t new_state);

void app_state_init(app_state_change_cb_t cb);
app_state_t app_state_get(void);
void app_state_set(app_state_t state);

#ifdef __cplusplus
}
#endif
