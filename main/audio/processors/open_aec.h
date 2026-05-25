#pragma once
#include <cstdint>

struct SpeexEchoState_;
typedef struct SpeexEchoState_ SpeexEchoState;

class OpenAec {
public:
    // frame_ms:   frame duration in ms (10 ms recommended for low latency)
    // filter_ms:  echo tail length in ms (200 ms handles most room echoes)
    OpenAec(int sample_rate, int frame_ms, int filter_ms);
    ~OpenAec();

    int frame_size() const { return frame_size_; }
    bool is_valid() const { return valid_; }

    // Process exactly frame_size() mono samples.
    // mic[frame_size] = near-end microphone, rec[frame_size] = far-end reference.
    // out[frame_size] = echo-cancelled output (must be pre-allocated by caller).
    void Process(const int16_t* mic, const int16_t* ref, int16_t* out);
    void Reset();

private:
    SpeexEchoState* echo_ = nullptr;
    int sample_rate_;
    int frame_size_;
    int filter_length_;
    bool valid_ = false;
};
