#include "open_ns.h"
#include "speex/speex_preprocess.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

extern "C" void speex_pool_rollback(void);

static const char* TAG = "OpenNS";

OpenNS::OpenNS(int sample_rate, int frame_ms, int noise_suppress_db)
    : sample_rate_(sample_rate),
      frame_size_(sample_rate * frame_ms / 1000),
      noise_suppress_db_(noise_suppress_db) {
    buf_.reserve(frame_size_);
    InitState();
    ESP_LOGI(TAG, "init: sr=%d frame=%d samples noise_suppress=%ddB",
             sample_rate_, frame_size_, noise_suppress_db_);
}

OpenNS::~OpenNS() {
    speex_preprocess_state_destroy(st_);
}

void OpenNS::InitState() {
    valid_ = false;
    if (st_) {
        speex_preprocess_state_destroy(st_);  /* frees any PSRAM fallback allocs */
        st_ = nullptr;
        speex_pool_rollback();                /* rewind pool to pre-NS mark */
    }
    size_t sram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    st_ = speex_preprocess_state_init(frame_size_, sample_rate_);
    if (!st_) {
        ESP_LOGE(TAG, "speex_preprocess_state_init OOM (pool exhausted); NS disabled");
        return;
    }
    size_t sram_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "NS state allocated: used %u bytes SRAM (free: %u→%u)",
             (unsigned)(sram_before - sram_after), (unsigned)sram_before, (unsigned)sram_after);
    int enable = 1;
    speex_preprocess_ctl(st_, SPEEX_PREPROCESS_SET_DENOISE, &enable);
    speex_preprocess_ctl(st_, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_suppress_db_);
    int disable = 0;
    speex_preprocess_ctl(st_, SPEEX_PREPROCESS_SET_AGC, &disable);
    speex_preprocess_ctl(st_, SPEEX_PREPROCESS_SET_VAD, &disable);
    valid_ = true;
}

void OpenNS::Feed(const int16_t* pcm, int samples, std::vector<int16_t>& out) {
    if (!valid_) {
        // NS unavailable: pass-through (caller still expects samples).
        out.insert(out.end(), pcm, pcm + samples);
        return;
    }
    buf_.insert(buf_.end(), pcm, pcm + samples);
    while ((int)buf_.size() >= frame_size_) {
        speex_preprocess_run(st_, buf_.data());
        out.insert(out.end(), buf_.begin(), buf_.begin() + frame_size_);
        buf_.erase(buf_.begin(), buf_.begin() + frame_size_);
    }
}

void OpenNS::Reset() {
    InitState();  /* pool rollback reuses same DRAM region, no PSRAM fallback */
    buf_.clear();
}
