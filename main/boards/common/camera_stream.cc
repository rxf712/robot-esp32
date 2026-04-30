#include "camera_stream.h"

#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include <cstring>

#include "display/lvgl_display/jpg/image_to_jpeg.h"

#define TAG "CameraStream"

CameraStream::CameraStream(int fps, int jpeg_quality)
    : fps_(fps), jpeg_quality_(jpeg_quality) {
    jpeg_buf_ = (uint8_t*)heap_caps_malloc(kJpegBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf_) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
    }
}

CameraStream::~CameraStream() {
    Stop();
    if (encode_buf_) {
        heap_caps_free(encode_buf_);
        encode_buf_ = nullptr;
    }
    if (jpeg_buf_) {
        heap_caps_free(jpeg_buf_);
        jpeg_buf_ = nullptr;
    }
}

void CameraStream::SetFrameCallback(std::function<void(std::unique_ptr<VideoStreamPacket>)> cb) {
    on_frame_ = std::move(cb);
}

void CameraStream::Start() {
    if (timer_ || !jpeg_buf_) return;

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
    if (!timer_) return;
    esp_timer_stop(timer_);
    esp_timer_delete(timer_);
    timer_ = nullptr;
    ESP_LOGI(TAG, "Stopped");
}

void CameraStream::TimerCallback(void* arg) {
    static_cast<CameraStream*>(arg)->Capture();
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

    uint8_t* src = fb->buf;
    size_t src_len = fb->len;
    v4l2_pix_fmt_t fmt;
    switch (fb->format) {
        case PIXFORMAT_YUV422:    fmt = V4L2_PIX_FMT_YUYV;   break;
        case PIXFORMAT_GRAYSCALE: fmt = V4L2_PIX_FMT_GREY;   break;
        case PIXFORMAT_RGB565:    fmt = V4L2_PIX_FMT_RGB565; break;
        default:
            ESP_LOGE(TAG, "Unsupported pixel format: %d", fb->format);
            esp_camera_fb_return(fb);
            return;
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
