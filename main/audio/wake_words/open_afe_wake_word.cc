#include "open_afe_wake_word.h"
#include "audio_service.h"
#include "processors/open_vad.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char* TAG = "OpenAfeWW";

// Wake word string reported upstream; must be non-empty for the protocol.
static constexpr const char* kWakeWordName = "你好小智";

OpenAfeWakeWord::OpenAfeWakeWord() = default;

OpenAfeWakeWord::~OpenAfeWakeWord() {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        stopped_ = true;
        running_ = false;
    }
    input_cv_.notify_all();

    // Wait for the detection task (xTaskCreate, FreeRTOS-owned heap stack) to
    // exit so it stops accessing `vad_`/`codec_`.
    constexpr int kDetectWaitMs = 500;
    int waited = 0;
    while (!detect_task_exited_.load(std::memory_order_acquire) && waited < kDetectWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }

    // Wait for any in-flight encode task to finish — it uses our static stack,
    // so we must not free it while the task is running (libopus over 2s of PCM
    // typically takes ~6s on ESP32-S3).
    constexpr int kEncodeWaitMs = 8000;
    waited = 0;
    while (encode_task_running_.load(std::memory_order_acquire) && waited < kEncodeWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    if (encode_task_running_.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "encode task did not exit within %dms; leaking stack to avoid UAF", kEncodeWaitMs);
        return;
    }

    if (encode_task_stack_)  heap_caps_free(encode_task_stack_);
    if (encode_task_buffer_) heap_caps_free(encode_task_buffer_);
}

bool OpenAfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* /*models_list*/) {
    codec_ = codec;
    // Mode 1 (low-bitrate): good speech sensitivity, fewer false triggers than mode 0
    // mode=3 (very-aggressive) to reject background noise; onset=5×30ms=150ms of speech
    vad_ = std::make_unique<OpenVad>(3, 16000, 5);
    ESP_LOGI(TAG, "init: vad trigger (no WakeNet); ref=%s",
             codec_->input_reference() ? "yes" : "no");

    detect_task_exited_ = false;
    xTaskCreate([](void* arg) {
        static_cast<OpenAfeWakeWord*>(arg)->DetectionTask();
        static_cast<OpenAfeWakeWord*>(arg)->detect_task_exited_.store(true, std::memory_order_release);
        vTaskDelete(NULL);
    }, "open_afe_detect", 4096 * 2, this, 3, nullptr);

    return true;
}

void OpenAfeWakeWord::OnWakeWordDetected(std::function<void(const std::string&)> cb) {
    wake_word_detected_callback_ = cb;
}

void OpenAfeWakeWord::Start() {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        running_ = true;
        vad_->Reset();
    }
    warmup_samples_left_ = 16000; // discard first 1s to absorb codec init transients
    input_cv_.notify_one();
    ESP_LOGD(TAG, "start (listening for speech)");
}

void OpenAfeWakeWord::Stop() {
    std::lock_guard<std::mutex> lock(input_mutex_);
    running_ = false;
    input_buf_.clear();
}

void OpenAfeWakeWord::Feed(const std::vector<int16_t>& data) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!running_) return;
    input_buf_.insert(input_buf_.end(), data.begin(), data.end());
    input_cv_.notify_one();
}

size_t OpenAfeWakeWord::GetFeedSize() {
    return 160; // 10 ms at 16 kHz
}

void OpenAfeWakeWord::DetectionTask() {
    ESP_LOGI(TAG, "detection task started");

    while (true) {
        std::vector<int16_t> data;
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait(lock, [this] {
                return stopped_ || (running_ && !input_buf_.empty());
            });
            if (stopped_) return;
            if (!running_ || input_buf_.empty()) continue;
            data.swap(input_buf_);
        }

        // Extract mic channel when 2-channel interleaved
        std::vector<int16_t> mono;
        if (codec_->input_reference()) {
            mono.resize(data.size() / 2);
            for (size_t i = 0; i < mono.size(); i++) mono[i] = data[i * 2];
        } else {
            mono = std::move(data);
        }

        // Keep rolling 2-second context for pre-wake encoding
        StoreWakeWordData(mono.data(), (int)mono.size());

        // Drain warmup window: ignore audio right after Start() (codec init transients)
        int remaining = warmup_samples_left_.load(std::memory_order_relaxed);
        if (remaining > 0) {
            warmup_samples_left_ = std::max(0, remaining - (int)mono.size());
            continue;
        }

        // VAD: detect speech onset
        if (vad_->Feed(mono.data(), (int)mono.size()) && vad_->IsSpeaking()) {
            Stop();
            last_detected_wake_word_ = kWakeWordName;
            ESP_LOGI(TAG, "speech detected → wake word fired");
            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
}

void OpenAfeWakeWord::StoreWakeWordData(const int16_t* data, int samples) {
    wake_word_pcm_.emplace_back(data, data + samples);
    // Keep ~2 seconds: 10ms chunks × 200 = 2000ms
    while (wake_word_pcm_.size() > 200) {
        wake_word_pcm_.pop_front();
    }
}

void OpenAfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 12; // libopus needs ~30 KB
    wake_word_opus_.clear();

    if (!encode_task_stack_) {
        encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(encode_task_stack_ != nullptr);
    }
    if (!encode_task_buffer_) {
        encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(encode_task_buffer_ != nullptr);
    }

    encode_task_running_ = true;
    xTaskCreateStatic([](void* arg) {
        auto self = static_cast<OpenAfeWakeWord*>(arg);
        {
            auto t0 = esp_timer_get_time();
            esp_opus_enc_config_t cfg = AS_OPUS_ENC_CONFIG();
            void* enc = nullptr;
            auto ret = esp_opus_enc_open(&cfg, sizeof(cfg), &enc);
            if (!enc) {
                ESP_LOGE(TAG, "enc open failed: %d", ret);
                std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
                self->wake_word_opus_.push_back({});
                self->wake_word_cv_.notify_all();
                vTaskDelete(NULL);
                return;
            }

            int frame_size = 0, outbuf_size = 0;
            esp_opus_enc_get_frame_size(enc, &frame_size, &outbuf_size);
            frame_size /= sizeof(int16_t);

            int packets = 0;
            std::vector<int16_t> buf;
            esp_audio_enc_in_frame_t  in  = {};
            esp_audio_enc_out_frame_t out = {};

            for (auto& pcm : self->wake_word_pcm_) {
                buf.insert(buf.end(), pcm.begin(), pcm.end());
                while ((int)buf.size() >= frame_size) {
                    std::vector<uint8_t> opus_buf(outbuf_size);
                    in.buffer = (uint8_t*)buf.data();
                    in.len    = (uint32_t)(frame_size * sizeof(int16_t));
                    out.buffer = opus_buf.data();
                    out.len    = outbuf_size;
                    out.encoded_bytes = 0;
                    if (esp_opus_enc_process(enc, &in, &out) == ESP_AUDIO_ERR_OK) {
                        std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
                        self->wake_word_opus_.emplace_back(
                            opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                        self->wake_word_cv_.notify_all();
                        packets++;
                    }
                    buf.erase(buf.begin(), buf.begin() + frame_size);
                }
            }
            self->wake_word_pcm_.clear();
            esp_opus_enc_close(enc);
            ESP_LOGI(TAG, "encoded %d opus packets in %ld ms",
                     packets, (long)((esp_timer_get_time() - t0) / 1000));

            std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
            self->wake_word_opus_.push_back({}); // sentinel
            self->wake_word_cv_.notify_all();
        }
        self->encode_task_running_.store(false, std::memory_order_release);
        vTaskDelete(NULL);
    }, "open_ww_enc", stack_size, this, 2, encode_task_stack_, encode_task_buffer_);
}

bool OpenAfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this] { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
