#include "open_afe_audio_processor.h"
#include "open_aec.h"
#include "open_ns.h"
#include "open_vad.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

extern "C" unsigned int speex_pool_usage(void);
extern "C" unsigned int speex_pool_capacity(void);
extern "C" void speex_pool_mark(void);
extern "C" void speex_pool_rollback(void);

static const char* TAG = "OpenAfeProc";

OpenAfeAudioProcessor::OpenAfeAudioProcessor() = default;

OpenAfeAudioProcessor::~OpenAfeAudioProcessor() {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        stopped_ = true;
        running_ = false;
    }
    input_cv_.notify_all();
    // Give the task a moment to exit (no join in FreeRTOS; task self-deletes)
    vTaskDelay(pdMS_TO_TICKS(50));
}

void OpenAfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms,
                                       srmodel_list_t* /*models_list*/) {
    codec_        = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    if (codec_->input_reference()) {
        aec_ = std::make_unique<OpenAec>(16000, 5, 5);  // 5ms/80-sample frame+filter: state ~20KB vs 10ms→40KB; fits 48KB pool
        aec_mic_.resize(aec_->frame_size());
        aec_ref_.resize(aec_->frame_size());
        aec_out_.resize(aec_->frame_size());
    }
    speex_pool_mark();  // mark pool before NS so Reset() can rollback and reuse same DRAM
    ns_  = std::make_unique<OpenNS>(16000, 5, -15);  // 5ms→80samples; state ~14KB vs 10ms→29KB; reduces DRAM pool requirement
    vad_ = std::make_unique<OpenVad>(2, 16000);
    output_buf_.reserve(frame_samples_);
    input_buf_.reserve(frame_samples_ * 4);  // keep in SRAM; prevents PSRAM realloc under load

    ESP_LOGI(TAG, "init: frame=%dms (%d samples) aec=%s pool=%u/%u sram_free=%u",
             frame_duration_ms, frame_samples_, aec_ ? "yes" : "no",
             speex_pool_usage(), speex_pool_capacity(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    xTaskCreatePinnedToCore([](void* arg) {
        static_cast<OpenAfeAudioProcessor*>(arg)->ProcessingTask();
        vTaskDelete(NULL);
    }, "open_afe_proc", 4096 * 2, this, 6, NULL, 1);
}

void OpenAfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!running_) return;
    // If the processor is falling behind, drop the stale backlog rather than let
    // it grow into PSRAM.  The ProcessingTask drains the entire buf_ in one pass
    // (unbounded loop), so a large backlog causes seconds of continuous CPU use
    // which starves IDLE and triggers the task watchdog.
    if ((int)input_buf_.size() > frame_samples_ * 3) {
        ESP_LOGW(TAG, "audio backlog %u > 3 frames, dropping", (unsigned)input_buf_.size());
        input_buf_.clear();
    }
    input_buf_.insert(input_buf_.end(), data.begin(), data.end());
    input_cv_.notify_one();
}

void OpenAfeAudioProcessor::Start() {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        running_ = true;
    }
    input_cv_.notify_one();
    ESP_LOGD(TAG, "start");
}

void OpenAfeAudioProcessor::Stop() {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        running_ = false;
        input_buf_.clear();
    }
    // Request reset: ProcessingTask will apply it on its own thread to avoid data races.
    // Never touch output_buf_ / aec_ / ns_ / vad_ from this thread while ProcessingTask runs.
    reset_pending_ = true;
    ESP_LOGD(TAG, "stop");
}

bool OpenAfeAudioProcessor::IsRunning() {
    std::lock_guard<std::mutex> lock(input_mutex_);
    return running_;
}

size_t OpenAfeAudioProcessor::GetFeedSize() {
    return 160; // 10 ms at 16 kHz
}

void OpenAfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&&)> cb) {
    output_callback_ = cb;
}

void OpenAfeAudioProcessor::OnVadStateChange(std::function<void(bool)> cb) {
    vad_state_change_callback_ = cb;
}

void OpenAfeAudioProcessor::EnableDeviceAec(bool /*enable*/) {
    // speex AEC runs continuously; no dynamic toggle
}

void OpenAfeAudioProcessor::Deinitialize() {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        stopped_ = true;
        running_ = false;
        input_buf_.clear();
    }
    input_cv_.notify_all();
    vTaskDelay(pdMS_TO_TICKS(100));  // wait for processing task to exit

    // Destroy AEC/NS/VAD while pool memory is still valid, then roll back pool.
    aec_.reset();
    ns_.reset();
    vad_.reset();
    speex_pool_rollback();

    // Release SRAM held by pre-allocated buffers.
    { std::vector<int16_t>().swap(input_buf_); }
    { std::vector<int16_t>().swap(output_buf_); }
    { std::vector<int16_t>().swap(aec_mic_); }
    { std::vector<int16_t>().swap(aec_ref_); }
    { std::vector<int16_t>().swap(aec_out_); }

    reset_pending_ = false;
    stopped_ = false;  // allow future re-initialization
    ESP_LOGI(TAG, "deinit: sram_free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void OpenAfeAudioProcessor::ProcessingTask() {
    ESP_LOGI(TAG, "processing task started");

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

        // Apply pending reset (requested by Stop()) on this thread to avoid races.
        if (reset_pending_.exchange(false)) {
            output_buf_.clear();
            is_speaking_ = false;
            if (aec_) aec_->Reset();
            if (ns_)  ns_->Reset();  // pool rollback + reinit; takes ~10-30ms
            if (vad_) vad_->Reset();
            // Discard audio that accumulated during reinit to avoid immediate backlog drop.
            std::lock_guard<std::mutex> lock(input_mutex_);
            input_buf_.clear();
        }

        // ── 1. AEC: de-interleave [mic,ref], cancel echo, output mono ──────
        if (aec_) {
            const int fs     = aec_->frame_size();
            const int stride = 2;
            std::vector<int16_t> mono;
            mono.reserve(data.size() / stride);
            int i = 0;
            while (i + fs * stride <= (int)data.size()) {
                for (int j = 0; j < fs; j++) {
                    aec_mic_[j] = data[i + j * stride];
                    aec_ref_[j] = data[i + j * stride + 1];
                }
                aec_->Process(aec_mic_.data(), aec_ref_.data(), aec_out_.data());
                mono.insert(mono.end(), aec_out_.begin(), aec_out_.end());
                i += fs * stride;
            }
            data = std::move(mono);
        } else if (codec_->input_reference()) {
            // 2-channel but no AEC: extract mic channel
            std::vector<int16_t> mono(data.size() / 2);
            for (size_t j = 0; j < mono.size(); j++) mono[j] = data[j * 2];
            data = std::move(mono);
        }
        // data is now mono 16 kHz

        // ── 2. NS ────────────────────────────────────────────────────────────
        std::vector<int16_t> ns_out;
        if (ns_) {
            ns_->Feed(data.data(), (int)data.size(), ns_out);
        } else {
            ns_out = std::move(data);
        }
        if (ns_out.empty()) continue;

        // ── 3. VAD ───────────────────────────────────────────────────────────
        if (vad_state_change_callback_ && vad_) {
            if (vad_->Feed(ns_out.data(), (int)ns_out.size())) {
                is_speaking_ = vad_->IsSpeaking();
                vad_state_change_callback_(is_speaking_);
            }
        }

        // ── 4. Output (accumulate to frame_samples_) ─────────────────────────
        if (output_callback_) {
            output_buf_.insert(output_buf_.end(), ns_out.begin(), ns_out.end());
            while ((int)output_buf_.size() >= frame_samples_) {
                if ((int)output_buf_.size() == frame_samples_) {
                    output_callback_(std::move(output_buf_));
                    output_buf_.clear();
                    output_buf_.reserve(frame_samples_);
                } else {
                    output_callback_(std::vector<int16_t>(
                        output_buf_.begin(), output_buf_.begin() + frame_samples_));
                    output_buf_.erase(output_buf_.begin(),
                                      output_buf_.begin() + frame_samples_);
                }
            }
        }

        // Yield 1ms so IDLE1 can run on CPU1 (resets task watchdog).
        // 10ms caused the backlog to grow: WiFi preemptions on CPU0 made
        // each NS frame take >10ms effective time; now on CPU1 those
        // preemptions are gone and 1ms yield is enough for watchdog.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
