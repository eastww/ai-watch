/*
 * config.h - 全局配置（WiFi / 云服务 / 密钥）
 *
 * ⚠️ 安全提示：本文件会被 gitignore（main/include/secrets.h 除外）。
 *    将你的 API Key 填入 secrets.h 并保留在本地，切勿提交到仓库。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 密钥（放 secrets.h，不入库） ---------- */
#ifdef CONFIG_AI_WATCH_USE_SECRETS
#include "secrets.h"
#else
#define SECRET_WIFI_SSID      "your_ssid"
#define SECRET_WIFI_PASSWORD  "your_password"
#define SECRET_DEEPSEEK_API_KEY   "sk-xxxx"
#define SECRET_ASR_APPID       "xxxx"
#define SECRET_ASR_API_KEY     "xxxx"
#define SECRET_ASR_API_SECRET  "xxxx"
#define SECRET_TTS_ACCESS_KEY  "xxxx"
#define SECRET_TTS_ACCESS_SECRET "xxxx"
#endif

/* ---------- WiFi ---------- */
#define WIFI_SSID          SECRET_WIFI_SSID
#define WIFI_PASSWORD      SECRET_WIFI_PASSWORD
#define WIFI_MAX_RETRY     10

/* ---------- 云服务端点 ---------- */
/* DeepSeek LLM (OpenAI 兼容) */
#define LLM_URL            "https://api.deepseek.com/chat/completions"
#define LLM_MODEL          "deepseek-chat"
#define LLM_MAX_TOKENS     512
#define LLM_TEMPERATURE    0.8f

/* 系统提示词：定义助理人格 */
#define LLM_SYSTEM_PROMPT  "你是装在微雪 ESP32-S3 圆形屏幕设备上的智能沟通助理，名叫小圆。回答简洁、口语化、不超过200字。"

/* ASR（讯飞流式听写，WebSocket） */
#define ASR_WS_URL         "wss://iat-api.xfyun.cn/v2/iat"
#define ASR_SAMPLE_RATE    16000

/* TTS（以阿里云交互式/或返回16k PCM 的服务为例；PCM 直通播放无需解码） */
#define TTS_SAMPLE_RATE    16000

/* ---------- 应用行为 ---------- */
#define UI_INACTIVITY_MS   30000      /* 无操作 30s 熄屏 */
#define CHAT_HISTORY_MAX   6          /* 保留最近 6 轮多轮记忆 */

/* ---------- 音频（对应板卡引脚，见官方 Wiki） ---------- */
#define AUDIO_SAMPLE_RATE  16000      /* 与 ASR 一致 */
#define AUDIO_MIC_CHANNELS 2          /* 双 MIC 经 ES7210 */

#ifdef __cplusplus
}
#endif
