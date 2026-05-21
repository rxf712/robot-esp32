# ESP32-S3 视频+语音并发资源分析报告

**硬件：** camera-xiaozhi (ESP32-S3, 8MB PSRAM, GC0308摄像头)  
**固件版本：** 2.2.6 (feat/camera-xiaozhi)  
**分析基准：** logfile.txt (2026-05-21)

---

## 一、问题现象

```
W (39586) AFE: Ringbuffer of AFE(FEED) is full,
          Please use fetch() to read data to avoid data loss or overwriting
```

该警告在唤醒词识别后进入 listening 状态约 **2 秒后**持续刷出，直到日志结束（约 15 秒内连续 ~500 条），期间语音功能实质性失效（麦克风数据丢失）。

---

## 二、根本原因分析

### 2.1 时间线还原

| 时间戳 (ms) | 事件 |
|------------|------|
| 1116 | LocalStream 启动(port 80), CameraStream 以 5fps 开始 |
| 7856 | 进入 idle, **free SRAM = 19,415 B**, minimal = 10,691 B |
| 12686 | 唤醒词触发 → connecting |
| 12796 | `esp-aes: Failed to allocate memory` → MQTT SSL 失败，断连 |
| 34996 | 第二次唤醒 → listening |
| 35026 | AfeAudioProcessor 初始化(VAD-only pipeline) |
| 35596 | listening → speaking |
| 37806 | speaking → listening |
| **39586** | **AFE FEED ringbuffer 开始溢出（持续至日志末尾）** |
| 46806 | **free SRAM = 6,303 B, minimal = 123 B** ← 接近耗尽 |

### 2.2 直接原因：任务优先级冲突

AFE FEED ringbuffer 溢出的机制：

```
[I2S DMA] --feed()--> [AFE FEED ringbuffer] --AFE内部任务--> [fetch output]
                                                ↑
                              priority=1 被高优先级任务抢占
```

系统任务优先级对比：

| 任务 | 优先级 | 绑定核心 | 说明 |
|------|--------|----------|------|
| WiFi Driver | 23 | Core 0 | 最高 |
| **camera_capture** | **5** | **Core 1** | 摄像头捕获+JPEG编码 |
| audio_communication | 3 | 任意 | AfeAudioProcessor fetch |
| encode_wake_word | 2 | 任意 | 唤醒词Opus编码 |
| **AFE 内部处理任务** | **1** | 任意 | AEC/VAD/WakeNet推理 |

**AFE 内部任务 priority=1**，是系统中最低优先级。camera_capture 任务 priority=5，在同核心上完全抢占 AFE 处理。

每帧 640×480 YUYV → JPEG 编码需要数十毫秒 CPU，占满 Core 1。AFE 内部任务无法运行 → FEED ringbuffer 无法被消费 → 溢出警告 → 麦克风数据全部丢失。

### 2.3 根本原因：摄像头未在语音会话期间暂停

`camera_xiaozhi_board.cc` 的状态回调：

```cpp
void OnDeviceStateChanged(DeviceState state) override {
    if (state == kDeviceStateIdle) {
        camera_stream_->Resume();
    } else if (state == kDeviceStateConnecting) {
        camera_stream_->Pause();   // ← 只在 Connecting 暂停
    }
    // ❌ kDeviceStateListening 和 kDeviceStateSpeaking 未处理！
}
```

实际进入 listening 状态时，摄像头继续以 5fps 运行并做 JPEG 编码，持续竞争 CPU。

### 2.4 次要原因：SRAM 严重不足

| 阶段 | 可用 SRAM | 说明 |
|------|-----------|------|
| 启动后 heap_init | ~280 KB | RAM 区域总量 |
| DMA/内部保留 | -96 KB | `Reserving pool of 96K` |
| 激活完成 idle | **19 KB** | WakeNet+AFE+WiFi 消耗约 165 KB |
| 视频+语音会话后 | **6 KB / 最低 123 B** | 几乎耗尽 |

SRAM 耗尽导致的连带问题：
- `esp-aes: Failed to allocate memory` → MBEDTLS SSL 握手失败
- MQTT 连接断开 / 重连失败
- TLS 证书验证中间缓冲区无法分配

---

## 三、视频+语音并发可行性评估

### 3.1 理论资源需求

| 子系统 | 峰值 SRAM | 峰值 PSRAM | CPU 占比(估算) |
|--------|-----------|------------|----------------|
| WiFi/MQTT/TLS | ~30 KB | ~50 KB | Core 0 高占 |
| AFE Wake Word (WakeNet) | ~20 KB | ~150 KB | 每帧推理 |
| AFE Audio Processor (VAD) | ~15 KB | ~50 KB | 低 |
| Opus 编码 | ~10 KB | — | 低 |
| LVGL + LCD | ~5 KB | ~2 MB | Core 0 低 |
| CameraStream (JPEG buf) | — | 64 KB | Core 1 中高 |
| 摄像头帧缓冲 (PSRAM) | — | 614 KB | DMA |
| HTTP 流媒体服务 (httpd) | ~10 KB | — | 按需 |

**可用 SRAM 约 19 KB**，已分布给各子系统后仅剩约 6 KB 裕量，几乎没有缓冲。

### 3.2 结论

| 场景 | 可行性 | 说明 |
|------|--------|------|
| 视频流（空闲状态）| ✅ 可行 | 无语音处理时 CPU 空闲，帧率正常 |
| 语音会话（无视频）| ✅ 可行 | 已验证 14 轮正常对话 |
| **视频+语音同时（当前实现）** | ❌ 不可行 | AFE 被饿死，SRAM 耗尽 |
| 视频+语音（语音时暂停视频）| ✅ 可行 | 只需修复暂停逻辑 |

**结论：** 视频和语音无法真正并发，但**分时复用**（语音时暂停视频）是可行且合理的产品形态。

---

## 四、修复方案

### 4.1 立即修复：扩展暂停状态覆盖

修改 `camera_xiaozhi_board.cc`，将暂停逻辑扩展到所有语音激活状态：

```cpp
void OnDeviceStateChanged(DeviceState state) override {
    if (!camera_stream_) return;
    switch (state) {
        case kDeviceStateIdle:
            camera_stream_->Resume();
            break;
        case kDeviceStateConnecting:
        case kDeviceStateListening:    // ← 新增
        case kDeviceStateSpeaking:     // ← 新增
            camera_stream_->Pause();
            break;
        default:
            break;
    }
}
```

这一改动可消除 AFE ringbuffer 溢出，代价仅是语音期间视频画面冻结（对用户体验几乎无影响，因为用户此时正在说话）。

### 4.2 中期：提升 AFE 任务优先级

在 `afe_wake_word.cc` 中：

```cpp
// 当前：
afe_config->afe_perferred_priority = 1;  // 极低，会被任何任务抢占

// 建议：
afe_config->afe_perferred_priority = 4;  // 高于 encode_wake_word(2) 和 audio_communication(3)
```

同时将 `camera_capture` 任务优先级从 5 降低到 2，使 AFE 不被抢占：

```cpp
// camera_stream.cc:
xTaskCreatePinnedToCore(..., /*priority=*/ 2, ...);  // 从 5 降为 2
```

---

## 五、闭源 AFE 库 vs 开源方案 资源对比

### 5.1 音频处理链路对比

| 组件 | 闭源 AFE (当前 camera-xiaozhi) | 开源方案 (Phase 7, CONFIG_USE_OPEN_SOURCE_AUDIO) |
|------|-------------------------------|--------------------------------------------------|
| **唤醒词** | WakeNet9 (神经网络, 高准确率) | libfvad VAD 代替 (任意语音触发, Phase 8 待完成) |
| **AEC** | ESP SR_HIGH_PERF AEC | speexdsp echo canceller (50ms, 800 taps) |
| **降噪 NS** | NSNet (神经网络) | speexdsp preprocessor (-15 dB) |
| **VAD** | WebRTC VAD + VADNet | libfvad mode=3 (onset=150ms) |
| **Opus 编码** | esp_audio_codec | libopus CELT-only (RESTRICTED_LOWDELAY) |
| **采样率转换** | esp_ae_rate_cvt | speexdsp resampler |

### 5.2 内存占用对比

| 内存类别 | 闭源 AFE | 开源方案 | 差值 |
|---------|---------|---------|------|
| **PSRAM - 模型权重** | ~150 KB (WakeNet+NSNet+VADNet) | 0 KB | **-150 KB** |
| **PSRAM - AFE pipeline** | ~80 KB (AFE_ALLOC_MORE_PSRAM) | ~30 KB (speexdsp buffers) | **-50 KB** |
| **SRAM - 任务栈** | ~20 KB (AFE内部任务+encode_ww) | ~10 KB (OpenAfeWakeWord task) | **-10 KB** |
| **SRAM - 运行时heap** | ~20 KB (AFE管理结构) | ~8 KB | **-12 KB** |
| **Flash - 模型文件** | ~500 KB (srmodel partition) | 0 KB | **-500 KB** |

> 开源方案可节省约 **220 KB PSRAM** 和 **22 KB SRAM**。

### 5.3 CPU 占用对比

| 场景 | 闭源 AFE | 开源方案 |
|------|---------|---------|
| **唤醒词检测（持续）** | WakeNet 每 30ms 推理一次，Core 0/1 约 15-25% | libfvad 每 10ms 一帧，CPU < 2% |
| **AEC（通话中）** | SR_HIGH_PERF: 高计算量 | speexdsp 50ms filter: 低计算量 |
| **NS（通话中）** | NSNet 推理: 中-高 | speexdsp preprocess: 低 |
| **综合通话 CPU** | 约 40-60% | 约 10-20% |

> 开源方案 CPU 占用显著更低，与视频流并发的竞争压力更小。

### 5.4 与视频流并发的兼容性

| 维度 | 闭源 AFE | 开源方案 |
|------|---------|---------|
| AFE内部任务优先级 | 1（最低，极易被抢占）| 无独立任务（用调用者任务） |
| 与摄像头 CPU 竞争 | 严重（已验证 ringbuffer 溢出）| 轻微（无低优先级内部任务） |
| SRAM 裕量（视频+语音）| 6 KB（危险）| 预计 ~28 KB（安全）|
| 视频+语音真正并发 | 不可行 | **理论上可行**（需验证）|

### 5.5 功能完整性对比

| 功能 | 闭源 AFE | 开源方案 |
|------|---------|---------|
| 唤醒词识别 | ✅ 高准确率 (WakeNet9) | ⚠️ 仅 VAD (Phase 8 待完成) |
| 多麦克风波束成形 | ✅ 内置 | ❌ 不支持 |
| AEC 效果 | ✅ 优秀 | ✅ 良好 (50ms filter) |
| NS 效果 | ✅ 优秀 (NSNet) | ✅ 良好 (-15dB speexdsp) |
| 跨平台可移植 | ❌ ESP32 专用 | ✅ 标准 C 库 |
| 授权 | ESP IDF 商业授权 | MIT/BSD 开源 |

---

## 六、最终处理结果（2026-05-22）

### 实施决策：禁用视频流

经过多轮尝试（调整 AFE 优先级、限制暂停状态、禁用说话期间唤醒词检测），视频和语音的 SRAM 竞争问题无法在保持视频开启的前提下彻底解决。

**最终决定：禁用视频流（移除 `InitializeStreaming()` 调用），优先保证语音功能稳定。**

| 功能 | 状态 | 说明 |
|------|------|------|
| 语音唤醒 + 多轮对话 | ✅ 正常 | 已验证稳定运行 |
| 视频流 (LocalStream HTTP) | ❌ 禁用 | 与语音内存冲突，暂时关闭 |
| 拍照 (`take_photo` MCP) | ⚠️ 修复中 | HTTP 超时导致 SRAM 耗尽崩溃，已缩短超时 |

### 已合入修改

| 文件 | 修改内容 |
|------|---------|
| `camera_xiaozhi_board.cc` | 移除 `InitializeStreaming()` 调用；扩展 pause/resume 到所有语音状态；`stream_port_` 成员变量化 |
| `application.cc` | 说话状态禁用唤醒词检测，减少 PSRAM 总线争用 |
| `local_stream_server.cc` | `Start()` 添加幂等保护（重复调用安全） |
| `esp32_camera.cc` | HTTP 超时从 30s 缩短至 10s，防止拍照超时耗尽 SRAM |

### `take_photo` 崩溃根因

崩溃链：HTTP POST 超时（30s × 2 次 = 60s）→ TCP 任务保持 SRAM → I2S DMA 重分配失败 → `LoadProhibited` panic

修复：`http->SetTimeout(10000)` 将单次等待上限缩短为 10s，服务器不可达时最多阻塞 ~20s 而非 60s，避免 SRAM 长时间被 TCP 任务占用。

---

## 七、结论与建议

### 当前状态
- **视频已禁用，语音稳定**。这是在 19 KB 空余 SRAM 约束下的务实选择。

### 如需重新启用视频
1. **切换到开源音频方案（Phase 7）**：节省约 150 KB PSRAM + 22 KB SRAM，无低优先级内部任务，是在 ESP32-S3 有限资源上实现视频+语音并发的最现实路径。
2. Phase 8（TFLite Micro KWS）补齐唤醒词能力后，开源方案可完整替代闭源 AFE，届时重新评估视频流可行性。

---

*报告生成日期：2026-05-21 / 最终更新：2026-05-22*
