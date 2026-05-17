#include "open_resampler.h"
#include "speex/speex_resampler.h"
#include <esp_log.h>
#include <inttypes.h>
#include <cstdlib>

static const char *TAG = "OpenResampler";

// Map esp complexity (1–3) to speexdsp quality (0–10).
// complexity 1 → quality 2 (fast, acceptable quality)
// complexity 2 → quality 3 (balanced; used by RATE_CVT_CFG default)
// complexity 3 → quality 5 (better quality, more CPU)
static int complexity_to_quality(int c) {
    if (c <= 1) return 2;
    if (c == 2) return 3;
    return 5;
}

struct OResamplerState {
    SpeexResamplerState *rs;
    uint32_t src_rate;
    uint32_t dest_rate;
    uint32_t channels;
    // --- diagnostics ---
    uint32_t call_count;
    uint64_t total_in;
    uint64_t total_out;
};

extern "C" {

esp_ae_err_t esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t *cfg, esp_ae_rate_cvt_handle_t *handle) {
    if (!cfg || !handle) return ESP_AE_ERR_INVALID_PARAMETER;

    int quality = complexity_to_quality(cfg->complexity);
    int err = RESAMPLER_ERR_SUCCESS;
    SpeexResamplerState *rs = speex_resampler_init(cfg->channel, cfg->src_rate,
                                                   cfg->dest_rate, quality, &err);
    if (err != RESAMPLER_ERR_SUCCESS || !rs) {
        ESP_LOGE(TAG, "speex_resampler_init failed: err=%d", err);
        *handle = nullptr;
        return ESP_AE_ERR_FAIL;
    }

    ESP_LOGI(TAG, "init: %u→%u Hz ch=%u quality=%d",
             cfg->src_rate, cfg->dest_rate, cfg->channel, quality);

    auto *s = static_cast<OResamplerState *>(malloc(sizeof(OResamplerState)));
    if (!s) {
        speex_resampler_destroy(rs);
        *handle = nullptr;
        return ESP_AE_ERR_MEM_LACK;
    }
    s->rs = rs;
    s->src_rate  = cfg->src_rate;
    s->dest_rate = cfg->dest_rate;
    s->channels  = cfg->channel;
    s->call_count = 0;
    s->total_in   = 0;
    s->total_out  = 0;
    *handle = s;
    return ESP_AE_ERR_OK;
}

esp_ae_err_t esp_ae_rate_cvt_get_max_out_sample_num(esp_ae_rate_cvt_handle_t handle,
                                                    uint32_t in_sample_num,
                                                    uint32_t *out_sample_num) {
    if (!handle || !out_sample_num) return ESP_AE_ERR_INVALID_PARAMETER;
    auto *s = static_cast<OResamplerState *>(handle);
    // Conservative upper bound: ratio + 8-sample margin for filter startup
    *out_sample_num = (uint32_t)((uint64_t)(in_sample_num + 8) * s->dest_rate / s->src_rate) + 8;
    return ESP_AE_ERR_OK;
}

esp_ae_err_t esp_ae_rate_cvt_process(esp_ae_rate_cvt_handle_t handle,
                                     esp_ae_sample_t in_samples, uint32_t in_sample_num,
                                     esp_ae_sample_t out_samples, uint32_t *out_sample_num) {
    if (!handle || !in_samples || !out_samples || !out_sample_num) {
        return ESP_AE_ERR_INVALID_PARAMETER;
    }
    auto *s = static_cast<OResamplerState *>(handle);

    spx_uint32_t in_len  = in_sample_num;   // frames (per channel)
    spx_uint32_t out_len = *out_sample_num; // max frames capacity

    int ret = speex_resampler_process_interleaved_int(
        s->rs,
        static_cast<const spx_int16_t *>(in_samples),  &in_len,
        static_cast<spx_int16_t *>(out_samples),        &out_len);

    if (ret != RESAMPLER_ERR_SUCCESS) {
        ESP_LOGE(TAG, "process failed: err=%d in=%u", ret, in_sample_num);
        *out_sample_num = 0;
        return ESP_AE_ERR_FAIL;
    }
    *out_sample_num = out_len;

    // Diagnostics — printed at DEBUG level; enable with:
    //   esp_log_level_set("OpenResampler", ESP_LOG_DEBUG);
    s->total_in  += in_sample_num;
    s->total_out += out_len;
    s->call_count++;
    ESP_LOGD(TAG, "call #%u: in=%u out=%u ratio=%.4f",
             s->call_count, in_sample_num, out_len,
             s->src_rate > 0 ? (float)s->total_out / (float)s->total_in
                                * (float)s->src_rate / (float)s->dest_rate : 0.0f);

    // Periodic summary every 200 calls — always at INFO level so it shows without menuconfig change
    if (s->call_count % 200 == 0) {
        float actual_ratio = (s->total_in > 0)
            ? (float)s->total_out / (float)s->total_in : 0.0f;
        float expected_ratio = (float)s->dest_rate / (float)s->src_rate;
        float drift_pct = (actual_ratio / expected_ratio - 1.0f) * 100.0f;
        ESP_LOGI(TAG, "diag: %u calls, in=%" PRIu64 " out=%" PRIu64 " drift=%.2f%%",
                 s->call_count, s->total_in, s->total_out, drift_pct);
    }

    return ESP_AE_ERR_OK;
}

esp_ae_err_t esp_ae_rate_cvt_reset(esp_ae_rate_cvt_handle_t handle) {
    if (!handle) return ESP_AE_ERR_INVALID_PARAMETER;
    auto *s = static_cast<OResamplerState *>(handle);
    speex_resampler_reset_mem(s->rs);
    ESP_LOGD(TAG, "reset (src=%u dest=%u)", s->src_rate, s->dest_rate);
    return ESP_AE_ERR_OK;
}

void esp_ae_rate_cvt_close(esp_ae_rate_cvt_handle_t handle) {
    if (!handle) return;
    auto *s = static_cast<OResamplerState *>(handle);
    ESP_LOGI(TAG, "close: %u calls, in=%" PRIu64 " out=%" PRIu64,
             s->call_count, s->total_in, s->total_out);
    speex_resampler_destroy(s->rs);
    free(s);
}

} // extern "C"
