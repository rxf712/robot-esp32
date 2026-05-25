#include "open_aec.h"
#include "speex/speex_echo.h"
#include <esp_log.h>

static const char* TAG = "OpenAec";

OpenAec::OpenAec(int sample_rate, int frame_ms, int filter_ms)
    : sample_rate_(sample_rate),
      frame_size_(sample_rate * frame_ms / 1000),
      filter_length_(sample_rate * filter_ms / 1000) {
    echo_ = speex_echo_state_init(frame_size_, filter_length_);
    if (!echo_) {
        ESP_LOGE(TAG, "speex_echo_state_init OOM (pool exhausted); AEC disabled");
        return;
    }
    speex_echo_ctl(echo_, SPEEX_ECHO_SET_SAMPLING_RATE, &sample_rate_);
    valid_ = true;
    ESP_LOGI(TAG, "init: sr=%d frame=%d filter=%d samples (%.0fms)",
             sample_rate_, frame_size_, filter_length_,
             (float)filter_ms);
}

OpenAec::~OpenAec() {
    if (echo_) speex_echo_state_destroy(echo_);
}

void OpenAec::Process(const int16_t* mic, const int16_t* ref, int16_t* out) {
    if (!valid_) return;
    speex_echo_cancellation(echo_,
        reinterpret_cast<const spx_int16_t*>(mic),
        reinterpret_cast<const spx_int16_t*>(ref),
        reinterpret_cast<spx_int16_t*>(out));
}

void OpenAec::Reset() {
    if (!valid_) return;
    // Reset filter state without freeing/reallocating: avoids heap fragmentation
    // that would push the next allocation into slow PSRAM.
    speex_echo_state_reset(echo_);
}
