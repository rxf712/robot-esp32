# 替换路线图（分阶段实施计划）

## 实施原则

- **适配器模式**：新建 `open_*` 类，对外暴露与原类相同的公共接口，通过 Kconfig 开关切换
- **不破坏原有代码**：原 `afe_audio_processor.cc` 等文件保持不动，作为对照参考
- **逐 Phase 验证**：每个 Phase 完成后独立验证，不堆叠未验证的改动

**Kconfig 主控开关**（添加到 `Kconfig.projbuild`）：
```kconfig
config USE_OPEN_SOURCE_AUDIO
    bool "Use open-source audio processing"
    default n
    help
        Replace proprietary esp-sr / esp_audio_codec / esp_audio_effects
        with open-source equivalents (libopus, speexdsp, libfvad).
        Required for non-ESP32 platform porting.
```

---

## Phase 0：组件准备 & 编译验证

**目标**：确认所有开源库能在 IDF 5.5.2 + ESP32-S3 下独立编译，建立目录骨架。

### 0.1 创建 components 目录结构

```bash
mkdir -p components/libopus
mkdir -p components/libspeexdsp
mkdir -p components/libfvad
```

### 0.2 引入 libopus

```bash
# 方式 A：git submodule（推荐，锁定版本）
git submodule add https://github.com/xiph/opus.git components/libopus/opus
# 方式 B：直接下载解压
```

创建 `components/libopus/CMakeLists.txt`：
```cmake
cmake_minimum_required(VERSION 3.16)
set(OPUS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/opus")
# 收集 opus 核心源文件（silk + celt + src）
file(GLOB_RECURSE OPUS_SRCS
    "${OPUS_SRC_DIR}/src/*.c"
    "${OPUS_SRC_DIR}/celt/*.c"
    "${OPUS_SRC_DIR}/silk/*.c"
    "${OPUS_SRC_DIR}/silk/float/*.c"
)
idf_component_register(
    SRCS ${OPUS_SRCS}
    INCLUDE_DIRS "${OPUS_SRC_DIR}/include" "${OPUS_SRC_DIR}"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD HAVE_LRINTF USE_ALLOCA
)
```

### 0.3 引入 libspeexdsp（resampler + NS + AEC 共用）

```bash
git submodule add https://github.com/xiph/speexdsp.git components/libspeexdsp/speexdsp
```

创建 `components/libspeexdsp/CMakeLists.txt`：
```cmake
set(SPEEX_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/speexdsp")
idf_component_register(
    SRCS
        "${SPEEX_SRC_DIR}/libspeexdsp/resample.c"
        "${SPEEX_SRC_DIR}/libspeexdsp/preprocess.c"
        "${SPEEX_SRC_DIR}/libspeexdsp/echo.c"
        "${SPEEX_SRC_DIR}/libspeexdsp/fftwrap.c"
        "${SPEEX_SRC_DIR}/libspeexdsp/filterbank.c"
        "${SPEEX_SRC_DIR}/libspeexdsp/mdf.c"
        "${SPEEX_SRC_DIR}/libspeexdsp/smallft.c"
        "${SPEEX_SRC_DIR}/libspeexdsp/vorbis_psy.c"
    INCLUDE_DIRS "${SPEEX_SRC_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}"
)
# speex_config.h 需要提供（在 components/libspeexdsp/ 下创建）
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H FLOATING_POINT EXPORT=
)
```

创建 `components/libspeexdsp/config.h`（speexdsp 编译所需）：
```c
#define FLOATING_POINT
#define USE_SMALLFT
#define EXPORT
```

### 0.4 引入 libfvad

```bash
git submodule add https://github.com/dpirch/libfvad.git components/libfvad/libfvad
```

创建 `components/libfvad/CMakeLists.txt`：
```cmake
set(FVAD_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libfvad")
idf_component_register(
    SRCS
        "${FVAD_SRC_DIR}/src/fvad.c"
        "${FVAD_SRC_DIR}/src/signal_processing/division_operations.c"
        "${FVAD_SRC_DIR}/src/signal_processing/energy.c"
        "${FVAD_SRC_DIR}/src/signal_processing/get_scaling_square.c"
        "${FVAD_SRC_DIR}/src/signal_processing/resample_48khz.c"
        "${FVAD_SRC_DIR}/src/signal_processing/resample_by_2_internal.c"
        "${FVAD_SRC_DIR}/src/signal_processing/resample_fractional.c"
        "${FVAD_SRC_DIR}/src/signal_processing/spl_inl.c"
        "${FVAD_SRC_DIR}/src/vad/vad_core.c"
        "${FVAD_SRC_DIR}/src/vad/vad_filterbank.c"
        "${FVAD_SRC_DIR}/src/vad/vad_gmm.c"
        "${FVAD_SRC_DIR}/src/vad/vad_sp.c"
    INCLUDE_DIRS "${FVAD_SRC_DIR}/include" "${FVAD_SRC_DIR}/src"
)
```

### 0.5 验证

```bash
# 单独编译验证（不涉及音频逻辑）
idf.py build 2>&1 | grep -E "error:|warning:" | head -20
```

**通过标准**：三个组件无编译错误（警告可接受）。

---

## Phase 1：Opus 编解码替换

**风险**：低 | **受影响库**：`libesp_audio_codec.a`, `libesp_audio_simple_dec.a`

### 受影响文件

| 文件 | 修改内容 |
|------|---------|
| `main/audio/audio_service.h` | 添加条件编译，引入 `open_opus_codec.h` |
| `main/audio/audio_service.cc` | 替换编码器/解码器初始化和调用 |
| `main/audio/wake_words/afe_wake_word.cc` | 替换 `EncodeWakeWordData` 中的编码调用 |
| `main/audio/wake_words/custom_wake_word.cc` | 同上 |
| `main/audio/codecs/open_opus_codec.h/cc` | **新建**：libopus 适配器 |
| `main/CMakeLists.txt` | 按 Kconfig 引入新源文件 |

### 实施步骤

**1.1** 创建适配器头文件 `main/audio/codecs/open_opus_codec.h`：

```cpp
#pragma once
#include <cstdint>
#include "esp_audio_err.h"   // 复用错误码类型（或自定义等价类型）

// 与 esp_opus_enc_config_t 等价的配置结构
typedef struct {
    int   sample_rate;
    int   channel;
    int   bits_per_sample;
    int   bitrate;
    int   frame_duration_ms;
    int   complexity;
} open_opus_enc_config_t;

typedef struct {
    int   sample_rate;
    int   channel;
    int   frame_duration_ms;
} open_opus_dec_config_t;

typedef struct { uint8_t *buffer; uint32_t len; } open_audio_in_frame_t;
typedef struct { uint8_t *buffer; uint32_t len; uint32_t encoded_bytes; } open_audio_out_frame_t;

int open_opus_enc_open(open_opus_enc_config_t *cfg, void **enc_hd);
int open_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);
int open_opus_enc_process(void *enc_hd, open_audio_in_frame_t *in, open_audio_out_frame_t *out);
void open_opus_enc_close(void *enc_hd);

int open_opus_dec_open(open_opus_dec_config_t *cfg, void **dec_hd);
int open_opus_dec_process(void *dec_hd, open_audio_in_frame_t *in, open_audio_out_frame_t *out);
void open_opus_dec_close(void *dec_hd);
```

**1.2** 实现 `main/audio/codecs/open_opus_codec.cc`（内部使用 libopus API，见 `04-api-mapping.md`）

**1.3** 修��� `main/audio/audio_service.h`：

```cpp
#ifdef CONFIG_USE_OPEN_SOURCE_AUDIO
#include "codecs/open_opus_codec.h"
// 重定义宏使 audio_service.cc 无需感知差异
#define AS_OPUS_ENC_CONFIG() open_opus_enc_config_t{ \
    .sample_rate=16000, .channel=1, .bits_per_sample=16, \
    .bitrate=24000, .frame_duration_ms=20, .complexity=5 }
#else
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#endif
```

**1.4** 在 `main/CMakeLists.txt` 的 camera-xiaozhi 配置段添加：

```cmake
if(CONFIG_USE_OPEN_SOURCE_AUDIO)
    list(APPEND SOURCES "audio/codecs/open_opus_codec.cc")
    list(APPEND MAIN_PRIV_REQUIRES_EXTRA libopus libspeexdsp libfvad)
endif()
```

### 验证

- `idf.py build` 无错误
- 开启 `CONFIG_USE_OPEN_SOURCE_AUDIO=y`，烧录，实际通话
- ASR 服务器能正确识别上传的语音（功能等价）
- 唤醒词录音上传后服务器响应正常

---

## Phase 2：采样率转换替换

**风险**：低 | **受影响库**：`libesp_audio_effects.a`（仅 rate_cvt 部分）

### 受影响文件

| 文件 | 修改内容 |
|------|---------|
| `main/audio/audio_service.h` | 替换 `esp_ae_rate_cvt_handle_t` 声明 |
| `main/audio/audio_service.cc` | 替换 resampler 初始化和调用（第 87-93 行） |
| `main/audio/codecs/open_resampler.h/cc` | **新建**：speexdsp resampler 适配器 |

### 实施步骤

**2.1** 创建 `main/audio/codecs/open_resampler.h`：

```cpp
#pragma once
#include <cstdint>

typedef void* open_resampler_handle_t;

int  open_resampler_open(uint32_t src_rate, uint32_t dest_rate,
                         int channels, open_resampler_handle_t *handle);
int  open_resampler_process(open_resampler_handle_t handle,
                            const int16_t *in, uint32_t in_num,
                            int16_t *out, uint32_t *out_num);
uint32_t open_resampler_get_max_out_samples(open_resampler_handle_t handle, uint32_t in_num);
void open_resampler_close(open_resampler_handle_t handle);
```

**2.2** 实现（内部调用 `speex_resampler_init` / `speex_resampler_process_int`，见 `04-api-mapping.md`）

**2.3** 修改 `audio_service.cc/h`：

```cpp
#ifdef CONFIG_USE_OPEN_SOURCE_AUDIO
#include "codecs/open_resampler.h"
using RateCvtHandle = open_resampler_handle_t;
#define RATE_CVT_OPEN(src, dst, ch, hd)   open_resampler_open(src, dst, ch, hd)
#define RATE_CVT_PROCESS(h, in, in_n, out, out_n)  open_resampler_process(h, in, in_n, out, out_n)
#define RATE_CVT_CLOSE(h)                  open_resampler_close(h)
#else
// 原有宏
#endif
```

### 验证

- 麦克风采样率不为 16kHz 时，通话语音 ASR 识别率正常

---

## Phase 3：JPEG 库替换

**风险**：极低 | **受影响库**：`libesp_new_jpeg.a`

### 受影响文件

| 文件 | 修改内容 |
|------|---------|
| `main/display/lvgl_display/jpg/image_to_jpeg.cpp` | 替换 `jpeg_enc_*` 调用 |
| `main/idf_component.yml` | 删除 `espressif/esp_new_jpeg`，改用内置 `esp_driver_jpeg` |

### 实施步骤

**3.1** 先读取 `main/display/lvgl_display/jpg/image_to_jpeg.cpp` 全文，确认所有 `jpeg_enc_*` 调用点

**3.2** 用 `esp_driver_jpeg` API（IDF 5.x 内置）重写编码部分

```cpp
// 替换前：esp_new_jpeg
jpeg_enc_config_t cfg = { .width=w, .height=h, .src_type=JPEG_PIXEL_FORMAT_RGB888, .quality=80 };
jpeg_enc_open(&cfg, &enc);
jpeg_enc_process(enc, in, in_size, out, out_size, &actual);
jpeg_enc_close(enc);

// 替换后：esp_driver_jpeg（硬件 JPEG，IDF 内置）
// 参考 IDF 文档：peripherals/jpeg.html
jpeg_encode_cfg_t enc_cfg = {
    .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
    .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
    .image_quality = 80,
    .width = w, .height = h,
};
jpeg_new_encoder_engine(&enc_cfg, &enc_engine);
jpeg_encoder_process(enc_engine, &enc_input_cfg, timeout);
jpeg_del_encoder_engine(enc_engine);
```

**3.3** `idf_component.yml` 中：
- 删除 `espressif/esp_new_jpeg: ^0.6.1`
- `esp_driver_jpeg` 是 ESP-IDF 内置组件，无需额外声明

### 验证

- 摄像头采集图像后正确编码为 JPEG 并通过 WebSocket 发送
- 接收端画面清晰，无明显色偏或损坏

---

## Phase 4：VAD 替换（libfvad）

**风险**：低 | **受影响库**：`libvadnet.a`（经 AFE 内部使用）

此阶段不替换整个 AFE，仅建立 VAD 适配类，供 Phase 7 使用。

### 新增文件

**`main/audio/processors/open_vad.h`**：
```cpp
#pragma once
#include <cstdint>

typedef enum { OPEN_VAD_SILENCE = 0, OPEN_VAD_SPEECH = 1 } open_vad_state_t;

class OpenVad {
public:
    OpenVad(int mode, int min_speech_ms, int min_silence_ms, int sample_rate = 16000);
    ~OpenVad();
    open_vad_state_t Process(const int16_t *pcm, int samples);
private:
    struct Fvad *vad_;
    int speech_count_  = 0;
    int silence_count_ = 0;
    int min_speech_frames_;
    int min_silence_frames_;
};
```

### 实施步骤

**4.1** 实现 `main/audio/processors/open_vad.cc`（内部调用 `fvad_new/fvad_process`，见 `04-api-mapping.md`）

**4.2** 编写单元测试（可选）：输入预录 16kHz PCM，验证 VAD 状态切换时机

### 验证（与 Phase 7 联合）

- 对着麦克风说话，日志打印 VAD SPEECH 状态
- 静音时不误触发（静音 1s 内不出现 SPEECH）
- 延迟 ≤ 200ms（从开始说话到 VAD 触发）

---

## Phase 5：NS 替换（speex_preprocess）

**风险**：中 | **受影响库**：`libnsnet.a`（经 AFE 内部使用）

此阶段建立 NS 适配类，供 Phase 7 使用。

### 新增文件

**`main/audio/processors/open_ns.h`**：
```cpp
#pragma once
#include <cstdint>

class OpenNs {
public:
    OpenNs(int frame_size, int sample_rate, int suppress_db = -25);
    ~OpenNs();
    void Process(int16_t *pcm);  // in-place 处理
private:
    struct SpeexPreprocessState_ *state_;
};
```

### 实施步骤

**5.1** 实现 `main/audio/processors/open_ns.cc`（调用 `speex_preprocess_state_init/run`，见 `04-api-mapping.md`）

**5.2** 调参指导：
- `suppress_db = -20`：温和降噪（对语音影响小）
- `suppress_db = -30`：激进降噪（可能影响语音音质）
- 建议先用 `-20`，根据实测调整

### 验证

- 噪声环境下，录音并上传，ASR 识别率与原 nsnet 相当
- 主观听感：背景噪声明显减弱，语音清晰度无明显下降

---

## Phase 6：AEC 替换（speex_echo）

**风险**：中高 | **受影响库**：`libesp_audio_front_end.a` 的 AEC 部分

### 前提条件

- camera-xiaozhi 板的 `input_reference()` 返回 true（需验证硬件层支持参考信号通道）
- 音频 codec 驱动正确采集参考信号（扬声器输出回采）

### 新增文件

**`main/audio/processors/open_aec.h`**：
```cpp
#pragma once
#include <cstdint>

class OpenAec {
public:
    // frame_size: 采样数（建议 320 = 20ms@16kHz）
    // filter_length: 建议 2400（150ms 回声尾）
    OpenAec(int frame_size, int filter_length, int sample_rate = 16000);
    ~OpenAec();
    // mic_in/ref_in 必须时间对齐，frame_size 采样数
    void Process(const int16_t *mic_in, const int16_t *ref_in, int16_t *out);
private:
    struct SpeexEchoState_ *echo_;
};
```

### 实施步骤

**6.1** 实现 `main/audio/processors/open_aec.cc`（调用 `speex_echo_state_init/cancellation`，见 `04-api-mapping.md`）

**6.2** 时间对齐验证：确保 `mic_in` 和 `ref_in` 来自同一帧时间戳

**6.3** 参数调优流程：
1. 初始值：`frame_size=320`，`filter_length=2400`
2. 播放 TTS 同时录音，听录音中是否有回声
3. 若回声未完全消除：增大 `filter_length`（最大 4800）
4. 若语音被过度消除：检查时间对齐问题

### 验证

- TTS 播放期间，麦克风录音中不存在明显回声
- 用 `audio_debugger.cc` 录制 AEC 前后波形，频谱对比

---

## Phase 7：开源 AFE 流水线集成

**风险**：中 | **目标**：整合 Phase 4/5/6，创建完整的 `OpenAfeAudioProcessor` 和 `OpenAfeWakeWord`

### 架构

```
当前实现（黑盒）：
  mic → [libesp_audio_front_end.a] → clean PCM + vad_state + wakeup_state
               ↑（AEC + NS + VAD + WakeNet 一体化）

新实现（透明流水线）：
  mic ──────────────────────────────────┐
  ref → [OpenAEC] → [OpenNS] → [OpenVAD] → clean PCM + vad_state
                                        └─ [OpenWakeWord] → wakeup detected
```

### 新增文件

**`main/audio/processors/open_afe_audio_processor.h`**：

```cpp
#pragma once
#include "audio_processor.h"   // 基类接口（与 AfeAudioProcessor 相同）
#include "open_aec.h"
#include "open_ns.h"
#include "open_vad.h"
#include <memory>
#include <functional>
#include <vector>

class OpenAfeAudioProcessor {
public:
    OpenAfeAudioProcessor();
    ~OpenAfeAudioProcessor();

    // 与 AfeAudioProcessor 完全相同的公共接口
    void Initialize(AudioCodec *codec, int frame_duration_ms, void *models = nullptr);
    void Feed(std::vector<int16_t> &&data);
    void Start();
    void Stop();
    bool IsRunning();
    size_t GetFeedSize();
    void OnOutput(std::function<void(std::vector<int16_t> &&)> callback);
    void OnVadStateChange(std::function<void(bool speaking)> callback);
    void EnableDeviceAec(bool enable);

private:
    std::unique_ptr<OpenAec> aec_;
    std::unique_ptr<OpenNs>  ns_;
    std::unique_ptr<OpenVad> vad_;
    // FreeRTOS 任务和事件组
    void ProcessingTask();
};
```

**`main/audio/wake_words/open_afe_wake_word.h`**（初期：VAD + 按键触发，不依赖 WakeNet）：

```cpp
#pragma once
#include "wake_word.h"   // 基类接口
#include <functional>
#include <string>

class OpenAfeWakeWord {
public:
    bool Initialize(AudioCodec *codec, void *models = nullptr);
    void Feed(const std::vector<int16_t> &data);
    void Start();
    void Stop();
    size_t GetFeedSize();
    void OnWakeWordDetected(std::function<void(const std::string&)> callback);
    // 供外部（按键驱动）主动触发唤醒
    void TriggerWakeWord(const std::string &word = "manual_trigger");
private:
    std::function<void(const std::string&)> wake_word_detected_callback_;
    // 初期实现：VAD 连续语音检测触发
};
```

### 修改 main/CMakeLists.txt

```cmake
if(CONFIG_USE_OPEN_SOURCE_AUDIO)
    list(APPEND SOURCES
        "audio/codecs/open_opus_codec.cc"
        "audio/codecs/open_resampler.cc"
        "audio/processors/open_vad.cc"
        "audio/processors/open_ns.cc"
        "audio/processors/open_aec.cc"
        "audio/processors/open_afe_audio_processor.cc"
        "audio/wake_words/open_afe_wake_word.cc"
    )
    list(APPEND MAIN_PRIV_REQUIRES_EXTRA libopus libspeexdsp libfvad)
else()
    list(APPEND SOURCES
        "audio/processors/afe_audio_processor.cc"
        "audio/wake_words/afe_wake_word.cc"
    )
endif()
```

### 修改 audio_service.cc 的条件编译

```cpp
#if CONFIG_USE_OPEN_SOURCE_AUDIO
#include "processors/open_afe_audio_processor.h"
#include "wake_words/open_afe_wake_word.h"
using AudioProcessor = OpenAfeAudioProcessor;
using WakeWord = OpenAfeWakeWord;
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "processors/afe_audio_processor.h"
#include "wake_words/afe_wake_word.h"
using AudioProcessor = AfeAudioProcessor;
using WakeWord = AfeWakeWord;
#else
// ...
#endif
```

### 验证

- 开启 `CONFIG_USE_OPEN_SOURCE_AUDIO=y`，烧录
- 长按按键触发对话，完整流程（语音→ASR→LLM→TTS）正常
- VAD 状态切换在日志中可观察到
- `idf_component.yml` 中 `espressif/esp-sr` 组件可移除，编译通过

---

## Phase 8：唤醒词替换（TFLite Micro）

**风险**：高 | **可独立并行**，不阻塞 Phase 9

### 阶段划分

**8a（过渡，Phase 7 已完成）**：
- `OpenAfeWakeWord::TriggerWakeWord()` 由按键驱动调用
- 无需说唤醒词，改为手动触发

**8b（正式，长期推进）**：

**8b.1** 引入 `espressif/esp-tflite-micro` 组件：
```yaml
dependencies:
  espressif/esp-tflite-micro: ">=1.0.0"
```

**8b.2** 准备 KWS 模型（独立 ML 任务）：
- 参考 https://github.com/ARM-software/ML-zoo/tree/main/models/keyword_spotting
- 使用 TensorFlow/Edge Impulse 训练目标唤醒词（如"你好小智"的音频特征）
- 量化为 int8 .tflite，大小目标 < 500KB

**8b.3** 实现 MFCC 特征提取（`open_mfcc.h/cc`）：
- 输入：16kHz PCM，25ms 窗口，10ms 步进
- 输出：40 维 MFCC 特征向量

**8b.4** 在 `OpenAfeWakeWord` 中集成推理：
```cpp
// 每 10ms 一帧，维护 1s 滑动窗口（100帧）
// 调用 TFLite Micro 推理，输出关键词置信度
// 超过阈值（0.85）时触发回调
```

### 验证

- 说目标唤醒词，设备正确触发（召回率目标 ≥ 80%）
- 不说唤醒词时不误触发（5min 静音无误唤醒）
- TFLite 推理延迟 < 50ms/帧

---

## Phase 9：跨平台抽象层

**风险**：中 | **前提**：Phase 1-7 全部完成

### 需要替换的 ESP32 专属依赖

| 当前 ESP32 API | 跨平台替换 | 文件位置 |
|--------------|---------|---------|
| `xTaskCreate` + `vTaskDelete` | `std::thread` | open_afe_audio_processor.cc |
| `xEventGroupCreate/Set/Wait` | `std::mutex` + `std::condition_variable` | open_afe_audio_processor.cc |
| `heap_caps_malloc(PSRAM)` | 普通 `new` / `malloc` | open_aec.cc, open_ns.cc |
| `ESP_LOGI/LOGE` | `printf` 或平台日志 | 所有 open_*.cc |
| `esp_timer_get_time()` | `std::chrono::steady_clock` | 如有时间戳需求 |
| `portMAX_DELAY` | `std::numeric_limits<int>::max()` 或等价 | open_afe_audio_processor.cc |
| `TickType_t` | `int` (ms) | 接口层 |

### 新增文件

**`main/platform/platform.h`**（平台抽象层）：

```cpp
#pragma once
#ifdef ESP_PLATFORM
  // ESP32 实现
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/event_groups.h"
  #include "esp_log.h"
  #include "esp_heap_caps.h"
  #define PLATFORM_LOGI(tag, ...) ESP_LOGI(tag, ##__VA_ARGS__)
  #define PLATFORM_LOGE(tag, ...) ESP_LOGE(tag, ##__VA_ARGS__)
  #define PLATFORM_MALLOC_PSRAM(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#else
  // 通用 C++ 实现
  #include <thread>
  #include <mutex>
  #include <condition_variable>
  #include <cstdio>
  #include <cstdlib>
  #define PLATFORM_LOGI(tag, fmt, ...) printf("[INFO][%s] " fmt "\n", tag, ##__VA_ARGS__)
  #define PLATFORM_LOGE(tag, fmt, ...) printf("[ERR ][%s] " fmt "\n", tag, ##__VA_ARGS__)
  #define PLATFORM_MALLOC_PSRAM(size) malloc(size)
#endif
```

### 实施步骤

**9.1** 将所有 `open_*.cc` 中的 ESP-IDF 专属调用替换为 `platform.h` 抽象

**9.2** 创建 Linux 平台 CMakeLists.txt（`CMakeLists_linux.txt`）：
```cmake
cmake_minimum_required(VERSION 3.16)
project(open_audio_test)
add_subdirectory(components/libopus)
add_subdirectory(components/libspeexdsp)
add_subdirectory(components/libfvad)
add_executable(audio_test tests/audio_pipeline_test.cc
    main/audio/processors/open_vad.cc
    main/audio/processors/open_ns.cc
    main/audio/processors/open_aec.cc
    main/audio/processors/open_afe_audio_processor.cc
)
```

**9.3** 编写测试程序 `tests/audio_pipeline_test.cc`：
- 读取预录 WAV 文件（带回声的麦克风 + 参考信号）
- 通过 AEC → NS → VAD 流水线处理
- 输出处理后的 PCM，用 sox 或 Audacity 主观验证效果

### 验证

```bash
cmake -B build_linux -f CMakeLists_linux.txt
make -C build_linux
./build_linux/audio_test test_data/mic_with_echo.wav test_data/ref.wav out.wav
# 用音频播放器听 out.wav，确认回声消除和降噪效果
```

---

## 里程碑总表

| Phase | 完成标志 | 可以移除的闭源依赖 |
|-------|---------|-----------------|
| Phase 0 | 三个开源组件无编译错误 | — |
| Phase 1 | 实际通话 ASR 正常 | `espressif/esp_audio_codec`（部分） |
| Phase 2 | 不同采样率通话正常 | `espressif/esp_audio_effects` |
| Phase 3 | 摄像头 JPEG 流正常 | `espressif/esp_new_jpeg` |
| Phase 4-6 | VAD/NS/AEC 独立可用 | — |
| Phase 7 | 完整通话正常，无 esp-sr | `espressif/esp-sr`，`espressif/esp_audio_codec` |
| Phase 8 | 说唤醒词触发对话 | — |
| Phase 9 | Linux 编译通过 + 测试通过 | — |
