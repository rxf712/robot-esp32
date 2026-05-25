# API 对照表

编码阶段的直接参考。逐函数列出闭源 API 与开源替代 API 的对照关系、参数差异和注意事项。

---

## 1. Opus 编码器（libesp_audio_codec.a → libopus）

### 初始化

| 步骤 | 闭源 API | 开源替代（libopus） |
|------|---------|-----------------|
| 创建编码器 | `esp_opus_enc_open(&cfg, sizeof(cfg), &handle)` | `opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &err)` |
| 查询帧大小 | `esp_opus_enc_get_frame_size(handle, &in_size, &out_size)` | `in_size = frame_duration_ms * sample_rate / 1000 * sizeof(int16_t)` |
| 设置比特率 | `esp_opus_enc_set_bitrate(handle, bitrate)` | `opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate))` |
| 销毁 | `esp_opus_enc_close(handle)` | `opus_encoder_destroy(enc)` |

### 编码

```c
// 闭源
esp_audio_enc_in_frame_t  in  = { .buffer = (uint8_t*)pcm, .len = frame_bytes };
esp_audio_enc_out_frame_t out = { .buffer = out_buf, .len = out_buf_size, .encoded_bytes = 0 };
esp_opus_enc_process(handle, &in, &out);
int actual_bytes = out.encoded_bytes;

// 开源（libopus）
int actual_bytes = opus_encode(
    enc,
    (const opus_int16*)pcm,   // 输入：int16_t PCM
    frame_size_samples,        // 每帧采样数（不是字节数）
    out_buf,                   // 输出缓冲
    out_buf_size               // 缓冲大小
);
// actual_bytes > 0 表示成功
```

**关键差异**：
- libopus `opus_encode` 的第三个参数是**采样数**，不是字节数
- 输出长度直接由返回值给出，不需要额外字段
- 建议 `out_buf_size = 4000`（足够容纳任何 20ms 帧）

### 配置对应关系

| esp_opus_enc_config_t 字段 | libopus 等价设置 |
|--------------------------|----------------|
| `sample_rate = 16000` | `opus_encoder_create(16000, ...)` |
| `channel = 1` | `opus_encoder_create(..., 1, ...)` |
| `bitrate = 24000` | `opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000))` |
| `frame_duration = 20ms` | `frame_size = 16000 * 20 / 1000 = 320` samples |
| `complexity = 5` | `opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5))` |
| `enable_fec = false` | `opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(0))` |
| `enable_dtx = false` | `opus_encoder_ctl(enc, OPUS_SET_DTX(0))` |
| `enable_vbr = false` | `opus_encoder_ctl(enc, OPUS_SET_VBR(0))` |
| `application_mode = VOIP` | `OPUS_APPLICATION_VOIP` |

---

## 2. Opus 解码器（libesp_audio_simple_dec.a → libopus）

### 初始化与解码

```c
// 闭源
esp_opus_dec_cfg_t dec_cfg = { .sample_rate = 16000, .channel = 1, .frame_duration = 20ms };
esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &decoder);
esp_audio_dec_in_frame_t  in  = { .buffer = opus_data, .len = opus_len };
esp_audio_dec_out_frame_t out = { .buffer = pcm_buf, .len = pcm_buf_size };
esp_opus_dec_process(decoder, &in, &out);
esp_opus_dec_close(decoder);

// 开源（libopus）
int err;
OpusDecoder *dec = opus_decoder_create(16000, 1, &err);

int samples = opus_decode(
    dec,
    opus_data,                 // 输入 Opus 数据（NULL 表示帧丢失）
    opus_len,                  // 输入数据字节数
    (opus_int16*)pcm_buf,      // 输出 PCM 缓冲
    frame_size_samples,        // 每帧采样数（320 for 20ms@16kHz）
    0                          // decode_fec: 通常为 0
);
// samples > 0 表示成功，等于实际解码的采样数

opus_decoder_destroy(dec);
```

**关键差异**：
- 解码时需要预知 `frame_size_samples`（320 for 20ms@16kHz@mono）
- `opus_decode` 返回值是采样数，PCM 字节数 = samples × sizeof(int16_t)

---

## 3. 采样率转换（libesp_audio_effects.a → libspeexdsp）

### 初始化与处理

```c
// 闭源
esp_ae_rate_cvt_cfg_t cfg = { .src_rate=48000, .dest_rate=16000, .channel=1, .complexity=2 };
esp_ae_rate_cvt_handle_t handle;
esp_ae_rate_cvt_open(&cfg, &handle);

uint32_t in_num = 480, out_num;
esp_ae_rate_cvt_process(handle, in_buf, in_num, out_buf, &out_num);
esp_ae_rate_cvt_close(handle);

// 开源（speexdsp）
#include "speex/speex_resampler.h"
int err;
SpeexResamplerState *resampler = speex_resampler_init(
    1,      // 通道数
    48000,  // 源采样率
    16000,  // 目标采样率
    3,      // quality: 0-10，3 为嵌入式推荐值
    &err
);

spx_uint32_t in_len = 480, out_len = 160;
speex_resampler_process_int(
    resampler,
    0,                          // 通道索引（单通道传 0）
    (const spx_int16_t*)in_buf, // 输入
    &in_len,                    // 输入采样数（会被修改为实际消费数）
    (spx_int16_t*)out_buf,      // 输出
    &out_len                    // 输出采样数（会被修改为实际产生数）
);

speex_resampler_destroy(resampler);
```

**关键差异**：
- speexdsp 的 `in_len` 和 `out_len` 是**双向参数**（传入最大值，返回实际值）
- 多通道时使用 `speex_resampler_process_interleaved_int`
- quality=3 时 CPU 占用约为 quality=7 的 1/3，质量足够语音使用

### 输出缓冲大小计算

```cpp
uint32_t get_max_out_samples(uint32_t in_samples, uint32_t src_rate, uint32_t dest_rate) {
    return (uint32_t)((uint64_t)in_samples * dest_rate / src_rate) + 10; // +10 safety margin
}
```

---

## 4. VAD（libvadnet.a → libfvad）

### 初始化与处理

```c
// 闭源（独立 VAD，不经 AFE）
vad_handle_t vad = vad_create_with_param(
    VAD_MODE_0,    // 模式 0-4
    16000,         // 采样率
    20,            // 帧长 ms
    128,           // min_speech_ms
    1000           // min_noise_ms
);
vad_state_t state = vad_process(vad, pcm, 16000, 20);
vad_destroy(vad);

// 开源（libfvad）
#include "fvad.h"
Fvad *vad = fvad_new();
fvad_set_mode(vad, 2);           // 0=宽松, 1=普通, 2=严格, 3=极严格
fvad_set_sample_rate(vad, 16000); // 8000/16000/32000/48000

// 每次处理一帧（10/20/30ms）
// 20ms @16kHz = 320 samples
int result = fvad_process(vad, pcm_frame, 320);
// result: 1=语音, 0=静音, -1=错误

fvad_free(vad);
```

**模式对应关系**：

| esp-sr vad_mode_t | libfvad mode | 说明 |
|-------------------|-------------|------|
| `VAD_MODE_0` | 0 | 最宽松（容易检测语音） |
| `VAD_MODE_1` | 1 | 略严格 |
| `VAD_MODE_2` | 2 | 较严格 |
| `VAD_MODE_3` | 3 | 很严格 |
| `VAD_MODE_4` | 3 | 极严格（libfvad 最高为 3）|

**min_speech_ms / min_noise_ms 的实现**：
libfvad 本身是逐帧 VAD，不内置最小持续时间逻辑，需要在 `OpenVad` 类中手动实现：

```cpp
vad_state_t OpenVad::Process(const int16_t *pcm, int samples) {
    int raw = fvad_process(vad_, pcm, samples);  // 1 or 0
    if (raw == 1) {
        speech_count_++;
        silence_count_ = 0;
        if (speech_count_ >= min_speech_frames_) return OPEN_VAD_SPEECH;
    } else {
        silence_count_++;
        speech_count_ = 0;
        if (silence_count_ >= min_silence_frames_) return OPEN_VAD_SILENCE;
    }
    return last_state_;  // 状态未切换
}
```

---

## 5. NS 噪声抑制（libnsnet.a → speex_preprocess）

### 初始化与处理

```c
// 闭源（独立 NS）
ns_handle_t ns = ns_pro_create(20, 1, 16000);  // 20ms 帧, mode=1(中等), 16kHz
ns_process(ns, indata, outdata);
ns_destroy(ns);

// 开源（speexdsp preprocess）
#include "speex/speex_preprocess.h"
// frame_size: 20ms @ 16kHz = 320 samples
SpeexPreprocessState *st = speex_preprocess_state_init(320, 16000);

// 配置降噪参数
int denoise = 1;
speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
int noise_suppress = -25;  // dB，范围约 -5 到 -45，越负越激进
speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_suppress);

// 可选：启用 VAD（副产物）
int vad = 1;
speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &vad);
int vad_prob_start = 85;   // VAD 开始概率阈值（0-100）
int vad_prob_continue = 50;
speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &vad_prob_start);
speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &vad_prob_continue);

// 处理（in-place）
// 返回值：1=有语音，0=静音（当开启 VAD 时有效）
int is_speech = speex_preprocess_run(st, (spx_int16_t*)pcm_frame);

speex_preprocess_state_destroy(st);
```

**关键参数说明**：
- `SPEEX_PREPROCESS_SET_NOISE_SUPPRESS = -20`：推荐起始值（温和）
- `SPEEX_PREPROCESS_SET_NOISE_SUPPRESS = -30`：较激进，可能影响音质
- frame_size 必须固定（不能动态变化）
- speex_preprocess 与 speex_echo 配合使用时，建议将 AEC 状态传给 preprocess

---

## 6. AEC 回声消除（libesp_audio_front_end.a → speex_echo）

### 初始化与处理

```c
// 闭源（直接 AEC 接口，非 AFE）
aec_handle_t *aec = aec_create(16000, 4, 1, AEC_MODE_VOIP_HIGH_PERF);
aec_process(aec, mic_data, ref_data, out_data);
aec_destroy(aec);

// 开源（speexdsp echo cancellation）
#include "speex/speex_echo.h"
// frame_size: 320 samples（20ms @16kHz，建议与 NS 一致）
// filter_length: 2400 samples（150ms 回声尾，16000*0.15）
SpeexEchoState *echo = speex_echo_state_init(320, 2400);

int sample_rate = 16000;
speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &sample_rate);

// 每帧处理
// mic_in: 麦克风信号（frame_size 个 int16_t）
// ref_in: 扬声器参考信号（frame_size 个 int16_t，与 mic 时间对齐）
// out:    消除回声后的信号（frame_size 个 int16_t）
speex_echo_cancellation(echo,
    (const spx_int16_t*)mic_in,
    (const spx_int16_t*)ref_in,
    (spx_int16_t*)out
);

speex_echo_state_destroy(echo);
```

### AEC + NS 联合使用（推荐方式）

```c
// speex 官方建议：AEC 输出作为 NS 的输入，并通知 NS 当前 AEC 状态
SpeexEchoState *echo = speex_echo_state_init(320, 2400);
SpeexPreprocessState *ns = speex_preprocess_state_init(320, 16000);

// 将 echo state 关联到 preprocess，NS 可利用 AEC 的残余回声估计
speex_preprocess_ctl(ns, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);

// 处理循环（每帧）
speex_echo_cancellation(echo, mic, ref, aec_out);
speex_preprocess_run(ns, aec_out);   // in-place 继续降噪
// aec_out 现在包含回声消除 + 降噪后的清洁语音
```

**时间对齐关键**：
- `mic` 和 `ref` 必须来自**同一录制时刻**
- 典型延迟：ESP32-S3 I2S 采集到 AEC 处理约 1-2ms（可忽略）
- 若存在较大延迟（数十 ms），需要缓冲 `ref` 信号并补偿对齐

---

## 7. JPEG 编码（libesp_new_jpeg.a → esp_driver_jpeg）

### ESP32-S3 阶段

```c
// 闭源（esp_new_jpeg）
jpeg_enc_config_t cfg = {
    .width = 320, .height = 240,
    .src_type = JPEG_PIXEL_FORMAT_RGB888,
    .subsampling = JPEG_SUBSAMPLE_420,
    .quality = 80,
    .rotate = JPEG_ROTATE_0D,
};
jpeg_enc_handle_t enc;
jpeg_enc_open(&cfg, &enc);
int actual_size;
jpeg_enc_process(enc, in_buf, in_size, out_buf, out_buf_size, &actual_size);
jpeg_enc_close(enc);

// 开源（ESP-IDF esp_driver_jpeg，IDF 5.1+）
#include "driver/jpeg_encode.h"
jpeg_encode_cfg_t enc_cfg = {
    .src_type    = JPEG_ENCODE_IN_FORMAT_RGB888,
    .sub_sample  = JPEG_DOWN_SAMPLING_YUV420,
    .image_quality = 80,
    .width  = 320,
    .height = 240,
};
jpeg_encoder_handle_t enc_engine;
jpeg_new_encoder_engine(&enc_cfg, &enc_engine);

jpeg_encode_input_cfg_t input_cfg = {
    .inbuf  = in_buf,
    .inbuf_size = in_size,
    .outbuf = out_buf,
    .outbuf_size = out_buf_size,
};
uint32_t actual_size;
jpeg_encoder_process(enc_engine, &input_cfg, &actual_size, 100 /* timeout ms */);
jpeg_del_encoder_engine(enc_engine);
```

**注意**：
- `esp_driver_jpeg` 使用 ESP32-S3 硬件 JPEG 加速，速度比软件快 5-10 倍
- 输入缓冲仍需 16 字节对齐（`heap_caps_aligned_alloc(16, size, MALLOC_CAP_DMA)`）
- API 在 IDF 5.1 引入，IDF 5.5.2 已稳定

### 跨平台阶段（libjpeg-turbo）

```c
// libjpeg-turbo 标准 libjpeg API
#include <jpeglib.h>
struct jpeg_compress_struct cinfo;
struct jpeg_error_mgr jerr;
cinfo.err = jpeg_std_error(&jerr);
jpeg_create_compress(&cinfo);

// 设置输出到内存缓冲
unsigned char *out_buf = NULL;
unsigned long out_size = 0;
jpeg_mem_dest(&cinfo, &out_buf, &out_size);

cinfo.image_width  = 320;
cinfo.image_height = 240;
cinfo.input_components = 3;
cinfo.in_color_space = JCS_RGB;
jpeg_set_defaults(&cinfo);
jpeg_set_quality(&cinfo, 80, TRUE);
jpeg_start_compress(&cinfo, TRUE);

// 逐行写入
while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = &rgb_buf[cinfo.next_scanline * 320 * 3];
    jpeg_write_scanlines(&cinfo, &row, 1);
}
jpeg_finish_compress(&cinfo);
jpeg_destroy_compress(&cinfo);
// out_buf 和 out_size 包含编码结果（调用方负责 free(out_buf)）
```

---

## 8. AFE 完整流水线对照

### 当前 esp-sr AFE 调用流程

```cpp
// 初始化
srmodel_list_t *models = esp_srmodel_init("model");  // 从 flash 加载模型
afe_config_t *cfg = afe_config_init("MR", models, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
cfg->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
cfg->aec_init = true;
cfg->vad_init = false;  // AEC 模式时关闭 VAD
cfg->ns_init = true;
cfg->ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
const esp_afe_sr_iface_t *iface = esp_afe_handle_from_config(cfg);
esp_afe_sr_data_t *data = iface->create_from_config(cfg);

// Feed（每帧，多通道交错格式）
// 格式：[mic_s0, ref_s0, mic_s1, ref_s1, ..., mic_sN, ref_sN]
int feed_size = iface->get_feed_chunksize(data);  // 采样数（单通道）
int channels = 2;  // "MR" = 1 mic + 1 ref
iface->feed(data, pcm_multi_ch);  // 输入 feed_size * channels 个样本

// Fetch（阻塞，从处理线程获取结果）
afe_fetch_result_t *res = iface->fetch_with_delay(data, portMAX_DELAY);
if (res && res->ret_value == ESP_OK) {
    // res->data: 单通道 clean PCM，res->data_size 字节
    // res->vad_state: VAD_SILENCE 或 VAD_SPEECH
}
```

### 开源替代流程（OpenAfeAudioProcessor 内部）

```cpp
// 初始化
aec_ = std::make_unique<OpenAec>(320, 2400, 16000);  // frame=20ms, filter=150ms
ns_  = std::make_unique<OpenNs>(320, 16000, -25);
vad_ = std::make_unique<OpenVad>(2, 128, 1000, 16000);
// 关联 AEC 状态到 NS（重要优化）
speex_preprocess_ctl(ns_->state(), SPEEX_PREPROCESS_SET_ECHO_STATE, aec_->state());

// Feed（单帧处理，frame_size = 320 samples）
// 输入格式与 esp-sr 相同：多通道交错 int16_t
void OpenAfeAudioProcessor::ProcessFrame(const int16_t *multi_ch, int16_t *out_single_ch) {
    // 解交错：分离 mic 和 ref 通道
    int16_t mic[320], ref[320];
    for (int i = 0; i < 320; i++) {
        mic[i] = multi_ch[i * 2];      // 偶数索引 = mic
        ref[i] = multi_ch[i * 2 + 1];  // 奇数索引 = ref
    }
    // AEC
    int16_t aec_out[320];
    aec_->Process(mic, ref, aec_out);
    // NS（in-place）
    ns_->Process(aec_out);
    // VAD
    open_vad_state_t vad_state = vad_->Process(aec_out, 320);
    // 输出
    memcpy(out_single_ch, aec_out, 320 * sizeof(int16_t));
    // 通知 VAD 状态变化（与原逻辑相同）
}
```

---

## 9. 常见移植陷阱

### 9.1 帧大小不一致

esp-sr AFE 的 `get_feed_chunksize()` 返回值可能不是固定的 320（取决于配置）。
替换后必须统一 frame_size：

```cpp
// 建议在 OpenAfeAudioProcessor::Initialize 中固定
const int kFrameSize = 320;  // 20ms @16kHz，对 AEC/NS/VAD 最优
```

### 9.2 多通道数据格式

esp-sr Feed 格式是**通道交错**（Channel Interleaved）：
```
[mic0, ref0, mic1, ref1, ..., micN, refN]
```

speex AEC 需要**分离通道**（mic 和 ref 各自是独立数组），必须先解交错。

### 9.3 speex AEC filter_length 选择

```
filter_length = sample_rate × echo_tail_seconds
推荐值（16kHz）：
  - 强音箱（近讲）：1600（100ms）
  - 普通场景：2400（150ms）
  - 远讲/大房间：4800（300ms）
```

初始用 2400，如果回声未消除则增大，如果 CPU 占用过高则减小。

### 9.4 libopus 内存分配

ESP32 的内部 SRAM 有限（512KB），libopus 编码器约需 30KB。
建议使用 PSRAM 分配编码器：

```cpp
// 自定义 opus 分配器（可选，若 opus 内部 malloc 压力大时）
// libopus 支持通过编译时宏替换 malloc/free
```

### 9.5 speexdsp 需要 HAVE_CONFIG_H

speexdsp 源码中用 `#ifdef HAVE_CONFIG_H` 包裹配置。
必须在 CMakeLists 中添加 `-DHAVE_CONFIG_H` 并提供 `config.h`（见 Phase 0.3）。
