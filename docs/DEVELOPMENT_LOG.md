# 开发记录 (DevLog)

本文件按时间顺序记录本项目的开发过程、关键决策、遇到的问题与解决方案。
按日期倒序排列，最新记录在最上方。

---

## 2026-08-30（续3）

### 🚀 M3 启动：WiFi 管理模块（wifi_mgr）

- 新建 `main/src/wifi/wifi_mgr.c` + `main/include/wifi/wifi_mgr.h`：
  - STA 模式异步连接（事件驱动），断线自动重连（`WIFI_MAX_RETRY` 次）。
  - 状态机：DOWN → CONNECTING → CONNECTED / DISCONNECTED，支持多回调注册。
  - `esp_netif`/`esp_event_loop` 幂等初始化；NVS 惰性初始化。
- `app_main.c` 接入：注册 `wifi_state_cb` 更新屏幕顶部 WiFi 状态标签
  （灰色=连接中，绿色=已连接+IP，红色=断开），并打印本机 IP。
- `config.h` 改进：用 `__has_include` 自动检测本机 `secrets.h`
  （已被 gitignore），不再依赖手动的 `CONFIG_AI_WATCH_USE_SECRETS` 开关。
- 开机日志预期：`STA start, connecting to '<ssid>'...` → `got ip: x.x.x.x`
  → `state -> CONNECTED`。

**备注**：SSID/密码取自 `main/include/secrets.h`（本地，不入库）。
若未配置，`wifi_mgr_init()` 返回 false 并跳过 WiFi（不阻塞 UI）。

---

## 2026-08-30（续2）

### 🔧 修复：录音回放"几乎无声"（根因：ES7210 24bit 右对齐，误用 >>16 取位）

**现象（实测日志，commit 8baf7be 诊断版）**：
- 播放链路正常（开机提示音清晰可听），但**录音回放只听到极微弱声音**。
- 诊断日志：
  ```
  first raw L: 0000af00 00014e00 0001fc00 00020200 | R: 00060900 00062500 ...
  record stats: raw_peak=10424320 mono_peak=158 avg_abs=9 total=16000
  ```

**根因分析**：
- `raw_peak = 10424320 = 0x9F2000` → **MIC 原始信号完全正常**（接近满量程），
  数据一直在流入。
- 但旧代码用 `>>16` 取左声道：`0x9F2000 >> 16 = 0x9F = 159`，
  正好等于日志中的 `mono_peak=158` → 有效信号被砍掉约 250 倍（≈ -46dB）。
- **本质**：ES7210 是 **24bit ADC**，数据**右对齐**在 32bit 槽的**低 24 位**
  （如 `0x0000af00`），正确提取 16bit 应为 **`>>8`**，不是 `>>16`。

**修复**（`main/src/audio/audio.c`）：
```c
mono[i] = (int16_t)(rbuf[i * CODEC_CHANNELS] >> 8);   // 24bit 右对齐 -> 16bit
```

**经验教训**：
1. **确认 codec 数据位宽与对齐方式**：ES7210 是 24bit ADC（右对齐），
   必须看寄存器/datasheet 确认有效数据在高位还是低位，再决定移位量。
2. **用诊断日志量化**：统计 `raw_peak`（原始 int32 峰值）与 `mono_peak`
   （提取后峰值）即可瞬间定位"MIC 没数据" vs "取位不对"。
3. 32bit 槽内 24bit 数据：`>>8`（右对齐）或 `<<8`（左对齐），
   高 16 位对齐时才是 `>>16`。

---

## 2026-08-30（续）

### 🔧 修复：录音打通但播放无声（根因：write 后立即 close 掐断音频）

**现象（实测日志，commit 72a57d9）**：
- 录音成功：`captured 16000 samples`（1 秒 @16kHz）✅
- 播放路径 open OK、write 阻塞约 1.2 秒（数据在写入 DMA），
  但扬声器**完全无声**——连开机纯正弦提示音也无声音。

**根因**：`audio_play_pcm()` 里 `esp_codec_dev_write()` 后**立即
`esp_codec_dev_close()`**。而：
1. `i2s_channel_write()` 只是把数据写入 **DMA 缓冲**就返回，**不等
   播放完成**（音频是由 I2S 时钟逐帧推出去的）。
2. `esp_codec_dev_close()` 内部会立即 **mute + 关闭 PA + suspend
   codec + disable I2S 通道**。
3. 于是 DMA 里还没播完（甚至还没开始播）的音频直接被掐断 → 完全无声。

**修复**（`main/src/audio/audio.c`）：
- write 成功后**延时播放时长 +100ms 再 close**，让 DMA 有足够时间
  把音频全部推给 ES8311 → 扬声器播完。
- 增加诊断日志：
  - `esp_codec_dev_dump_reg()` 打印 ES8311 全部寄存器（确认初始化、
    mute/音量/时钟状态）
  - write 失败时打 `codec write FAILED ret=%d`
  - 播放完成打 `playback done: N samples (M ms)`

**经验教训**：
1. **I2S 播放是"异步推流"**：`esp_codec_dev_write` 填满 DMA 缓冲即返回，
   真正发声依赖 I2S 时钟持续把 DMA 数据推出。write 后**绝不能立即
   close**（close 会 mute/关 PA/disable 通道），必须等播放时长过去。
2. 排查"无声"时先确认：数据是否写入成功（write 返回值）、DMA 是否在
   被消费（write 是否阻塞了约播放时长）、PA 是否使能、ES8311 寄存器
   的 mute/音量/时钟状态。

---

## 2026-08-30

### 🔧 修复：无声音 + 录音无数据（根因：esp_codec_dev 返回值语义误用）

**现象（实测日志）**：
- 录音 started → stopped 约 3 秒，**期间没有任何 read 错误日志**，
  但 captured 0 samples，屏幕显示 "No audio check MIC"。
- 播放路径 open OK 两次（两个提示音），但扬声器无声。

**真正根因（两个叠加 bug）**：

1. **录音无数据**：`esp_codec_dev_read()` 返回的是**状态码**（成功=
   `ESP_CODEC_DEV_OK`=0），**不是实际读取的字节数**！旧代码 `if (n > 0)`
   永远不成立（成功时 n==0），导致数据其实一直在流入 `rbuf` 却被丢弃，
   回调从不执行。日志证据：3 秒内 `n < 0` 分支从未触发（无 err 日志），
   说明 read 一直成功，数据一直在。
   - **修复**：改判断为 `if (ret == ESP_CODEC_DEV_OK)`，成功后按请求的
     整块大小处理（I2S 阻塞读会填满请求量）。

2. **播放无声**：`esp_codec_dev_set_out_vol()` 在 codec **未 open** 时
   调用会失败——`_verify_codec_setting()` 返回 `WRONG_STATE` 直接 return，
   **`dev->volume = volume` 那行在 return 之后，根本没执行** → 音量保持
   默认 0 → open 时框架 `_update_codec_setting()` 用 `_get_vol_db(0)`
   = **-96dB（几乎静音）** → 无声！
   - **修复**：把 `esp_codec_dev_set_out_vol()` 移到 `esp_codec_dev_open()`
     成功**之后**再调用。

**经验教训**：
1. `esp_codec_dev_read/write` 返回**状态码**（`ESP_CODEC_DEV_OK`=0），
   不是字节数。不要用返回值判断读到了多少。
2. `esp_codec_dev_set_out_vol/mute` 必须在 codec open 之后调用，否则
   静默失败且音量不会被保存（框架内部有 WRONG_STATE 校验）。
3. 判断"数据是否真的在流入"：看日志里 read 是否长时间无 err 且无数据
   到达，这是"数据被丢弃"而非"硬件没数据"的典型特征。

---

## 2026-08-29（续）

### 🔧 修复：无开机声音 + 录音无数据（No audio / check MIC）

**现象**：烧录后屏幕/触摸正常，但：
- 开机无提示音（扬声器无声）
- 触摸触发录音自测后，屏幕显示 "No audio\ncheck MIC"（`esp_codec_dev_read` 拿不到数据）

**根因**：codec 设备 open 时使用的 **I2S 数据格式与 Waveshare 官方 demo 不一致**。
- 我的旧实现：`esp_codec_dev_open()` 传 `16bit / 单声道 / 16kHz`，且手动调用
  `bsp_audio_init(&std_cfg)` 传自定义 I2S 配置。
- 官方 demo（已验证能出声）：`bsp_audio_codec_speaker_init()` 内部自动初始化
  （I2C + I2S 默认配置），`esp_codec_dev_open()` 传 **`32bit / 双声道 / 16kHz`**。

ESP-IDF `esp_codec_dev` 框架要求 channel 为偶数，单声道需转成
`channel=2 + channel_mask(bit0)`，且 ES8311/ES7210 对 slot 位宽/声道有
严格要求——16bit 单声道配置下数据流不匹配，导致 ES8311 无法解码出声音、
ES7210 无法产出数据。

**修复**（`main/src/audio/audio.c` 重写）：
1. **不再手动调 `bsp_audio_init()`**，完全交给 BSP 的
   `bsp_audio_codec_speaker_init()` / `bsp_audio_codec_microphone_init()`
   内部自动初始化（与官方 demo 一致）。
2. codec open 统一用 **`32bit / 2ch / 16kHz`**（与官方 demo 一致）。
3. 模块内部做格式转换，**对外 API 保持 16bit 单声道 PCM 语义不变**：
   - 播放：16bit mono → 32bit stereo（左右声道相同，数据放高 16 位）
   - 录音：32bit stereo → 16bit mono（取左声道高 16 位）

**结果**：✅ 编译通过（`ai_watch.bin` 0xab570），待烧录实测。

**经验教训**：
1. 音频 codec（ES8311/ES7210）的 I2S 数据格式**不能随意选**，必须以
   Waveshare 官方 demo 的 `32bit/2ch/16kHz` 为准，否则硬件不工作。
2. 不要手动传自定义 `i2s_std_config_t` 给 `bsp_audio_init()`，让 BSP
   内部自动初始化更稳妥。

---

## 2026-08-29

### ✅ M2 里程碑：音频模块（播放 + 录音）编译通过

**完成内容**
- 新增 `main/src/audio/audio.c`（+ `include/audio/audio.h`）：
  - **播放（ES8311）**：`audio_play_init()` / `audio_play_pcm()` /
    `audio_play_tone()`（正弦提示音）/ `audio_set_volume()`
  - **录音（ES7210 + 双MIC）**：`audio_record_start()` / `audio_record_stop()`，
    PCM 回调逐块（20ms/320样本）输送，返回 false 可主动停止
- 主程序集成：
  - 开机播放双音提示（660Hz + 990Hz）验证扬声器链路
  - 触摸屏幕触发"录音 1s → 回放"自测（验证 MIC → ES7210 → I2S → 回放闭环）
- 清除了与 DeepSeek 图片上传相关的残留代码（`main/src/http/`、`main/src/image/`）

**硬件链路（半双工，播放/录音不能同时）**：
```
播放：ESP32-S3 I2S(TX) -> ES8311 -> PA(GPIO9) -> 扬声器
录音：双MIC -> ES7210(回声消除) -> I2S(RX) -> ESP32-S3
```

**编译结果**：✅ `ai_watch.bin` 0xab430 bytes（~701KB），分区剩余 95%。

**经验教训**：
1. `esp_codec_dev_write()` 第二参数是 `void *`（非 const），传 `const` 指针
   需显式强转，否则 -Werror 报错。
2. 编辑 `app_main.c` 时若引用跨函数的 static 任务/变量，务必同步删除或保留，
   否则会被编译器的 "unreferenced" 误伤——这次就是残留了已删除的
   `image_test_task`/`send_image_task` 引用导致编译失败。

### 🔧 修复：触摸无反应（LVGL 事件机制陷阱）

**现象**：烧录后屏幕正常显示，但点击触摸屏无任何反应（日志无输出）。

**根因**：
- BSP 的 `esp_lv_adapter_register_touch()` 内部会调用
  `lv_indev_set_read_cb(indev, lvgl_touch_read)` 注册**真正的触摸读取回调**，
  它负责读 CST816S 触摸芯片数据（支持 IRQ 中断模式）。
- 而 `app_main.c` 里用 `lv_indev_set_read_cb(tp, my_touch_read_cb)` **覆盖**了
  BSP 的回调，但我的回调只检查 `data->state`，而 LVGL 传给它的 `data`
  是个空结构——**没有任何真实触摸数据被读取**，所以永远感知不到触摸。

**正确做法**：
- **不要覆盖 BSP 的 indev read_cb**——触摸数据由 BSP 驱动持续读取。
- 业务响应触摸应使用 **LVGL 事件机制**：
  ```c
  lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_CLICKED, NULL);
  // 回调里用 lv_event_get_code(e) 判断事件类型
  ```
- 屏幕对象/控件绑定事件即可，底层读取交给 BSP。

**结果**：✅ 编译通过，点击屏幕触发 `LV_EVENT_CLICKED` → 提示文字更新 + 日志输出。

**经验教训**：
1. `lv_indev_set_read_cb()` 是"谁来读取触摸硬件"的底层回调，只有驱动层能碰；
   应用层响应触摸必须用 `lv_obj_add_event_cb()`。
2. 排查触摸问题：先看日志里 BSP 有没有打 `Touch input device registered`，
   再确认有没有覆盖 read_cb。

**注意**：LVGL 默认只分发 `LV_EVENT_CLICKED` 到"被点击对象"上，
如果点击在空白处需绑定到 `screen_active()`（本工程已绑定）。

---

## 2026-08-28（晚间）

### 🔧 修复：持续编译报错 + 白屏问题（已完成）

**现象**：
- `idf.py build` 报 `ninja failed`。
- 烧录后白屏、反复重启（`task_wdt` 看门狗超时）。

**根因（两个独立问题）**：

1. **编译报错直接原因**：`main/CMakeLists.txt` 引用的源文件
   `app_main_simple_2.c` 不存在（调试过程中残留了 5 个实验副本：
   `app_main_api_error.c` / `app_main_full.c` / `app_main_previous.c` /
   `app_main_with_state.c` / `app_main_with_touch_issue.c`，没有一个叫被引用的名字）。
   - 解决：删除全部实验副本，CMakeLists 改为显式引用
     `app_main.c` + `src/app_state.c`，`PRIV_REQUIRES` 与已验证过的结构一致。

2. **LVGL 9 API 使用错误**：用了不存在的 `lv_indev_set_cb`，
   应为 `lv_indev_set_read_cb`（`lv_indev_read_cb_t` 签名）；且触摸回调
   定义在函数内部（GCC 嵌套函数扩展）。
   - 解决：回调移到文件作用域，改用标准 LVGL 9 API，`
   bsp_display_lock()/unlock()` 保护。

**白屏根因（上轮已确认）**：`app_main` 手动调 `lv_timer_handler()` 与
`esp_lv_adapter` 内部 worker 任务并发驱动 LVGL → 数据竞争 → 看门狗复位。
本轮延续修复：主循环只做日志，不再碰 LVGL。

**IDE 假报错**：IntelliSense 找不到 `FreeRTOS.h`/`sdkconfig.h` 等
（不影响编译）。解决：新增 `.vscode/c_cpp_properties.json`
指向 `build/compile_commands.json`。

**清理**：删除调试残留临时文件（`test_syntax.py`、`QUICK_FIX.md`、
`CODE_VERIFICATION.md`、`TROUBLESHOOTING.md`、`BUILD_INSTRUCTIONS.md`、
`setup_env.ps1`）。

**结果**：✅ `idf.py build` 通过（`ai_watch.bin` 0x9d170 bytes，分区剩余 96%）。

**经验教训**：
1. 每次改 `app_main` 前先同步更新 `main/CMakeLists.txt` 里的源文件列表，
   否则会出现"引用不存在的文件"这类莫名报错。
2. 不要在 `main/` 下堆实验副本文件，改用 git 分支/提交管理版本。
3. LVGL 9 触摸 API：`lv_indev_set_read_cb()`，不是 v8 的 `lv_indev_set_cb()`。

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