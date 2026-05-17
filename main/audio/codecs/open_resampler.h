#pragma once

// Drop-in replacement for esp_ae_rate_cvt.h + esp_ae_types.h.
// Provides identical type names and function signatures so call sites in
// audio_service.cc compile unchanged when CONFIG_USE_OPEN_SOURCE_AUDIO is set.

#include <stdint.h>

// ---------- from esp_ae_types.h ----------

typedef void *esp_ae_sample_t;

typedef enum {
    ESP_AE_ERR_OK                = 0,
    ESP_AE_ERR_FAIL              = 1,
    ESP_AE_ERR_MEM_LACK          = 2,
    ESP_AE_ERR_INVALID_PARAMETER = 3,
} esp_ae_err_t;

// ---------- from esp_ae_rate_cvt.h ----------

typedef enum {
    ESP_AE_RATE_CVT_PERF_TYPE_MEMORY = 0,
    ESP_AE_RATE_CVT_PERF_TYPE_SPEED  = 1,
} esp_ae_rate_cvt_perf_type_t;

typedef void *esp_ae_rate_cvt_handle_t;

typedef struct {
    uint32_t                    src_rate;
    uint32_t                    dest_rate;
    uint8_t                     channel;
    uint8_t                     bits_per_sample;
    uint8_t                     complexity;       // maps to speexdsp quality (1→2, 2→3, 3→5)
    esp_ae_rate_cvt_perf_type_t perf_type;        // ignored; speexdsp has no equivalent
} esp_ae_rate_cvt_cfg_t;

// ---------- function declarations ----------

#ifdef __cplusplus
extern "C" {
#endif

esp_ae_err_t esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t *cfg, esp_ae_rate_cvt_handle_t *handle);
esp_ae_err_t esp_ae_rate_cvt_get_max_out_sample_num(esp_ae_rate_cvt_handle_t handle,
                                                    uint32_t in_sample_num,
                                                    uint32_t *out_sample_num);
esp_ae_err_t esp_ae_rate_cvt_process(esp_ae_rate_cvt_handle_t handle,
                                     esp_ae_sample_t in_samples, uint32_t in_sample_num,
                                     esp_ae_sample_t out_samples, uint32_t *out_sample_num);
esp_ae_err_t esp_ae_rate_cvt_reset(esp_ae_rate_cvt_handle_t handle);
void         esp_ae_rate_cvt_close(esp_ae_rate_cvt_handle_t handle);

#ifdef __cplusplus
}
#endif
