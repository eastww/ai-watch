# AI Watch 音频系统笔记（M2）

> 本文档整理本项目（Waveshare ESP32-S3-Touch-LCD-1.85B）音频子系统的基础概念、
> 硬件架构、数据流格式与排查经验，供后续 M3/M4 开发参考。
>
> 相关代码：`main/src/audio/audio.c`、`main/include/audio/audio.h`
> 相关 BSP：`components/waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c`

---

## 1. 基础概念

### 1.1 什么是 CODEC

**CODEC = COder-DECoder（编码器-解码器）**

音频语境下指能完成 **模拟 ↔ 数字** 双向转换的芯片：

| 方向 | 全称 | 功能 | 本板芯片 |
|---|---|---|---|
| **ADC** | Analog-to-Digital Converter | MIC 模拟电压 → 数字（录音/编码） | **ES7210** |
| **DAC** | Digital-to-Analog Converter | 数字 → 模拟驱动喇叭（播放/解码） | **ES8311** |

`esp_codec_dev` 是 ESP-IDF 对 codec 芯片驱动的统一抽象层，日志里的
`Adev_Codec: Open codec device OK` 即来自该组件。

### 1.2 采样率 / 位深 / 声道

- **采样率 $f_s$（Sample Rate）**：每秒采集的声音样本数（单位 Hz）。
  常说的 **48k / 96k 指的就是采样率**（48000 / 96000 Hz），即 LRCLK 频率。

| 采样率 | 场景 |
|---|---|
| 8k / 16k | 语音 / 电话（本项目用 **16k**）|
| 44.1k | CD 音质 |
| 48k | 专业音频 / 视频标准 |
| 96k / 192k | 高解析度音频（Hi-Res）|

- **位深（Bit Depth）**：每个采样点用多少 bit 表示，常见 16 / 24 / 32 bit。
  本板 I2S 总线用 **32bit** 槽位；ES7210 是 **24bit ADC**。
- **声道（Channel）**：单声道（1ch）或双声道（2ch）。本板录音为双 MIC → 2ch。

### 1.3 三级时钟：LRCLK / BCLK / MCLK

一句话记忆：**LRCLK 定"帧"、BCLK 定"位"、MCLK 定"系统节奏"**。

| 时钟 | 全称 | 频率 | 职责 |
|---|---|---|---|
| **LRCLK** | Left/Right Clock（= WS, Word Select）| = 采样率 $f_s$ | 标识每个采样帧；高低电平区分左右声道（先左后右）|
| **BCLK** | Bit Clock（= SCLK）| = $f_s$ × 声道 × 位深 | 传输每个 bit 的节拍，一个周期送一位数据 |
| **MCLK** | Master Clock | = $f_s$ × 256（或 512）| 给 codec 内部 PLL / 滤波器 / Delta-Sigma 调制器作参考 |

频率公式：

$$LRCLK = f_s \qquad BCLK = f_s \times \text{channels} \times \text{bits} \qquad MCLK = f_s \times 256$$

**本项目实际参数**（16kHz / 2ch / 32bit / MCLK 256×）：

| 时钟 | 计算 | 数值 |
|---|---|---|
| LRCLK | 16000 | **16 kHz** |
| BCLK | 16000 × 2 × 32 | **1.024 MHz** |
| MCLK | 16000 × 256 | **4.096 MHz** |

对应日志：`I2S_IF: STD: TX, sample_rate_hz: 16000, mclk_multiple: 256`

**一个 LRCLK 周期内**：L 通道 32bit + R 通道 32bit = 64 个 BCLK 周期。

### 1.4 MCLK 的来源

- MCLK **可以由独立外部时钟提供**（音频晶振、时钟发生器、他控输出），音响系统常见。
- **但"独立"不等于"随意"**：MCLK 必须与采样率保持标准整数倍关系（256/512×），
  且组合必须在 codec 驱动的预置表内（ES7210 的 `coeff_div` 表），否则初始化直接失败。
- 本板由 **ESP32 提供 MCLK**（`use_mclk = true`），好处：同源同步、采样率可软件动态调整、省晶振。

---

## 2. 硬件架构

### 2.1 音频链路

```
ESP32-S3 I2S0
        │
        ├──── MCLK(GPIO2) ──→ ES8311 + ES7210   （主时钟，两芯片共享）
        ├──── BCLK(GPIO48) ─→ ES8311 + ES7210   （位时钟，共享）
        ├──── LRCLK(GPIO38) → ES8311 + ES7210   （帧时钟，共享）
        ├──── DOUT(GPIO47) ──→ ES8311           （ESP32→codec，播放数据 TX）
        └──── DIN(GPIO39) ←─── ES7210           （codec→ESP32，录音数据 RX）

播放：ESP32 I2S(TX) → ES8311(DAC) → PA(GPIO9) → 扬声器
录音：双 MIC → ES7210(ADC) → I2S(RX) → ESP32
```

- 三条时钟线（MCLK/BCLK/LRCLK）**物理共享**；数据线 DOUT/DIN **方向独立**。
- 三颗时钟全部由 ESP32 作为 **I2S Master** 产生，ES8311/ES7210 均为 **Slave** 被动接收。

### 2.2 共享 I2S → 半双工

**MIC（ES7210）与 Speaker（ES8311）共用同一路 I2S 总线**：

- BSP `bsp_audio_init()` 只初始化一次 I2S 总线（`i2s_data_if` 静态全局判重）。
- speaker 与 mic 创建 codec 设备时 **传入同一个 `i2s_data_if`**：

```c
// speaker
esp_codec_dev_cfg_t codec_dev_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    .codec_if = es8311_dev,
    .data_if = i2s_data_if,   // 同一个
};
// mic
esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_IN,
    .codec_if = es7210_dev,
    .data_if = i2s_data_if,   // 同一个
};
```

- 两芯片都工作在 Slave、共享同一组时钟 → **同一时刻采样率/帧格式必须一致**，
  因此采用**半双工时分复用**：播放时 open ES8311、录音时 open ES7210，依次 close。

**典型时序**（`audio.c`）：

```
audio_play_pcm():    open(spk) → set_vol → write → 延时(播放时长+100ms) → close(spk)
audio_record_start(): open(mic) → read 循环 → close(mic)
```

日志证据（同一总线模式切换）：

```
I (2250) I2S_IF: current mode: playback, ...
I (4820) I2S_IF: current mode: record, ...
I (6100) I2S_IF: current mode: playback, ...
```

> ⚠️ **M3/M4 通话模式**不能指望两个 codec 同时工作，需设计"播放/录音快速切换调度器"。

---

## 3. 数据流格式

### 3.1 对外 API 语义（16bit 单声道 PCM）

- 播放 `audio_play_pcm(const int16_t *data, size_t samples)`：**16bit 单声道** PCM。
- 录音回调 `audio_record_cb_t(const int16_t *pcm, size_t samples, void *ctx)`：**16bit 单声道** PCM。

### 3.2 内部 I2S 格式（32bit 双声道）

- 与 Waveshare 官方 demo 一致：**32bit / 双声道 / 16000Hz**。
- `esp_codec_dev` open 时通过 `codec_fs()` 固定 2ch / 32bit，采样率动态。

### 3.3 播放转换（16bit mono → 32bit stereo）

```c
for (size_t i = 0; i < samples; i++) {
    int32_t v = ((int32_t)data[i]) << 16;  // 数据放高 16 位（左对齐）
    sbuf[i * CODEC_CHANNELS]     = v;      // L
    sbuf[i * CODEC_CHANNELS + 1] = v;      // R（左右相同）
}
```

### 3.4 录音转换（32bit stereo → 16bit mono）

```c
mono[i] = (int16_t)(rbuf[i * CODEC_CHANNELS] >> 8);  // 取左声道
```

**关键：ES7210 是 24bit ADC，数据右对齐在 32bit 槽的低 24 位，用 `>>8` 取有效 16bit。**

**数据量核对**（每块 20ms）：

```
ch_frames = s_sample_rate / 50 = 16000 / 50 = 320 帧
rbuf:  320 帧 × 2 声道 × 4 字节 = 2560 字节 = 640 个 int32
mono:  320 个 int16 = 640 字节   ← 每帧取左声道，右声道丢弃（双声道→单声道下混）
```

- `i` 是"帧索引"而非 int32 索引，`rbuf[i * 2]` 跳过右声道取左声道。
- 读入 640 个 int32、产出 320 个 int16，**数据不丢帧**，只是降为单声道。
- 将来做回声消除（AEC）时需把两路都保留分别处理。

---

## 4. 排查经验（踩坑记录）

### 4.1 播放"完全无声"

- **现象**：开机音、回放都听不到。
- **根因**：`esp_codec_dev_write()` 后**立即 `esp_codec_dev_close()`**。
  `i2s_channel_write()` 只把数据写入 **DMA 缓冲即返回，不等播放完成**；
  紧接着 `close()` 会立刻 mute + 关闭 PA + suspend codec + disable I2S → 音频被掐断。
- **修复**：write 成功后**延时播放时长 +100ms 再 close**：

```c
uint32_t play_ms = (uint32_t)(samples * 1000 / s_sample_rate);
vTaskDelay(pdMS_TO_TICKS(play_ms + 100));
esp_codec_dev_close(dev);
```

### 4.2 录音回放"几乎无声"（音量 -46dB）

- **现象**：播放链路正常（开机音可听），回放录音只听到极微弱声音。
- **诊断日志**（实测）：
  ```
  first raw L: 0000af00 00014e00 ...
  record stats: raw_peak=10424320 mono_peak=158 avg_abs=9
  ```
  raw_peak = 10424320 = `0x9F2000` 说明 **MIC 原始信号正常**（接近满量程）。
- **根因**：取位错误。`>>16` 得到 `0x9F2000 >> 16 = 0x9F = 159`（正是 mono_peak=158），
  而 ES7210 数据**右对齐在低 24 位**，正确应 **`>>8`**。
- **修复**：`mono[i] = (int16_t)(rbuf[i * CODEC_CHANNELS] >> 8);` 音量恢复满量程。

### 4.3 esp_codec_dev 返回值语义

- `esp_codec_dev_read/write` 返回的是**状态码**（`ESP_CODEC_DEV_OK = 0` 成功），
  **不是字节数**。判断成功用 `ret == ESP_CODEC_DEV_OK`。

### 4.4 音量设置时序

- `esp_codec_dev_set_out_vol()` **必须在 codec open 之后调用**；
  未 open 时调用会静默失败且不保存音量。

### 4.5 I2S 格式必须与官方 demo 一致

- 早期用 16bit/单声道导致完全无声、无录音数据；对齐 **32bit/双声道** 后恢复正常。
- `_i2s_valid_fmt` 要求声道为偶数 → 必须用 2ch。

---

## 5. 常用速查

| 项 | 值 |
|---|---|
| 采样率 $f_s$ | 16000 Hz |
| I2S 位深 | 32 bit |
| 声道 | 2（stereo）|
| MCLK 倍率 | 256 × |
| 录音块时长 | 20ms（`rate/50` 帧）|
| MIC PGA 增益 | 默认 30dB（驱动 `_es7210_set_channel_gain(codec, 0xF, 30.0)`，最高 37.5dB）|
| 播放音量 | `s_volume = 80`（`audio_set_volume()` 可调 0-100）|
| PA 引脚 | GPIO 9（`BSP_POWER_AMP_IO`，高电平使能）|

## 6. 后续方向

- **M3**：录音流式上传云端 ASR（16k mono PCM 已就绪）。
- **M4**：TTS 下传播放（16k mono → 32bit stereo 转换已就绪）。
- **通话模式**：设计播放/录音快速切换调度器（半双工约束）。
- **可选优化**：MIC PGA 调高、播放音量策略、双 MIC 下混（平均）或 AEC 保留两路。
