# 智能沟通助理 · 方案设计

> 硬件平台：Waveshare **ESP32-S3-Touch-LCD-1.85B**
> 开发框架：**ESP-IDF v5.5.3**（完全自研）
> AI 服务：**DeepSeek（LLM）+ 独立云端 ASR/TTS**
> 文档版本：v0.1（2026-08-28）

---

## 1. 项目概述

基于微雪 ESP32-S3-Touch-LCD-1.85B 开发板，打造一个**可穿戴的智能沟通助理**：一个 1.85 英寸圆形屏幕的"AI 徽章/随身助手"。用户通过**语音**或**触摸**与它交互，它通过 **DeepSeek 大模型**理解并回答问题、提供信息，并用**语音**回复，同时在圆形屏幕上显示状态与内容。

### 1.1 核心价值
- **随身 AI 助手**：口袋/挂坠形态，抬手即用
- **多模态交互**：语音对话 + 触摸屏幕 + 姿态感应（IMU 抬手亮屏）
- **开源可扩展**：ESP-IDF 自研，可自由扩展功能（提醒、翻译、信息播报、连接 IOT 等）

### 1.2 目标场景（V1）
| 场景 | 说明 |
|------|------|
| AI 对话 | 唤醒后说问题 → DeepSeek 回答 → 语音播报 + 屏幕显示 |
| 时间显示 | 常态显示时间/日期/电池，像一块智能手表屏 |
| 触摸交互 | 点击屏幕查看对话记录、调节音量、查看设置 |
| 姿态交互 | 抬手亮屏 / 放下熄屏（省电） |

---

## 2. 硬件资源盘点

来自官方 Wiki 的板卡外设清单（均经官方 demo 验证可用）：

| 外设 | 芯片/接口 | 引脚/地址 | 用途 |
|------|-----------|-----------|------|
| 主控 | ESP32-S3R8 双核 LX7 @240MHz | - | 8MB PSRAM + 16MB Flash |
| 显示 | ST77916 (QSPI) 360×360 | CS=21 PCLK=40 D0-3=46/45/42/41 RST=3 BL=5 | 圆形屏 LVGL UI |
| 触摸 | CST816S (I2C) | SCL=10 SDA=11 RST=1 INT=4, addr 0x15 | 触摸输入 |
| 音频输出 | ES8311 (I2S+I2C) | MCLK=2 BCLK=48 LRCK=38 DOUT=47 DIN=39, addr 0x30, PA=9 | 播报/提示音 |
| 音频输入 | ES7210 回声消除 + 双 MIC (I2S+I2C) | 同 I2S，addr 0x80 | 语音采集 |
| IMU | QMI8658 六轴 (I2C) | addr 0x6B | 抬手亮屏/姿态 |
| RTC | PCF85063 (I2C) | addr 0x51, INT=6 | 时间保持 |
| 电池计量 | BQ27220 (I2C) | addr 0x55 | 电量显示 |
| TF 卡 | SDMMC 4-bit | CLK=15 CMD=14 D0-3=16/17/12/13 | 扩展存储 |
| 按键 | BOOT(GPIO0)/PWR | - | 唤醒/重启/自定义 |
| 电源 | Type-C / 3.7V 锂电池 MX1.25 | - | 供电 |

> ⚠️ 注意：I2C 总线是共享的（SCL=GPIO10, SDA=GPIO11），所有外设地址需避开冲突。
> ⚠️ 扬声器为 pads 形式，需焊接；本方案假设喇叭与电池已就位。

---

## 3. 总体架构

```
┌──────────────────────── ESP32-S3-Touch-LCD-1.85B ────────────────────────┐
│                                                                          │
│  ┌──────────────┐  ┌─────────────────┐  ┌───────────────────────────┐   │
│  │   UI 层       │  │   音频管线       │  │   云服务客户端              │   │
│  │  LVGL v9      │  │  录音: ES7210   │  │  ASR 客户端 (WebSocket)    │   │
│  │  · 主界面时钟  │  │  播放: ES8311   │  │  LLM 客户端 (HTTPS/SSE)   │   │
│  │  · 对话界面    │  │  唤醒词: ESP-SR │  │  TTS 客户端 (HTTPS)        │   │
│  │  · 设置/音量   │  │  · 回声消除     │  │                           │   │
│  └──────┬───────┘  └────────┬────────┘  └─────────────┬─────────────┘   │
│         │                   │                        │                 │
│  ┌──────┴───────────────────┴────────────────────────┴───────────────┐ │
│  │                    应用状态机 app_state.c                          │ │
│  │   Idle → 唤醒/点击 → 录音(ASR) → 思考(LLM) → 播报(TTS) → Idle      │ │
│  └──────────────────────────────┬────────────────────────────────────┘ │
│                                 │                                      │
│  ┌──────────────────────────────┴────────────────────────────────────┐ │
│  │   BSP：waveshare__esp32_s3_touch_lcd_1_85B（官方 BSP 组件）        │ │
│  │   显示/触摸/音频 codec 初始化 + I2C 总线 + 电源管理                  │ │
│  └───────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │ WiFi (2.4GHz) · HTTPS / WSS
                    ┌──────────────┴───────────────┬───────────────┐
                    │ ASR 语音识别                    │ DeepSeek LLM  │ TTS 语音合成
                    │ 讯飞/阿里/百度 (流式)            │ chat 接口      │ 阿里/讯飞/edge
                    └────────────────────────────────┴───────────────┘
```

### 3.1 数据流（一次完整对话）
```
用户说话
  → ESP-SR 唤醒词命中 或 触摸触发
  → ES7210 + 双MIC 采集 16kHz/16bit PCM
  → WebSocket 流式上传 → 云端 ASR → 返回文本
  → DeepSeek chat API（多轮记忆 + 系统提示词）→ 返回回答文本
  → 云端 TTS → 返回音频（PCM/MP3）
  → 解码后经 ES8311 → 扬声器播放
  → 同时文本滚动显示在 360×360 圆屏上
```

---

## 4. 功能设计

### 4.1 唤醒词（本地，低功耗）
- 使用乐鑫 **ESP-SR**（`esp-sr` 托管组件），唤醒词可选：
  - `Hi, 乐鑫`（默认，含在 esp-sr 中，无需训练）
  - 中文："你好小圆" 等需自行训练（可选，V2 再做）
- 唤醒后亮屏 + 提示音，进入对话态

**实现原理（纯软件，无专用硬件加速）**：
- ESP32-S3 无语音协处理器，唤醒词识别 = **CPU 实时运行轻量神经网络 WakeNet**（空洞卷积，WakeNet9/9l 支持 S3）
- 链路：16kHz/16bit PCM → **AFE 音频前端**（AEC 回声消除 / NS 降噪 / VAD 人声检测 / DOA 方位 / Beamforming 波束成形）→ MFCC 特征 → WakeNet 推理 → 多帧平滑 → 触发
- ESP32-S3 的 8MB PSRAM + 240MHz 双核足以实时跑通全链路（官方 demo 04_esp_wakeword_det 已验证）
- 省电手段：light sleep + 周期性唤醒采样检测（M6 优化）
- 自定义唤醒词需走乐鑫训练定制流程

### 4.2 语音识别 ASR（云端）
| 方案 | 接入方式 | 优点 | 备注 |
|------|---------|------|------|
| **A. 讯飞开放平台**（推荐起步） | 流式 WebSocket，PCM 直传 | 中文强、免费额度 | 需要 APPID/APIKey/APISecret |
| B. 阿里云智能语音 | WebSocket 流式，Paraformer | 与 TTS 同生态 | 需 AccessKey |
| C. 百度短语音 | REST，音频 base64 上传 | 简单 | 单次 60s 内 |

> 起步建议 **讯飞**，因其 WebSocket 协议文档清晰、免费额度够用、PCM 直传无需压缩。

### 4.3 大模型 LLM：DeepSeek
- 接口：`POST https://api.deepseek.com/chat/completions`（OpenAI 兼容格式）
- 模型：`deepseek-chat`（对话）/ `deepseek-reasoner`（推理，可选）
- 特性：
  - 支持**流式（SSE）**输出，可实现"边说边显"的流式体验
  - 多轮上下文：本地保存最近 N 轮，随请求携带
  - 系统提示词设定助理人格
- 免费 API Key 申请：https://platform.deepseek.com

### 4.4 语音合成 TTS
| 方案 | 说明 | 输出格式 |
|------|------|---------|
| **A. 阿里云 CosyVoice / 交互式语音** | 中文音色好，流式 | PCM/WAV 可直接播 |
| B. 讯飞在线合成 | 与 ASR 同账号 | MP3（需解码） |
| C. edge-tts（微软） | 免费，无需 key，但需代理/接口不稳定 | MP3 |

> 起步建议 **阿里云**（或 TTS 返回 16k PCM 的服务），**PCM 直通 ES8311 播放，省去解码器**，大幅简化音频链路。

### 4.5 UI 设计（LVGL v9，360×360 圆形）
| 界面 | 内容 |
|------|------|
| 主界面（Idle） | 圆形表盘时钟 + 日期 + 电量 + 音量 + 状态灯 |
| 对话界面 | 上方显示用户问句，下方显示 AI 回复（自动换行滚动） |
| 唤醒态 | 环形音波动画（示意"聆听中"） |
| 思考态 | 转圈动画 + "思考中…" |
| 播报态 | 声波纹动画 |
| 设置 | 音量滑条、亮度、WiFi 状态、固件信息 |

### 4.6 省电策略
- 无交互 30s → 熄屏（关背光），ESP-SR 低功耗唤醒监听
- 电池供电时降低主频/关闭外设（V2）
- 利用 QMI8658 抬手亮屏、翻面静音

---

## 5. 软件结构（ESP-IDF 工程）

```
ai-watch/
├── CMakeLists.txt                  # IDF 工程入口
├── sdkconfig.defaults              # 默认配置（PSRAM、SPI 频率等）
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml           # 托管组件依赖声明
│   ├── app_main.c                  # 入口：初始化 + 启动
│   ├── include/
│   │   ├── app_state.h             # 应用状态机
│   │   ├── ui/ui.h                 # LVGL 界面模块
│   │   ├── audio/audio.h           # 录音/播放封装
│   │   ├── cloud/cloud.h           # 云服务统一入口
│   │   └── config.h                # WiFi/API Key 等配置
│   └── src/
│       ├── app_state.c             # 状态机实现
│       ├── ui/ui_main.c            # 主界面
│       ├── ui/ui_chat.c            # 对话界面
│       ├── ui/ui_anim.c            # 音波动画
│       ├── audio/audio_record.c    # ES7210 录音
│       ├── audio/audio_play.c      # ES8311 播放
│       ├── cloud/asr_client.c      # 讯飞/阿里 ASR 客户端
│       ├── cloud/llm_client.c      # DeepSeek 客户端
│       ├── cloud/tts_client.c      # TTS 客户端
│       └── wifi/wifi_mgr.c         # WiFi 连接管理
└── components/
    └── waveshare__esp32_s3_touch_lcd_1_85B/   # 官方 BSP（从官方 demo 拷贝）
```

### 5.1 托管组件依赖（`main/idf_component.yml`）
| 组件 | 用途 |
|------|------|
| `espressif/esp_codec_dev` | ES8311/ES7210 codec 驱动 |
| `espressif/esp_lv_adapter` | LVGL 与 ESP-IDF 显示/输入适配 |
| `espressif/lvgl` | 图形库 v9 |
| `espressif/esp-sr` | 唤醒词识别（ESP-SR） |
| `espressif/esp_audio_simple_player` | 播放（可选，也可直接写 codec） |
| `espressif/gmf_core/gmf_audio/gmf_io` | 音频管线（官方 demo 用，可裁剪） |
| `espressif/iot_button` | 按键（BOOT 自定义） |

> 官方 demo 的 BSP 已声明了大部分依赖，我们以其为基准。

---

## 6. 环境搭建（Windows + ESP-IDF v5.5.3）

| 步骤 | 内容 | 状态 |
|------|------|------|
| 1 | 安装 Git（已有 2.55） | ✅ |
| 2 | 克隆 ESP-IDF v5.5.3 至 `D:\esp\esp-idf-v5.5.3` | ⏳ 进行中 |
| 3 | 运行 `install.ps1` 安装工具链 + Python 虚拟环境 | ⏳ |
| 4 | `export.ps1` 激活环境，`idf.py --version` 验证 | ⏳ |
| 5 | 配置 VS Code ESP-IDF 扩展（指向 D:\esp） | ⏳ |
| 6 | 导入官方 BSP 组件 | ⏳ |
| 7 | 首次编译 `hello_world` 验证工具链 | ⏳ |

### 6.1 烧录与日志
```powershell
# 进入 IDF 环境（每次新终端需执行）
D:\esp\esp-idf-v5.5.3\export.ps1

# 编译
idf.py build

# 烧录（USB 连接，按住 BOOT 可进下载模式）
idf.py -p COM5 flash monitor
```

---

## 7. 里程碑规划

| 阶段 | 内容 | 验收标准 |
|------|------|---------|
| **M1 环境** | IDF 搭建 + 点亮屏幕 | 圆形屏显示 LVGL 时钟，触摸可用 |
| **M2 音频** | ES8311 播放 + ES7210 录音 | 播放提示音；录音可回放/看到波形 |
| **M3 语音链路** | WiFi + ASR + TTS | 说话→文字→语音回放（本地闭环） |
| **M4 智能对话** | 接入 DeepSeek | 完整"说话→回答→播报+显示"闭环 |
| **M5 唤醒词** | ESP-SR 唤醒 | 喊唤醒词触发对话 |
| **M6 打磨** | UI 美化、省电、抬手亮屏、错误处理 | 全天候稳定运行 |

---

## 8. 风险与注意事项

1. **Python 3.14 兼容性**：系统为 Python 3.14，ESP-IDF v5.5.3 官方要求 3.8+；若依赖安装失败，需另装 Python 3.11/3.12。
2. **网络**：esp-idf 与工具链下载自 dl.espressif.com，国内网络可能较慢；必要时配置代理或镜像。
3. **I2C 共享总线**：所有外设共用 SCL=10/SDA=11，避免地址冲突（0x15/0x6B/0x30/0x80/0x51/0x55 已占用）。
4. **音频时钟**：ES8311 与 ES7210 共用 MCLK=2/BCLK=48/LRCK=38，播放与录音需互斥管理（半双工）。
5. **扬声器焊接**：pads 需自行焊接喇叭，注意阻抗（参考官方 8Ω/1W）。
6. **API Key 安全**：存于 `main/include/config.h`（或 sdkconfig/Kconfig），不入库；生产可用 NVS 加密存储。
7. **PSRAM 依赖**：LVGL 帧缓冲 + 网络缓冲放 PSRAM，务必启用 PSRAM 配置。

---

## 9. 参考资源

- 板卡 Wiki：https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B
- 官方示例：https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B （ESP-IDF-V5.5.3 目录）
- 小智AI（可作参考实现）：https://github.com/78/xiaozhi-esp32 （官方支持本板卡）
- DeepSeek API 文档：https://api-docs.deepseek.com
- 乐鑫组件注册表：https://components.espressif.com
