# 开源音频链路运行问题与修复记录

本文记录 `CONFIG_USE_OPEN_SOURCE_AUDIO=y` 后，在 ESP32-S3 上用开源库替换闭源音频链路时遇到的实时性问题、根因分析、代码修复和验证结果。

## 背景

当前开源音频链路由以下模块组成：

- Opus 编解码：`components/libopus`
- 重采样：`components/libspeexdsp`
- AFE 处理：`main/audio/processors/open_afe_audio_processor.*`
- NS/VAD/AEC 适配：`open_ns.*`、`open_vad.*`、`open_aec.*`
- 唤醒词临时后端：`open_afe_wake_word.*` 或 `mww_wake_word.*`

默认配置已启用：

```text
CONFIG_USE_OPEN_SOURCE_AUDIO=y
```

## 问题一：OpenAfeProc 输入 backlog

### 现象

运行中曾出现：

```text
W OpenAfeProc: audio backlog 3200 > 3 frames, dropping
```

含义是 AFE 处理线程消费速度低于音频输入速度，输入缓冲持续堆积，最终开始丢弃旧音频帧。

### 根因

主要有三点：

1. `OpenAfeProc` 的处理链路里使用显式 `vTaskDelay(1)` 让出 CPU。当前 tick 配置下 1 tick 约等于 10ms，而音频输入节奏本身也是 10ms 级别，固定延迟会直接吃掉实时预算。
2. 软件 AEC 路径基于 Speex MDF，在 ESP32-S3 上无法稳定在每帧预算内完成处理。
3. AFE 处理线程如果被下游编码队列阻塞，会把编码慢的问题反向传导到采集处理链路。

### 修复

修复点：

- `OpenAfeProc` 不再每轮显式 `vTaskDelay(1)`，依赖队列等待自然阻塞释放 CPU。
- 当前开源链路禁用 Speex AEC，仅保留 NS/VAD。设备在无 AEC 时使用非打断式监听流程，TTS 播放结束后再进入监听，避免同播同录场景下必须做回声抵消。
- AFE 处理任务使用静态任务栈，栈放到 PSRAM，任务控制块保留在内部 RAM，销毁时等待任务确认退出，避免 task stack 过早释放导致 UAF 风险。
- 实时编码队列满时丢弃最旧实时帧，不阻塞 AFE 生产线程。

## 问题二：encode queue full 与 Opus 编码超时

### 现象

AFE backlog 消失后，曾出现：

```text
W AudioService: encode queue full, dropping oldest realtime audio frame
E task_wdt: Task watchdog got triggered
CPU 1: opus_codec
```

对 WDT backtrace 解码后，热点落在 libopus SILK float 编码路径：

```text
silk_warped_autocorrelation_FLP
silk_noise_shape_analysis_FLP
silk_encode_frame_FLP
silk_Encode
opus_encode
esp_opus_enc_process
AudioService::OpusCodecTask
```

### 根因

`components/libopus/CMakeLists.txt` 原先编译了 `silk/float/*.c`，并假设 ESP32-S3 可以承担 float SILK 编码。实际运行中，SILK float analysis 在 ESP32-S3 上耗时过高，`opus_codec` 线程持续占用 CPU1，导致：

- 编码队列消费不及时；
- 实时音频帧被丢弃；
- IDLE1 无法及时运行，触发 task watchdog。

### 修复

修复点：

- libopus 改为 fixed-point SILK：
  - 使用 `silk/fixed/*_FIX.c`
  - 编译参数启用 `FIXED_POINT=1`
  - 编译参数启用 `DISABLE_FLOAT_API`
  - 移除 float analysis 相关源文件
- Opus encode complexity 从 `5` 降到 `0`，优先保证实时性。
- 编码线程不再在 encode 分支里额外 `vTaskDelay(1)`，避免人为降低编码吞吐。

## 当前测试结果

最新 `logfile.txt` 显示开源音频链路已能完成一次完整对话流程：

```text
I OpenOpus: encoder: sr=16000 ch=1 complexity=0
I Application: Wake word detected: 你好小智
I OpenAfeProc: init: frame=60ms (960 samples) aec=off ns=yes vad=yes
I OpenAfeProc: processing task started
I Application: >> Hi, 小智
I Application: << 嗨，咋啦。
...
I MQTT: Received goodbye message
I StateMachine: State: listening -> idle
```

当前日志中未再出现：

```text
audio backlog
encode queue full
Task watchdog got triggered
```

构建验证：

```text
idf.py build
Project build complete.
Generated build/xiaozhi.bin
```

## 已知取舍

- 当前开源 AEC 暂时关闭，牺牲播放中打断能力，换取实时稳定性。
- `WAKE_WORD_BACKEND_VAD` 仍是开发期临时唤醒方案，误触发率高，不适合最终产品。
- Opus complexity 当前为 `0`，后续可在稳定基础上逐步测试 `1`、`2`，以评估音质与 CPU 预算的平衡。
- 如果未来重新启用 AEC，需要优先验证 fixed-point Speex、WebRTC AEC3 或其他更适合 ESP32-S3 的实现，不能直接恢复当前 Speex MDF 浮点/高耗时路径。

## 关键代码位置

- `components/libopus/CMakeLists.txt`
- `main/audio/audio_service.h`
- `main/audio/audio_service.cc`
- `main/audio/processors/open_afe_audio_processor.cc`
- `main/audio/processors/open_afe_audio_processor.h`
- `main/CMakeLists.txt`
- `main/Kconfig.projbuild`
- `sdkconfig.defaults`
