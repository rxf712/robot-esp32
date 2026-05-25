# 开源替代库选型

## 选型原则

1. **许可证**：BSD / MIT / Apache 2.0，允许商业使用，不要求开源衍生品
2. **ESP32 可行性**：能在 IDF 5.x 体系下编译，内存需求不超出硬件限制
3. **跨平台**：纯 C/C++ 实现，无平台专属汇编（或有可关闭的 SIMD 选项）
4. **维护状态**：有持续维护，或功能已稳定不需维护（如 WebRTC VAD）

---

## 各模块选型

### 1. Opus 编解码

**需求**：替换 `libesp_audio_codec.a` 的 Opus 编码器和解码器。

| 方案 | 许可证 | ROM | RAM | ESP32 可行性 | 备注 |
|------|--------|-----|-----|------------|------|
| **libopus（推荐）** | BSD-3-Clause | ~200 KB | ~30 KB | ✅ 成熟 | Opus 官方参考实现，已有多个 ESP32 移植 |
| opusfile | BSD-3-Clause | 额外 ~80 KB | — | ✅ | 含 OGG 解封装，按需添加 |

**选型结论**：使用官方 libopus（https://github.com/xiph/opus）。

ESP32 注意事项：
- 关闭 `OPUS_FIXED_POINT`（对应 int 运算）或保持浮点；ESP32-S3 有 FPU，浮点可用
- 建议 complexity=5（平衡性能与质量）
- 复杂度 10 时 CPU 占用约 15-20%（ESP32-S3 @ 240 MHz，Opus 20ms 帧）

**组件引入方式**：
```yaml
# idf_component.yml
dependencies:
  idf: ">=5.0"
  # Option A: 社区组件（需验证版本）
  # espressif/libopus: "*"
  # Option B: 本地 components/libopus/（推荐，可控版本）
```

---

### 2. 采样率转换

**需求**：替换 `libesp_audio_effects.a` 的 `esp_ae_rate_cvt`，用于将麦克风采样率转为 16kHz。

| 方案 | 许可证 | ROM | RAM | ESP32 可行性 | 备注 |
|------|--------|-----|-----|------------|------|
| **libspeexdsp resampler（推荐）** | BSD-3-Clause | ~50 KB | ~15 KB | ✅ 已有嵌入式案例 | 仅需 `resample.c`，质量可调（0-10） |
| soxr | LGPL-2.1 | ~200 KB | 较大 | ⚠️ 许可证问题 | 高质量但 LGPL 有限制 |
| 自实现线性插值 | 自有 | ~2 KB | 极小 | ✅ | 质量低，仅适合 ±10% 误差场景 |

**选型结论**：使用 libspeexdsp 中的 `speex_resampler_*` 接口（https://github.com/xiph/speexdsp）。

关键参数：`quality=3`（ESP32 推荐，CPU 开销低，质量足够）。

---

### 3. VAD（语音活动检测）

**需求**：替换 `libvadnet.a`，实现 VAD_SILENCE/VAD_SPEECH 状态输出。

| 方案 | 许可证 | ROM | RAM | ESP32 可行性 | 备注 |
|------|--------|-----|-----|------------|------|
| **libfvad（推荐）** | BSD-3-Clause | ~20 KB | ~10 KB | ✅ 极轻量 | WebRTC VAD 的纯 C 移植，零依赖 |
| silero-vad（int8 量化） | MIT | ~500 KB（模型） | ~200 KB | ⚠️ PSRAM 可用 | 效果更好但重量级 |
| speexdsp VAD（preprocess） | BSD | ~80 KB | ~30 KB | ✅ | 作为 NS 的副产物可同时获得 VAD |

**选型结论**：使用 libfvad（https://github.com/dpirch/libfvad）。

原因：
- 仅需 `src/fvad.c` + 5 个内部源文件，总计约 800 行代码
- WebRTC VAD 经过大量生产验证，准确性可接受
- 支持 10/20/30 ms 帧、16kHz（与当前系统完全匹配）
- 模式 0-3 对应 `VAD_MODE_0-3`，语义一致

---

### 4. NS（噪声抑制）

**需求**：替换 `libnsnet.a`，对麦克风音频进行降噪。

| 方案 | 许可证 | ROM | RAM | ESP32 可行性 | 备注 |
|------|--------|-----|-----|------------|------|
| **speex_preprocess NS（推荐）** | BSD-3-Clause | ~80 KB | ~30 KB | ✅ | speexdsp 的频域降噪 |
| RNNoise | BSD-3-Clause | ~200 KB（模型） | ~100 KB | ⚠️ PSRAM 可用 | 神经网络 NS，效果接近 nsnet |
| WebRTC NS（libwebrtc_ns） | BSD | ~60 KB | ~20 KB | ✅ | 纯 C，效果中等 |

**选型结论**：speex_preprocess NS 作为主选，RNNoise 作为进阶选项。

speex_preprocess NS 注意事项：
- 使用 `SPEEX_PREPROCESS_SET_NOISE_SUPPRESS`，推荐 -20dB 至 -30dB
- 同时可开启 AGC（`SPEEX_PREPROCESS_SET_AGC`）替代 `libvadnet.a` 中的 AGC 功能
- 与 speex AEC 配合时，NS 输入应为 AEC 处理后的信号

**关于 RNNoise（进阶，Phase 5 可选）**：
- 模型文件约 175KB，运行时需约 100KB PSRAM
- 效果与 nsnet 相近，推荐在 PSRAM 富余时启用
- 源码：https://github.com/xiph/rnnoise

---

### 5. AEC（声学回声消除）

**需求**：替换 `libesp_audio_front_end.a` 的 AEC 部分，消除扬声器播放音对麦克风的干扰。

| 方案 | 许可证 | ROM | RAM | ESP32 可行性 | 备注 |
|------|--------|-----|-----|------------|------|
| **speex_echo（推荐）** | BSD-3-Clause | ~100 KB | ~100 KB | ✅ | 自适应 NLMS，需参考信号 |
| WebRTC AEC3 | BSD | ~500 KB | ~300 KB | ⚠️ 偏重 | 效果最佳但资源需求高 |
| WebRTC AEC（旧版） | BSD | ~200 KB | ~150 KB | ⚠️ PSRAM 可用 | 比 speex 效果好，资源居中 |

**选型结论**：speex_echo（`speex_echo_state_init` / `speex_echo_cancellation`）。

关键约束：
- `mic_data` 与 `ref_data` 必须**精确时间对齐**（同一帧时间戳）
- filter_length 建议设为 `sample_rate × 0.15`（150ms 回声尾）= 2400 samples
- 需要 camera-xiaozhi 板的 `input_reference()` 返回 true（参考信号通道存在）
- frame_size 与 NS 保持一致（建议 320 samples = 20ms @16kHz）

**进阶替换路径**：
若 speex AEC 效果不满足要求，可替换为 WebRTC AEC（older version）。
两者接口差异较大，但适配层封装后对外接口一致。

---

### 6. 唤醒词检测

**需求**：替换 `libwakenet.a`，检测特定唤醒词（如"你好小智"）。

这是整个迁移中**风险最高**的部分，因为需要针对特定唤醒词训练或获取模型。

| 方案 | 许可证 | ROM | RAM | ESP32 可行性 | 备注 |
|------|--------|-----|-----|------------|------|
| **VAD + 按键触发（初期）** | N/A | 极小 | 极小 | ✅ | 不需要唤醒词，按键激活，作为过渡方案 |
| TFLite Micro + KWS 模型 | Apache 2.0 | ~500 KB | ~200 KB PSRAM | ✅ | 需要训练/获取模型，长期方案 |
| Porcupine 免费版 | 商业（免费层） | ~300 KB | ~50 KB | ✅ | 有 ESP32 SDK，非完全开源 |
| openWakeWord | Apache 2.0 | ~2 MB（模型） | 偏大 | ❌ 过重 | 依赖 Python runtime，不适合 MCU |

**选型结论**：两阶段方案

**阶段一（过渡，立即可用）**：
- `OpenAfeWakeWord` 初期实现：硬件按键长按触发 + VAD 检测语音开始
- 效果：取消免唤醒词功能，改为手动触发，保留完整通话功能

**阶段二（长期，独立推进）**：
- `espressif/esp-tflite-micro` 组件 + ARM KWS 参考模型
- 训练步骤参考 https://github.com/ARM-software/ML-zoo（keyword spotting）
- 输入特征：MFCC（40维，25ms 窗口，10ms 步进，16kHz）
- 模型格式：`.tflite`，量化到 int8 以减小体积

---

### 7. JPEG 编解码

**需求**：替换 `libesp_new_jpeg.a`，用于摄像头图像的 JPEG 编码。

| 方案 | 许可证 | ROM | RAM | ESP32 可行性 | 备注 |
|------|--------|-----|-----|------------|------|
| **esp_driver_jpeg（ESP32-S3 阶段，推荐）** | Apache 2.0 | 0（IDF 内置） | 硬件加速 | ✅ | IDF 5.x 内置，硬件 JPEG 加速 |
| libjpeg-turbo（跨平台阶段，推荐） | BSD | ~300 KB | ~50 KB | ✅ | 跨平台标准，纯 C 实现 |
| TinyJPEG | 公有领域 | ~20 KB | 极小 | ✅ | 编码质量较低，仅编码 |

**选型结论**：两阶段方案
- **ESP32-S3 阶段**：改用 `esp_driver_jpeg`（ESP-IDF 内置，已是开源，硬件 JPEG 加速，零额外依赖）
- **跨平台阶段**：通过 `#ifdef` 切换到 libjpeg-turbo（https://github.com/libjpeg-turbo/libjpeg-turbo）

---

## 选型汇总表

| 模块 | 开源库 | 许可证 | ESP32 ROM | ESP32 RAM | 风险 | 是否需要模型文件 |
|------|--------|--------|-----------|-----------|------|----------------|
| Opus 编解码 | libopus | BSD-3 | ~200 KB | ~30 KB | 低 | 否 |
| 采样率转换 | libspeexdsp resampler | BSD-3 | ~50 KB | ~15 KB | 低 | 否 |
| VAD | libfvad | BSD-3 | ~20 KB | ~10 KB | 低 | 否 |
| NS | speex_preprocess | BSD-3 | ~80 KB | ~30 KB | 中 | 否 |
| AEC | speex_echo | BSD-3 | ~100 KB | ~100 KB | 中高 | 否 |
| 唤醒词（过渡） | 按键+VAD | N/A | 极小 | 极小 | 极低 | 否 |
| 唤醒词（正式） | TFLite Micro + KWS | Apache 2.0 | ~500 KB | ~200 KB | 高 | 是（.tflite） |
| JPEG（ESP32） | esp_driver_jpeg | Apache 2.0 | 0（内置） | 硬件 | 极低 | 否 |
| JPEG（跨平台） | libjpeg-turbo | BSD | ~300 KB | ~50 KB | 低 | 否 |

---

## 总资源估算（Phase 7 完成后，不含唤醒词 TFLite）

- **额外 ROM**：约 450 KB（libopus 200 + speexdsp 230）
- **额外 RAM**：约 185 KB（建议分配到 PSRAM：AEC 100 + NS 30 + VAD 10 + Opus 30 + Resampler 15）
- **当前固件剩余空间**：28%（约 1.1 MB），添加后仍有余量

> ESP32-S3 PSRAM 8MB，当前使用量主要为 LVGL 帧缓冲和 AFE 缓冲，新增 185KB 完全可容纳。

---

## 依赖关系图

```
components/
├── libopus/              ← Phase 1 引入
├── libspeexdsp/          ← Phase 2/5/6 引入（resampler + NS + AEC 共用）
└── libfvad/              ← Phase 4 引入

main/audio/
├── codecs/
│   ├── open_opus_codec.h/cc      ← Phase 1：libopus 适配器
│   └── open_resampler.h/cc       ← Phase 2：speexdsp resampler 适配器
└── processors/
    ├── open_vad.h/cc              ← Phase 4：libfvad 适配器
    ├── open_ns.h/cc               ← Phase 5：speex_preprocess 适配器
    ├── open_aec.h/cc              ← Phase 6：speex_echo 适配器
    └── open_afe_audio_processor.h/cc ← Phase 7：组合适配器
main/audio/wake_words/
    └── open_afe_wake_word.h/cc    ← Phase 7/8：按键+VAD 或 TFLite KWS
```
