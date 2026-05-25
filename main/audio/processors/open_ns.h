#pragma once
#include <cstdint>
#include <vector>

struct SpeexPreprocessState_;
typedef struct SpeexPreprocessState_ SpeexPreprocessState;

class OpenNS {
public:
    // frame_ms: sub-frame duration (10/20/30 ms); noise_suppress_db: negative, e.g. -25
    OpenNS(int sample_rate, int frame_ms, int noise_suppress_db);
    ~OpenNS();

    bool is_valid() const { return valid_; }

    // Feed arbitrary-length PCM; NS-processed samples appended to `out`.
    // Internally slices into fixed-size frames; leftover samples buffered until next call.
    void Feed(const int16_t* pcm, int samples, std::vector<int16_t>& out);
    void Reset();

private:
    SpeexPreprocessState* st_ = nullptr;
    int sample_rate_;
    int frame_size_;
    int noise_suppress_db_;
    std::vector<int16_t> buf_;
    bool valid_ = false;

    void InitState();
};
