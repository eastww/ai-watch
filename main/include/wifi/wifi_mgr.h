/*
 * wifi_mgr.h - WiFi 连接管理（STA 模式）
 *
 * 提供：异步连接（事件驱动）、断线自动重连、状态查询与状态回调。
 * 供 M3 云服务（ASR/TTS/LLM）在 WiFi 就绪后使用。
 *
 * 用法：
 *   wifi_mgr_init();            // 注册事件处理器 + 启动连接
 *   // 等待 wifi_mgr_is_connected() 或状态回调
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 连接状态 */
typedef enum {
    WIFI_MGR_STATE_DOWN = 0,     /* 未初始化/断开 */
    WIFI_MGR_STATE_CONNECTING,   /* 连接中 */
    WIFI_MGR_STATE_CONNECTED,    /* 已连接（有 IP） */
    WIFI_MGR_STATE_DISCONNECTED, /* 曾连接后断开（等待重连） */
} wifi_mgr_state_t;

/* 状态变化回调（在事件任务上下文调用，勿做耗时操作） */
typedef void (*wifi_mgr_state_cb_t)(wifi_mgr_state_t state, void *ctx);

/* 初始化：注册事件处理器并按 config.h 中的 SSID/密码发起连接。
 * 返回 false 表示参数缺失（SSID 未配置）。 */
bool wifi_mgr_init(void);

/* 注册状态回调（可多次调用，多个回调都会触发） */
void wifi_mgr_register_cb(wifi_mgr_state_cb_t cb, void *ctx);

/* 主动断开（例如进入省电模式） */
void wifi_mgr_disconnect(void);

/* 重新连接（断线后也可手动触发） */
void wifi_mgr_reconnect(void);

/* 查询当前状态 */
wifi_mgr_state_t wifi_mgr_get_state(void);

/* 是否已连接并有 IP */
bool wifi_mgr_is_connected(void);

/* 获取本机 IP（未连接返回 0.0.0.0） */
char *wifi_mgr_get_ip_str(void);

#ifdef __cplusplus
}
#endif
