# AI Watch · 智能沟通助理

基于 **Waveshare ESP32-S3-Touch-LCD-1.85B**（1.85" 圆形屏 / 双 MIC / 扬声器 / IMU / 电池）开发的**可穿戴智能沟通助理**。

- 语音对话（本地唤醒词 → 云端 ASR → **DeepSeek** → 云端 TTS → 播报）
- 360×360 圆形 LVGL 触摸界面
- ESP-IDF v5.5.3 完全自研

## 文档
- [方案设计](docs/SOLUTION.md)

## 快速开始（Windows）

```powershell
# 1. 激活 ESP-IDF 环境（每次新终端）
D:\esp\esp-idf-v5.5.3\export.ps1

# 2. 配置 WiFi / API Key（复制并填写）
Copy-Item main/include/config.h main/include/secrets.h

# 3. 编译
idf.py build

# 4. 烧录 + 监视
idf.py -p COM5 flash monitor
```

> 当前里程碑：**M1**（点亮屏幕 + 触摸）。音频/云服务逐步按 `docs/SOLUTION.md` 里程碑推进。

## 目录结构
```
main/        # 应用代码（UI / 音频 / 云服务）
components/  # 板级 BSP（拷贝自 Waveshare 官方示例）
docs/        # 方案与文档
```
