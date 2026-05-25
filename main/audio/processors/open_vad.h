#pragma once
#include <cstdint>
#include <vector>

struct Fvad;

class OpenVad {
public:
    // mode: 0=quality, 1=low-bitrate, 2=aggressive, 3=very-aggressive
    // speech_frames: consecutive 30ms frames classified as speech before onset fires
    // silence_frames: consecutive 30ms frames of silence before offset fires
    OpenVad(int mode, int sample_rate, int speech_frames = 3, int silence_frames = 15);
    ~OpenVad();

    bool is_valid() const { return valid_; }

    // Feed arbitrary-length 16kHz mono PCM.
    // Returns true when speaking state changes; check IsSpeaking() for new state.
    bool Feed(const int16_t* pcm, int samples);
    bool IsSpeaking() const { return is_speaking_; }
    void Reset();

private:
    Fvad* vad_ = nullptr;
    int mode_;
    int sample_rate_;
    int sub_frame_samples_;  // 30ms worth of samples
    std::vector<int16_t> buf_;
    bool is_speaking_ = false;
    int speech_count_ = 0;
    int silence_count_ = 0;
    bool valid_ = false;

    int speech_frames_;
    int silence_frames_;
};
