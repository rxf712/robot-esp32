#include "open_vad.h"
#include "fvad.h"
#include <esp_log.h>

static const char* TAG = "OpenVad";

OpenVad::OpenVad(int mode, int sample_rate, int speech_frames, int silence_frames)
    : mode_(mode), sample_rate_(sample_rate),
      sub_frame_samples_(sample_rate * 30 / 1000),
      speech_frames_(speech_frames), silence_frames_(silence_frames) {
    vad_ = fvad_new();
    if (!vad_) {
        ESP_LOGE(TAG, "fvad_new OOM; VAD disabled");
        return;
    }
    fvad_set_sample_rate(vad_, sample_rate_);
    fvad_set_mode(vad_, mode_);
    buf_.reserve(sub_frame_samples_);
    valid_ = true;
    ESP_LOGI(TAG, "init: mode=%d sr=%d onset=%dx30ms silence=%dx30ms",
             mode, sample_rate, speech_frames_, silence_frames_);
}

OpenVad::~OpenVad() {
    if (vad_) fvad_free(vad_);
}

bool OpenVad::Feed(const int16_t* pcm, int samples) {
    if (!valid_) return false;
    buf_.insert(buf_.end(), pcm, pcm + samples);
    bool changed = false;
    while ((int)buf_.size() >= sub_frame_samples_) {
        int r = fvad_process(vad_, buf_.data(), (size_t)sub_frame_samples_);
        buf_.erase(buf_.begin(), buf_.begin() + sub_frame_samples_);
        if (r < 0) {
            ESP_LOGW(TAG, "fvad_process: invalid frame");
            continue;
        }
        if (r == 1) {
            silence_count_ = 0;
            if (!is_speaking_ && ++speech_count_ >= speech_frames_) {
                is_speaking_ = true;
                speech_count_ = 0;
                changed = true;
                ESP_LOGD(TAG, "speech onset");
            }
        } else {
            speech_count_ = 0;
            if (is_speaking_ && ++silence_count_ >= silence_frames_) {
                is_speaking_ = false;
                silence_count_ = 0;
                changed = true;
                ESP_LOGD(TAG, "silence detected");
            }
        }
    }
    return changed;
}

void OpenVad::Reset() {
    if (!valid_) return;
    fvad_reset(vad_);
    fvad_set_sample_rate(vad_, sample_rate_);
    fvad_set_mode(vad_, mode_);
    buf_.clear();
    is_speaking_ = false;
    speech_count_ = 0;
    silence_count_ = 0;
}
