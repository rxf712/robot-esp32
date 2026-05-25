#pragma once

#include <atomic>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <memory>

#include "audio_processor.h"
#include "audio_codec.h"

class OpenAec;
class OpenNS;
class OpenVad;

class OpenAfeAudioProcessor : public AudioProcessor {
public:
    OpenAfeAudioProcessor();
    ~OpenAfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;
    void Deinitialize() override;

private:
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;

    std::unique_ptr<OpenAec> aec_;
    std::unique_ptr<OpenNS>  ns_;
    std::unique_ptr<OpenVad> vad_;

    // AEC scratch buffers (pre-allocated in Initialize)
    std::vector<int16_t> aec_mic_;
    std::vector<int16_t> aec_ref_;
    std::vector<int16_t> aec_out_;

    std::vector<int16_t> input_buf_;
    std::mutex           input_mutex_;
    std::condition_variable input_cv_;
    bool running_ = false;
    bool stopped_ = false;

    std::vector<int16_t> output_buf_;
    bool is_speaking_ = false;
    std::atomic<bool> reset_pending_{false};
    std::atomic<bool> task_exited_{false};

    StackType_t*  proc_task_stack_  = nullptr;
    StaticTask_t* proc_task_buffer_ = nullptr;

    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;

    void ProcessingTask();
};
