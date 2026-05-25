# 闭源库依赖分析

## 1. 总览

工程中共有 8 个闭源预编译静态库，分布在 4 个 managed component 中，全部仅支持 ESP32 系列芯片。

```
managed_components/
├── espressif__esp-sr/lib/esp32s3/
│   ├── libesp_audio_front_end.a   ← AFE 框架 + AEC
│   ├── libesp_audio_processor.a   ← 音频处理基础层
│   ├── libwakenet.a               ← 唤醒词神经网络推理
│   ├── libnsnet.a                 ← 神经网络噪声抑制
│   ├── libvadnet.a                ← 神经网络 VAD
│   ├── libmultinet.a              ← 命令词识别（本项目未启用）
│   ├── libc_speech_features.a     ← 声学特征提取（MFCC 等）
│   ├── libdl_lib.a                ← 神经网络推理基础
│   ├── libfst.a                   ← TTS 辅助（本项目未用）
│   └── libhufzip.a                ← 压缩工具（本项目未用）
├── espressif__esp_audio_codec/lib/esp32s3/
│   ├── libesp_audio_codec.a       ← Opus 编解码器
│   └── libesp_audio_simple_dec.a  ← 轻量解码器
├── espressif__esp_audio_effects/lib/esp32s3/
│   └── libesp_audio_effects.a     ← 采样率转换、音效处理
└── espressif__esp_new_jpeg/lib/esp32s3/
    └── libesp_new_jpeg.a          ← JPEG 图像编解码
```

本项目 camera-xiaozhi 板实际用到的库（去除未启用模块）：

| 库文件 | 是否实际使用 | 直接调用文件 |
|--------|------------|------------|
| `libesp_audio_front_end.a` | ✅ | `afe_audio_processor.cc`, `afe_wake_word.cc` |
| `libwakenet.a` | ✅ | 经 `afe_wake_word.cc` 的 AFE 配置加载 |
| `libnsnet.a` | ✅ | 经 `afe_audio_processor.cc` 的 AFE 配置加载 |
| `libvadnet.a` | ✅ | 经 `afe_audio_processor.cc` 的 AFE 配置加载 |
| `libesp_audio_codec.a` | ✅ | `audio_service.cc`, `afe_wake_word.cc`, `custom_wake_word.cc` |
| `libesp_audio_simple_dec.a` | ✅ | `audio_service.cc`（Opus 解码） |
| `libesp_audio_effects.a` | ✅ | `audio_service.cc`（采样率转换） |
| `libesp_new_jpeg.a` | �� | `image_to_jpeg.cpp` |
| `libesp_audio_processor.a` | ✅（间接） | AFE 内部依赖 |
| `libc_speech_features.a` / `libdl_lib.a` | ✅（间接） | esp-sr 内部依赖 |

---

## 2. 各库详细分析

### 2.1 libesp_audio_front_end.a — AFE 框架

**接口头文件**：`esp_afe_sr_iface.h`、`esp_afe_config.h`、`esp_afe_aec.h`

**核心数据结构**：

```c
// AFE 实例句柄（不透明指针）
typedef struct esp_afe_sr_data_t esp_afe_sr_data_t;

// Fetch 结果
typedef struct afe_fetch_result_t {
    int16_t *data;              // 单通道输出 PCM
    int      data_size;         // 字节数
    vad_state_t    vad_state;   // VAD_SILENCE=0 / VAD_SPEECH=1
    wakenet_state_t wakeup_state; // WAKENET_NO_DETECT=0 / WAKENET_DETECTED=1
    int      wake_word_index;   // 唤醒词索引（1-based）
    int      wakenet_model_index;
    float    data_volume;
    int      ret_value;         // ESP_OK=0 / ESP_FAIL
} afe_fetch_result_t;

// 函数指针表（接口）
typedef struct {
    esp_afe_sr_data_t* (*create_from_config)(afe_config_t*);
    void  (*feed)(esp_afe_sr_data_t*, int16_t* in);
    afe_fetch_result_t* (*fetch_with_delay)(esp_afe_sr_data_t*, TickType_t);
    int   (*get_feed_chunksize)(esp_afe_sr_data_t*);   // 返回采样数
    int   (*get_fetch_chunksize)(esp_afe_sr_data_t*);  // 返回采样数
    int   (*get_samp_rate)(esp_afe_sr_data_t*);        // 返回 Hz
    void  (*reset_buffer)(esp_afe_sr_data_t*);
    int   (*enable_aec)(esp_afe_sr_data_t*);
    int   (*disable_aec)(esp_afe_sr_data_t*);
    int   (*enable_vad)(esp_afe_sr_data_t*);
    int   (*disable_vad)(esp_afe_sr_data_t*);
    int   (*enable_ns)(esp_afe_sr_data_t*);
    int   (*disable_ns)(esp_afe_sr_data_t*);
    void  (*destroy)(esp_afe_sr_data_t*);
    // ... 其他控制���数
} esp_afe_sr_iface_t;
```

**配置结构体关键字段**（`afe_config_t`）：

```c
typedef struct {
    bool         aec_init;          // 是否启用 AEC
    aec_mode_t   aec_mode;          // AEC_MODE_SR_HIGH_PERF 等
    bool         ns_init;           // 是否启用 NS
    char        *ns_model_name;     // nsnet 模型名（NULL 时回退到 WebRTC NS）
    afe_ns_mode_t afe_ns_mode;      // AFE_NS_MODE_WEBRTC=0 / AFE_NS_MODE_NET=1
    bool         vad_init;          // 是否启用 VAD
    vad_mode_t   vad_mode;          // VAD_MODE_0~4
    char        *vad_model_name;    // vadnet 模型名（NULL 时使用 WebRTC VAD）
    int          vad_min_speech_ms; // 最小语音持续时间 ms
    int          vad_min_noise_ms;  // 最小噪声持续时间 ms
    bool         wakenet_init;      // 是否启用唤醒词
    char        *wakenet_model_name;
    afe_memory_alloc_mode_t memory_alloc_mode; // AFE_MEMORY_ALLOC_MORE_PSRAM
    afe_pcm_config_t pcm_config;    // 通道配置（mic + ref）
} afe_config_t;
```

**调用入口函数**：

```c
// 获取 AFE 接口（根据配置选择实现）
const esp_afe_sr_iface_t* esp_afe_handle_from_config(afe_config_t*);

// 初始化配置（根据 input_format 字符串构造）
// input_format: "M"=单麦, "MR"=麦+参考, "MM"=双麦, "MMR"=双麦+参考
afe_config_t* afe_config_init(const char* input_format, srmodel_list_t*, afe_type_t, afe_mode_t);
```

**数据流约定**：
- 采样率：固定 16000 Hz
- 位深：int16_t
- Feed 格式：多通道交错（Channel Interleaved），最后通道为参考信号
- Fetch 格式：单通道输出（已选择最优麦克风通道）
- Chunk size：通过 `get_feed_chunksize()` 动态查询（样本数，非字节）

**调用位置**（`afe_audio_processor.cc`）：

```
Initialize() → afe_config_init() → esp_afe_handle_from_config() → create_from_config()
Feed()        → get_feed_chunksize() → feed()
Task loop     → fetch_with_delay() → 读取 vad_state 和 data
EnableDeviceAec() → enable_aec()/disable_aec()
```

**调用位置**（`afe_wake_word.cc`）：

```
Initialize() → esp_srmodel_init() → afe_config_init() → esp_afe_handle_from_config()
Feed()        → get_feed_chunksize() → feed()
Task loop     → fetch_with_delay() → 读取 wakeup_state / wakenet_model_index
```

---

### 2.2 libwakenet.a — 唤醒词检测

**接口头文件**：`esp_wn_iface.h`

**工作方式**：由 `libesp_audio_front_end.a` 内部调度，通过 `afe_config->wakenet_model_name` 加载模型。
项目代码不直接调用 `esp_wn_iface_t`，而是通过 AFE fetch 结果的 `wakeup_state` 字段获知检测结果。

**模型文件**：从 flash 的 `model` 分区加载（编译时由 `srmodels.bin` 打包）。

```c
// 间接触发路径
esp_srmodel_init("model")             // 从 flash 加载模型列表
esp_srmodel_filter(models, ESP_WN_PREFIX, NULL)  // 筛选唤醒词模型名
// 模型名传入 afe_config->wakenet_model_name
```

---

### 2.3 libnsnet.a — 神经网络噪声抑制

**接口头文件**：`esp_ns.h`（基础 NS 接口，speex-style）

**工作方式**：同样由 AFE 内部调度，通过 `afe_config->ns_model_name` 加载 nsnet 神经网络模型。
如果 `ns_model_name = NULL` 则���退到内置 WebRTC NS（基于 `AFE_NS_MODE_WEBRTC`）。

**直接 NS 接口**（不经 AFE，可独立使用）：

```c
ns_handle_t ns_create(int frame_length);     // frame_length: 10/20/30 ms
ns_handle_t ns_pro_create(int frame_length, int mode, int sample_rate);
void ns_process(ns_handle_t inst, int16_t* indata, int16_t* outdata);
void ns_destroy(ns_handle_t inst);
```

---

### 2.4 libvadnet.a — 神经网络 VAD

**接口头文件**：`esp_vad.h`

**工作方式**：由 AFE 内部调度，通过 `afe_config->vad_model_name` 加载 vadnet 模型。
如果 `vad_model_name = NULL` 则回退到 WebRTC VAD。

**直接 VAD 接口**（可独立使用）：

```c
typedef enum { VAD_SILENCE = 0, VAD_SPEECH = 1 } vad_state_t;
typedef enum { VAD_MODE_0 = 0, ..., VAD_MODE_4 } vad_mode_t;

vad_handle_t vad_create(vad_mode_t mode);
vad_handle_t vad_create_with_param(vad_mode_t, int sample_rate, int frame_ms,
                                   int min_speech_ms, int min_noise_ms);
vad_state_t  vad_process(vad_handle_t, int16_t* data, int sample_rate, int frame_ms);
void         vad_destroy(vad_handle_t);
```

**支持参数**：采样率 8000/16000/32000 Hz，帧长 10/20/30 ms。

---

### 2.5 libesp_audio_codec.a — Opus 编解码器

**接口头文件**：`esp_opus_enc.h`、`esp_opus_dec.h`

**编码器关键结构和函数**：

```c
// 配置结构体
typedef struct {
    int sample_rate;             // 8000/12000/16000/24000/48000
    int channel;                 // 1 或 2
    int bits_per_sample;         // 16
    int bitrate;                 // bps
    esp_opus_enc_frame_duration_t frame_duration; // 2.5/5/10/20/40/60/80/100/120 ms
    esp_opus_enc_application_t application_mode;  // VOIP/AUDIO/LOWDELAY
    int complexity;              // 0-10
    bool enable_fec;
    bool enable_dtx;
    bool enable_vbr;
} esp_opus_enc_config_t;

// 输入/输出帧
typedef struct { uint8_t *buffer; uint32_t len; } esp_audio_enc_in_frame_t;
typedef struct { uint8_t *buffer; uint32_t len; uint32_t encoded_bytes; uint64_t pts; } esp_audio_enc_out_frame_t;

// 函数
esp_audio_err_t esp_opus_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);
esp_audio_err_t esp_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);
esp_audio_err_t esp_opus_enc_process(void *enc_hd, esp_audio_enc_in_frame_t*, esp_audio_enc_out_frame_t*);
void            esp_opus_enc_close(void *enc_hd);
```

**调用位置**：
- `audio_service.cc:75-83` — 通话语音编码
- `afe_wake_word.cc:189-243` — 唤醒词 PCM 转 Opus 上传
- `custom_wake_word.cc:235-283` — 同上

**当前配置宏**（`audio_service.h:65-74`）：

```cpp
#define AS_OPUS_ENC_CONFIG() {       \
    .sample_rate    = 16000,         \
    .channel        = 1,             \
    .bits_per_sample = 16,           \
    .bitrate        = 24000,         \
    .frame_duration = 20ms,          \
    .application_mode = VOIP,        \
    .complexity     = 5,             \
    .enable_fec     = false,         \
    .enable_dtx     = false,         \
    .enable_vbr     = false,         \
}
```

---

### 2.6 libesp_audio_effects.a — 采样率转换

**接口头文件**：`esp_ae_rate_cvt.h`

**使用的接口**（仅采样率转换，音效功能未使用）：

```c
typedef struct {
    uint32_t src_rate;       // 源采样率
    uint32_t dest_rate;      // 目标采样率（通常转为 16000）
    uint8_t  channel;        // 通道数
    uint8_t  bits_per_sample; // 16
    uint8_t  complexity;     // 1-3
    esp_ae_rate_cvt_perf_type_t perf_type; // MEMORY 或 SPEED
} esp_ae_rate_cvt_cfg_t;

esp_ae_err_t esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t*, esp_ae_rate_cvt_handle_t*);
esp_ae_err_t esp_ae_rate_cvt_process(handle, in, in_num, out, out_num*);
esp_ae_err_t esp_ae_rate_cvt_get_max_out_sample_num(handle, in_num, out_num*);
void         esp_ae_rate_cvt_close(handle);
```

**调用位置**（`audio_service.cc:87-93`）：

```cpp
// 仅在麦克风采样率 ≠ 16000 Hz 时创建
esp_ae_rate_cvt_cfg_t cfg = RATE_CVT_CFG(codec->input_sample_rate(), 16000, channels);
esp_ae_rate_cvt_open(&cfg, &input_resampler_);
```

---

### 2.7 libesp_new_jpeg.a — JPEG 编解码

**接口头文件**：`esp_jpeg_enc.h`、`esp_jpeg_dec.h`、`esp_jpeg_common.h`

**编码关键接口**：

```c
typedef struct {
    int    width, height;
    jpeg_pixel_format_t src_type;   // RGB888 / YCbYCr / GRAY 等
    jpeg_subsampling_t  subsampling; // 444 / 422 / 420
    uint8_t quality;                 // 1-100
    jpeg_rotate_t rotate;
    bool task_enable;
} jpeg_enc_config_t;

jpeg_error_t jpeg_enc_open(jpeg_enc_config_t*, jpeg_enc_handle_t*);
jpeg_error_t jpeg_enc_process(handle, in_buf, in_size, out_buf, out_size, out_actual*);
jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t);
```

**解码关键接口**：

```c
typedef struct {
    jpeg_pixel_format_t output_type;
    jpeg_resolution_t scale;
    jpeg_rotate_t rotate;
} jpeg_dec_config_t;

jpeg_error_t jpeg_dec_open(jpeg_dec_config_t*, jpeg_dec_handle_t*);
jpeg_error_t jpeg_dec_parse_header(handle, jpeg_dec_io_t*, jpeg_dec_header_info_t*);
jpeg_error_t jpeg_dec_process(handle, jpeg_dec_io_t*);
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t);
```

**注意**：输入/输出缓冲区均需要 **16 字节对齐**（通过 `jpeg_calloc_align(size, 16)` 分配）。

**调用位置**：
- `main/display/lvgl_display/jpg/image_to_jpeg.cpp` — 摄像头 RGB 转 JPEG

---

## 3. 调用关系总图

```
audio_service.cc
  ├─ esp_opus_enc_*          → libesp_audio_codec.a
  ├─ esp_opus_dec_*          → libesp_audio_simple_dec.a
  └─ esp_ae_rate_cvt_*       → libesp_audio_effects.a

afe_audio_processor.cc
  └─ esp_afe_handle_from_config()  → libesp_audio_front_end.a
       ├─ AEC (内部)               → libesp_audio_front_end.a
       ├─ NS (内部，nsnet 模型)    → libnsnet.a
       └─ VAD (内部，vadnet 模型)  → libvadnet.a

afe_wake_word.cc
  ├─ esp_srmodel_init()            → libesp_audio_front_end.a (模型加载)
  ├─ esp_afe_handle_from_config()  → libesp_audio_front_end.a
  │    └─ WakeNet (内部)           → libwakenet.a
  └─ esp_opus_enc_*                → libesp_audio_codec.a

image_to_jpeg.cpp
  └─ jpeg_enc_*               → libesp_new_jpeg.a
```

---

## 4. 可复用的开源源码部分

`esp-sr` 组件中有少量开放源码，可直接复用（不需要替换）：

| 源文件 | 功能 | 位置 |
|--------|------|------|
| `model_path.c` | 从 flash 加载模型路径 | `espressif__esp-sr/src/` |
| `esp_mn_speech_commands.c` | 语音命令配置 | `espressif__esp-sr/src/` |
| `esp_process_sdkconfig.c` | sdkconfig 处理 | `espressif__esp-sr/src/` |

替换后这些文件中的 `esp_srmodel_init` 调用方式需要调整（或直接去掉 flash 模型加载逻辑）。
