#pragma once
#include <functional>
#include <memory>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "protocol.h"

class CameraStream {
public:
    CameraStream(int fps = 5, int jpeg_quality = 65);
    ~CameraStream();

    void SetFrameCallback(std::function<void(std::unique_ptr<VideoStreamPacket>)> cb);
    void Start();
    void Stop();
    void Pause();   // stop timer only; task stays alive — cheap, no alloc/free
    void Resume();  // restart timer

private:
    int fps_;
    int jpeg_quality_;
    esp_timer_handle_t timer_        = nullptr;
    TaskHandle_t       capture_task_ = nullptr;
    std::function<void(std::unique_ptr<VideoStreamPacket>)> on_frame_;

    uint8_t* jpeg_buf_ = nullptr;
    uint8_t* rgb_buf_  = nullptr;   // pre-allocated: eliminates per-frame 900KB malloc/free
    static constexpr size_t kJpegBufSize = 32 * 1024;
    static constexpr size_t kRgbBufSize  = 320 * 240 * 3; // QVGA RGB888

    void Capture();
    static void TimerCallback(void* arg);
};
