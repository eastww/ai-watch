# 开发记录 (DevLog)

本文件按时间顺序记录本项目的开发过程、关键决策、遇到的问题与解决方案。
按日期倒序排列，最新记录在最上方。

---

## 2026-08-28

### ✅ M1 里程碑：环境搭建 + 首次编译通过

**完成内容**
- ESP-IDF v5.5.3 完整搭建（Windows 10/11，Python 3.14 虚拟环境）
- 工具链安装完成（xtensa-esp-elf 14.2 / cmake 3.30 / ninja 等 11 个工具）
- 官方 BSP 导入：`waveshare__esp32_s3_touch_lcd_1_85B` + `qmi8658` + `pcf85063a`
- 托管依赖 22 个自动解析（LVGL 9.5 / esp_codec_dev / ST77916 / CST816S 等）
- `idf.py set-target esp32s3` + `idf.py build` 编译通过
  - 固件 `ai_watch.bin` 约 640KB（0x9cf30 bytes），应用分区剩余 96%
- 代码推送 GitHub：https://github.com/eastww/ai-watch（SSH）

### ⚠️ 待修复：白屏问题（LVGL 双重驱动导致崩溃）

**现象**：烧录后屏幕白屏、反复重启（`task_wdt` 看门狗超时）。

**根因分析**（已通过源码确认）：
- BSP 的 `bsp_display_start()` 内部调用 `esp_lv_adapter_start()`，
  它会在内部创建 `lvgl_worker` 任务，循环持有 `esp_lv_adapter_lock()`
  并调用 `lv_timer_handler()` 处理 LVGL 事件（esp_lv_adapter.c:925-967）。
- 而 `app_main()` 里又写了一个死循环手调 `lv_timer_handler()`，且**未持锁**。
- 结果：LVGL 被两个任务并发操作 → 数据竞争 → 主任务卡死 → 看门狗复位。
- 崩溃回溯关键帧：
  ```
  task_wdt_timeout_handling ← lv_inv_area ← lv_obj_set_style_bg_color ← ui_main_screen_create ← app_main
  ```

**修复方案**：删除 `app_main` 里的手动 `lv_timer_handler()` 循环，UI 操作包在
`bsp_display_lock()/bsp_display_unlock()` 内（由 BSP worker 驱动刷新）。

**经验教训**：
1. **不要手动调 lv_timer_handler()** —— 用了 esp_lv_adapter / BSP 的显示驱动后，
   刷新由它的 worker 任务负责，业务代码只需持锁操作 LVGL API。
2. 所有 LVGL 调用都应包在 `bsp_display_lock()` 内（LVGL 非线程安全）。

### 🔧 已解决编译问题（本次搭建踩的坑）

| 问题 | 原因 | 解决 |
|------|------|------|
| 浅克隆子模块空目录 | `git clone --depth 1` 中断，子模块只有 `.git` 无内容 | `git submodule update --init --recursive --force`（用 `git -C` 避免目录问题） |
| partitions.csv 报错 | CSV 不支持行尾 `#` 注释 | 移除行尾注释 |
| GLOB 触发组件管理器 bug | `file(GLOB_RECURSE)` 与 idf_component_manager 2.5 不兼容 | 显式列出源文件 |
| lv_timer_create 编译错 | LVGL 9 回调签名改为 `lv_timer_cb_t(lv_timer_t*)` | 修正回调签名，用 `lv_timer_get_user_data` |
| 字体未定义 | LVGL 9 默认只启用了部分字体 | 用内置 `lv_font_montserrat_14`（大字号需 menuconfig 启用） |
| include 找不到 | `main/CMakeLists.txt` 未含 `include/` 目录 | INCLUDE_DIRS 加上 `"include"` |

---

## 2026-08-27（前期调研）

### 开发板选型确认

**型号**：Waveshare **ESP32-S3-Touch-LCD-1.85B**
**特点**：1.85" 圆形 360×360 电容触摸屏 / 双 MIC + ES7210 回声消除 / ES8311 音频编解码 /
QMI8658 六轴 IMU / PCF85063 RTC / BQ27220 电量计 / TF 卡 / 锂电池接口 / Type-C

**关键外设引脚**（官方 Wiki）：
| 外设 | 引脚/地址 |
|------|-----------|
| LCD ST77916 (QSPI) | CS=21 PCLK=40 D0-3=46/45/42/41 RST=3 BL=5 |
| 触摸 CST816S (I2C) | SCL=10 SDA=11 RST=1 INT=4，@0x15 |
| 音频输出 ES8311 | MCLK=2 BCLK=48 LRCK=38 DOUT=47 DIN=39 PA=9，@0x30 |
| 音频输入 ES7210 | 与 ES8311 共享 I2S，@0x80 |
| IMU QMI8658 | @0x6B |
| RTC PCF85063 | @0x51 |
| 电量计 BQ27220 | @0x55 |
| SDMMC | CLK=15 CMD=14 D0-3=16/17/12/13 |

**调试方式确认**：Type-C 直连 USB-Serial-JTAG 即可烧录与看日志，**无需外接 USB-TTL**。
按住 BOOT 可强制进入下载模式。

### 技术要点备忘

- **ESP-SR 唤醒词是纯软件算法**（WakeNet 神经网络，跑在 CPU 上，无专用硬件），
  ES7210+双 MIC 只是输入保障（硬件回声消除），"听懂唤醒词"由 MFCC+神经网络完成。
- 乐鑫小智AI（xiaozhi-esp32）官方案例支持本板卡，可作参考实现。
- BSP 未发布到乐鑫组件注册表，需从 Waveshare 官方示例仓库拷贝。