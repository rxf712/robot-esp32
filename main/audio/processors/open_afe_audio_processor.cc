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
    Deinitialize();
}

void OpenAfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms,
                                       srmodel_list_t* /*models_list*/) {
    codec_        = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // NOTE: Software AEC (OpenAec / Speex MDF) is intentionally disabled here.
    //   - Speex MDF on ESP32-S3 cannot process a 10ms frame in real time
    //     (3-5× the available window) → audio backlog accumulates.
    //   - CONFIG_USE_SERVER_AEC is also not viable: the upstream xiaozhi
    //     server (tenclass) rejects the `features.aec=true` hello field with
    //     a goodbye, so the conversation can't even start.
    //   - With aec_mode_=kAecOff, Application chooses kListeningModeAutoStop:
    //     the device listens *after* TTS playback ends, so there is no echo
    //     to cancel.  This sacrifices barge-in but keeps the audio pipeline
    //     within the S3's real-time budget.
    //
    // When a faster AEC (Speex FIXED_POINT, WebRTC AEC3, or ESP-SR) lands,
    // re-enable creation below and gate via Kconfig.
    //
    // The downstream code (line ~205) extracts the mic channel from 2-channel
    // input when aec_ is null, so this is a clean fall-through.
    speex_pool_mark();  // mark pool before NS so Reset() can rollback and reuse same DRAM
    ns_  = std::make_unique<OpenNS>(16000, 5, -15);  // 5ms→80samples; state ~14KB
    vad_ = std::make_unique<OpenVad>(2, 16000);
    output_buf_.reserve(frame_samples_);
    input_buf_.reserve(frame_samples_ * 4);  // keep in SRAM; prevents PSRAM realloc under load

    const bool aec_ok = !aec_ || aec_->is_valid();
    const bool ns_ok  = ns_->is_valid();
    const bool vad_ok = vad_->is_valid();
    ESP_LOGI(TAG, "init: frame=%dms (%d samples) aec=%s ns=%s vad=%s pool=%u/%u sram_free=%u",
             frame_duration_ms, frame_samples_,
             aec_ok ? (aec_ ? "yes" : "off") : "FAIL",
             ns_ok ? "yes" : "FAIL", vad_ok ? "yes" : "FAIL",
             speex_pool_usage(), speex_pool_capacity(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!aec_ok || !ns_ok || !vad_ok) {
        ESP_LOGE(TAG, "audio processor init partial-fail; pipeline will degrade gracefully");
    }

    static constexpr uint32_t kTaskStackWords = 4096 * 2;
    static constexpr size_t kTaskStackBytes = kTaskStackWords * sizeof(StackType_t);
    proc_task_stack_  = (StackType_t*)heap_caps_malloc(kTaskStackBytes, MALLOC_CAP_SPIRAM);
    proc_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    assert(proc_task_stack_ && proc_task_buffer_);
    task_exited_ = false;
    xTaskCreateStaticPinnedToCore([](void* arg) {
        static_cast<OpenAfeAudioProcessor*>(arg)->ProcessingTask();
        static_cast<OpenAfeAudioProcessor*>(arg)->task_exited_.store(true, std::memory_order_release);
        vTaskDelete(NULL);
    }, "open_afe_proc", kTaskStackWords, this, 6, proc_task_stack_, proc_task_buffer_, 1);
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

    // Wait up to 1s for the processing task to actually exit.  Polling avoids
    // the race where vTaskDelay(100) elapses before the task observed `stopped_`.
    constexpr int kMaxWaitMs = 1000;
    int waited_ms = 0;
    while (proc_task_stack_ && !task_exited_.load(std::memory_order_acquire) && waited_ms < kMaxWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    const bool task_done = task_exited_.load(std::memory_order_acquire);

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

    // Only free task memory if we confirmed the task exited; otherwise leak to
    // avoid use-after-free.  Returning before the free would also be fine, but
    // we still want the buffers above released.
    if (task_done) {
        if (proc_task_stack_)  { heap_caps_free(proc_task_stack_);  proc_task_stack_  = nullptr; }
        if (proc_task_buffer_) { heap_caps_free(proc_task_buffer_); proc_task_buffer_ = nullptr; }
    } else if (proc_task_stack_) {
        ESP_LOGE(TAG, "processing task did not exit within %dms; leaking %u-byte stack to avoid UAF",
                 kMaxWaitMs, (unsigned)(4096 * 2 * sizeof(StackType_t)));
    }
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
        if (aec_ && aec_->is_valid()) {
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
            // 2-channel without working AEC: extract mic channel (no echo cancellation)
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

        // No explicit yield here: the cv::wait at the top of the loop will
        // naturally block whenever input_buf_ is drained (~7ms between feeds),
        // giving IDLE0 enough time to reset the task watchdog.  An explicit
        // vTaskDelay here costs at least 1 tick (=10ms at 100Hz) which exceeds
        // the 10ms feed cadence and causes input backlog.
    }
}
