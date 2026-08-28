# AI Watch 项目构建指南

## 📋 项目概述
AI Watch 是基于 ESP32-S3-Touch-LCD-1.85B 开发的智能沟通助理项目。当前实现了 M1 里程碑：屏幕显示 + 触摸功能（Touch to Wake）。

## 🛠️ 构建要求

### 硬件
- Waveshare ESP32-S3-Touch-LCD-1.85B 开发板
- Micro USB 数据线
- 3.7V 锂电池（可选）

### 软件
- Windows 10/11
- Python 3.11 或 3.12（推荐 3.11，避免 Python 3.14 兼容性问题）
- ESP-IDF v5.5.3

## 🔧 环境设置

### 1. 安装 Python 3.11（如果需要）
```powershell
# 从 https://www.python.org/downloads/ 下载 Python 3.11
# 安装时勾选 "Add Python to PATH"
```

### 2. 设置 ESP-IDF 环境
```powershell
# 1. 打开 PowerShell（管理员模式）
# 2. 进入 ESP-IDF 目录
cd D:\esp\esp-idf-v5.5.3

# 3. 运行安装脚本
.\install.ps1

# 4. 激活环境
. export.ps1
```

### 3. 验证环境
```powershell
idf.py --version
# 应该显示类似：idf.py v5.5.3
```

## 📝 配置文件设置

### 1. 复制 secrets 模板
```powershell
Copy-Item main\include\config.h main\include\secrets.h
```

### 2. 编辑 secrets.h
打开 `main/include/secrets.h`，填入你的实际配置：
```c
#define SECRET_WIFI_SSID      "你的WiFi名称"
#define SECRET_WIFI_PASSWORD  "你的WiFi密码"
#define SECRET_DEEPSEEK_API_KEY   "你的DeepSeek API密钥"
```

> **注意**： secrets.h 不会被提交到 git，请妥善保管你的密钥。

## 🔨 构建步骤

### 1. 编译项目
```powershell
# 进入项目目录
cd D:\Projects\ai-watch

# 确保环境已激活
D:\esp\esp-idf-v5.5.3\export.ps1

# 编译
idf.py build
```

### 2. 烧录到设备
```powershell
# 确保设备通过 USB 连接
# 按住 BOOT 按钮，然后按 RST 按钮，松开 BOOT
idf.py -p COM5 flash
```

### 3. 监视串口输出
```powershell
idf.py -p COM5 monitor
```

## 🎯 功能测试

### Touch to Wake 功能测试
1. 烧录成功后，设备启动显示：
   ```
   AI WATCH
   00:00:00
   Touch to wake
   M1: display + touch OK
   ```

2. 触摸屏幕任意位置，应该看到：
   ```
   AI WATCH
   00:00:00
   Device awake!
   Touch to start...
   ```

3. 5秒无操作，自动恢复到初始状态

### 预期日志输出
```
I (1234) main_task: Started on core 0
I (1234) esp_lcd: panel internal resolution: 360 x 360
I (1234) ai_watch: ai-watch running (M1): LVGL driven by esp_lv_adapter worker task
I (2345) touch_input_cb: Touch detected at x=180, y=180
I (2345) ai_watch: Touch to wake - activating device
I (2345) app_state: State changed to: WAKE
I (3345) app_state: Returning to IDLE state
```

## 🔍 故障排除

### 1. 编译错误
- **错误**: `MSys/Mingw is not supported`
  **解决**: 确保 Python 3.11，卸载 LLVM MinGW
  
- **错误**: `fatal error: lvgl.h: No such file`
  **解决**: 确保运行了 `export.ps1`

- **错误**: `undefined reference to app_state`
  **解决**: 检查 CMakeLists.txt 是否包含 src/app_state.c

### 2. 烧录问题
- **错误**: `Failed to connect to ESP32-S3`
  **解决**: 
  - 检查 USB 驱动
  - 尝试不同 COM 端口
  - 按住 BOOT + RST 重启设备

### 3. 触摸无响应
- **检查**: 确保屏幕触摸区域没有被遮挡
- **检查**: 观察日志是否有触摸事件

## 📊 项目结构
```
ai-watch/
├── main/
│   ├── app_main.c          # 主程序入口
│   ├── src/
│   │   └── app_state.c     # 状态机实现
│   └── include/
│       ├── app_state.h     # 状态机定义
│       ├── config.h        # 配置模板
│       └── secrets.h       # 密钥配置（请编辑）
├── components/
│   └── waveshare__esp32_s3_touch_lcd_1_85B/  # BSP组件
└── BUILD_INSTRUCTIONS.md   # 本文件
```

## 🚀 下一步开发

完成当前 M1 里程碑后，可以继续开发：
- M2: 音频功能（ES8311 播放 + ES7210 录音）
- M3: 语音链路（WiFi + ASR + TTS）
- M4: 智能对话（接入 DeepSeek）
- M5: 唤醒词（ESP-SR）
- M6: 省电优化和功能打磨

## 📞 支持
如果遇到问题，请检查：
1. ESP-IDF 环境是否正确设置
2. COM 端口是否正确
3. 设备是否正确连接
4. 代码是否与 README 中的版本一致