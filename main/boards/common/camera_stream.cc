#include "camera_stream.h"

#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include <cstring>

#include "display/lvgl_display/jpg/image_to_jpeg.h"

#define TAG "CameraStream"

// Notification values for the capture task.
static constexpr uint32_t kNotifyCapture = 1;
static constexpr uint32_t kNotifyStop    = 0xDEADBEEF;

// YUYV (YCbCr 4:2:2 interleaved) → RGB888, BT.601 fixed-point (<<10).
// Output must be width*height*3 bytes.
static void yuyv_to_rgb888_local(const uint8_t* yuyv, uint8_t* rgb, int width, int height) {
    int n = width * height / 2;
    for (int i = 0; i < n; i++) {
        int y0 = yuyv[0], cb = yuyv[1] - 128;
        int y1 = yuyv[2], cr = yuyv[3] - 128;
        yuyv += 4;
        int r = (1436 * cr) >> 10;
        int g = (352 * cb + 731 * cr) >> 10;
        int b = (1814 * cb) >> 10;
        auto clamp = [](int v) -> uint8_t { return v<0?0:v>255?255:(uint8_t)v; };
        rgb[0]=clamp(y0+r); rgb[1]=clamp(y0-g); rgb[2]=clamp(y0+b); rgb+=3;
        rgb[0]=clamp(y1+r); rgb[1]=clamp(y1-g); rgb[2]=clamp(y1+b); rgb+=3;
    }
}

CameraStream::CameraStream(int fps, int jpeg_quality)
    : fps_(fps), jpeg_quality_(jpeg_quality) {
    jpeg_buf_ = (uint8_t*)heap_caps_malloc(kJpegBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf_) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
    }
    // Pre-allocate the RGB888 intermediate buffer once to avoid 900KB per-frame
    // malloc/free which fragments PSRAM after hundreds of frames.
    rgb_buf_ = (uint8_t*)heap_caps_malloc(kRgbBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb_buf_) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer");
    }
}

CameraStream::~CameraStream() {
    Stop();
    if (jpeg_buf_) { heap_caps_free(jpeg_buf_); jpeg_buf_ = nullptr; }
    if (rgb_buf_)  { heap_caps_free(rgb_buf_);  rgb_buf_  = nullptr; }
}

void CameraStream::SetFrameCallback(std::function<void(std::unique_ptr<VideoStreamPacket>)> cb) {
    on_frame_ = std::move(cb);
}

void CameraStream::Start() {
    if (capture_task_ || !jpeg_buf_) return;

    // Dedicated task: stb JPEG encoding for 640×480 YUYV needs ~8KB stack.
    // esp_timer callback has only ~3.5KB — not enough for stb internals.
    xTaskCreatePinnedToCore([](void* arg) {
        auto* self = static_cast<CameraStream*>(arg);
        while (true) {
            uint32_t val = 0;
            xTaskNotifyWait(0, 0xFFFFFFFF, &val, portMAX_DELAY);
            if (val == kNotifyStop) break;
            self->Capture();
        }
        vTaskDelete(NULL);
    }, "cam_capture", 4096 * 3, this, 5, &capture_task_, 1);

    esp_timer_create_args_t args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cam_stream",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    uint64_t interval_us = 1000000ULL / fps_;
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, interval_us));
    ESP_LOGI(TAG, "Started at %d fps", fps_);
}

void CameraStream::Stop() {
    if (timer_) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (capture_task_) {
        xTaskNotify(capture_task_, kNotifyStop, eSetValueWithOverwrite);
        // Give the task time to finish the current frame and exit cleanly.
        vTaskDelay(pdMS_TO_TICKS(300));
        capture_task_ = nullptr;
    }
    ESP_LOGI(TAG, "Stopped");
}

void CameraStream::Pause() {
    if (timer_) {
        esp_timer_stop(timer_);
        ESP_LOGD(TAG, "Paused");
    }
}

void CameraStream::Resume() {
    if (timer_ && capture_task_) {
        uint64_t interval_us = 1000000ULL / fps_;
        esp_timer_start_periodic(timer_, interval_us);
        ESP_LOGD(TAG, "Resumed at %d fps", fps_);
    }
}

void CameraStream::TimerCallback(void* arg) {
    auto* self = static_cast<CameraStream*>(arg);
    if (self->capture_task_) {
        // eSetValueWithOverwrite: if previous frame not yet processed, drop it.
        xTaskNotify(self->capture_task_, kNotifyCapture, eSetValueWithOverwrite);
    }
}

void CameraStream::Capture() {
    if (!on_frame_ || !jpeg_buf_) return;

    // Flush stale frames to get the latest
    camera_fb_t* fb = nullptr;
    for (int i = 0; i < 2; i++) {
        if (fb) esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "esp_camera_fb_get failed");
            return;
        }
    }

    uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // For YUYV: pre-convert to RGB888 into the pre-allocated rgb_buf_ so that
    // encode_with_stb receives RGB24 and skips its internal 900KB malloc.
    uint8_t* src = fb->buf;
    size_t src_len = fb->len;
    v4l2_pix_fmt_t fmt;
    if (fb->format == PIXFORMAT_YUV422 && rgb_buf_) {
        yuyv_to_rgb888_local(fb->buf, rgb_buf_, fb->width, fb->height);
        src     = rgb_buf_;
        src_len = (size_t)fb->width * fb->height * 3;
        fmt     = V4L2_PIX_FMT_RGB24;
    } else {
        switch (fb->format) {
            case PIXFORMAT_GRAYSCALE: fmt = V4L2_PIX_FMT_GREY;   break;
            case PIXFORMAT_RGB565:    fmt = V4L2_PIX_FMT_RGB565; break;
            default:
                ESP_LOGE(TAG, "Unsupported pixel format: %d", fb->format);
                esp_camera_fb_return(fb);
                return;
        }
    }

    struct JpegCtx {
        uint8_t* buf;
        size_t offset;
        size_t max_size;
    } ctx = { jpeg_buf_, 0, kJpegBufSize };

    bool ok = image_to_jpeg_cb(src, src_len, fb->width, fb->height, fmt, jpeg_quality_,
        [](void* arg, size_t /*index*/, const void* data, size_t len) -> size_t {
            auto* c = static_cast<JpegCtx*>(arg);
            if (data && len > 0 && c->offset + len <= c->max_size) {
                memcpy(c->buf + c->offset, data, len);
                c->offset += len;
            }
            return len;
        }, &ctx);

    esp_camera_fb_return(fb);

    if (!ok || ctx.offset == 0) {
        ESP_LOGW(TAG, "JPEG encode failed");
        return;
    }

    auto pkt = std::make_unique<VideoStreamPacket>();
    pkt->timestamp_ms = timestamp_ms;
    pkt->keyframe = true;
    pkt->jpeg_payload.assign(jpeg_buf_, jpeg_buf_ + ctx.offset);

    ESP_LOGD(TAG, "Frame: %zu bytes, ts=%lu ms", ctx.offset, (unsigned long)timestamp_ms);
    on_frame_(std::move(pkt));
}
