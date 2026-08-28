/*
 * llm_client.h - DeepSeek LLM 客户端（OpenAI 兼容 chat/completions）
 *
 * 支持流式(SSE)与非流式；多轮上下文由调用方维护。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 每收到一段流式增量文本时回调（在 esp_http_client 事件上下文，勿做重活） */
typedef void (*llm_stream_cb_t)(const char *delta, void *user_ctx);
/* 一次回答结束回调：full_text 为完整回答 */
typedef void (*llm_done_cb_t)(const char *full_text, bool ok, void *user_ctx);

typedef struct {
    const char *api_key;        /* DeepSeek API Key */
    const char *model;          /* "deepseek-chat" / "deepseek-reasoner" */
    const char *system_prompt;  /* 系统提示词 */
    float       temperature;
    int         max_tokens;
    int         timeout_ms;     /* 单次请求超时 */
} llm_client_cfg_t;

typedef struct llm_client llm_client_t;

llm_client_t *llm_client_create(const llm_client_cfg_t *cfg);
void          llm_client_destroy(llm_client_t *c);

/*
 * 发送对话请求（阻塞直到完成或超时）。
 * messages: 形如 "[\n{\"role\":\"system\",\"content\":\"...\"},\n{\"role\":\"user\",\"content\":\"...\"}\n]"
 *           或由辅助函数 llm_build_messages 生成。
 * stream=true 时走 SSE，逐段回调；false 时一次性返回。
 */
bool llm_client_chat(llm_client_t *c,
                     const char *messages_json,
                     bool stream,
                     llm_stream_cb_t stream_cb,
                     llm_done_cb_t done_cb,
                     void *user_ctx);

/* 辅助：把 role/content 数组拼成 messages JSON（返回 malloc 字符串，用 free 释放） */
char *llm_build_messages(const char *const *roles,
                         const char *const *contents,
                         int count);

#ifdef __cplusplus
}
#endif
