#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <memory>

#include "audio_codec.h"
#include "wake_word.h"

class OpenVad;

class OpenAfeWakeWord : public WakeWord {
public:
    OpenAfeWakeWord();
    ~OpenAfeWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string&)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    AudioCodec* codec_ = nullptr;
    std::unique_ptr<OpenVad> vad_;

    std::vector<int16_t> input_buf_;
    std::mutex           input_mutex_;
    std::condition_variable input_cv_;
    bool running_ = false;
    bool stopped_ = false;

    // Rolling 2-second PCM ringbuffer for pre-wake context
    std::deque<std::vector<int16_t>> wake_word_pcm_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex           wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> wake_word_detected_callback_;
    std::atomic<int> warmup_samples_left_{0};  // discard first N samples after Start()

    StackType_t*  encode_task_stack_  = nullptr;
    StaticTask_t* encode_task_buffer_ = nullptr;
    std::atomic<bool> encode_task_running_{false};
    std::atomic<bool> detect_task_exited_{false};

    void DetectionTask();
    void StoreWakeWordData(const int16_t* data, int samples);
};
