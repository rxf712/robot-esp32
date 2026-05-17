#pragma once

// Drop-in replacement for esp_opus_enc.h + esp_opus_dec.h + esp_audio_enc.h.
// Provides the same type names and function signatures so that audio_service.h
// and call sites compile unchanged when CONFIG_USE_OPEN_SOURCE_AUDIO is set.
//
// esp_audio_types.h (for esp_audio_err_t, ESP_AUDIO_ERR_OK, ESP_AUDIO_MONO,
// ESP_AUDIO_SAMPLE_RATE_16K, etc.) is still included by the caller; we do not
// redefine those here.

#include <stdint.h>
#include <stdbool.h>
#include "esp_audio_types.h"

// ---------- from esp_audio_enc.h ----------

typedef struct {
    uint8_t *buffer;
    uint32_t len;
} esp_audio_enc_in_frame_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    uint32_t encoded_bytes;
} esp_audio_enc_out_frame_t;

// ---------- from esp_audio_dec.h ----------

#define ESP_AUDIO_DEC_RECOVERY_NONE 0

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    uint32_t consumed;
    int      frame_recover;
} esp_audio_dec_in_raw_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    uint32_t decoded_size;
} esp_audio_dec_out_frame_t;

typedef struct {
    int dummy;
} esp_audio_dec_info_t;

// ---------- from esp_opus_enc.h ----------

#define ESP_OPUS_BITRATE_AUTO (-1000)

typedef enum {
    ESP_OPUS_ENC_FRAME_DURATION_ARG    = -1,
    ESP_OPUS_ENC_FRAME_DURATION_2_5_MS = 0,
    ESP_OPUS_ENC_FRAME_DURATION_5_MS   = 1,
    ESP_OPUS_ENC_FRAME_DURATION_10_MS  = 2,
    ESP_OPUS_ENC_FRAME_DURATION_20_MS  = 3,
    ESP_OPUS_ENC_FRAME_DURATION_40_MS  = 4,
    ESP_OPUS_ENC_FRAME_DURATION_60_MS  = 5,
    ESP_OPUS_ENC_FRAME_DURATION_80_MS  = 6,
    ESP_OPUS_ENC_FRAME_DURATION_100_MS = 7,
    ESP_OPUS_ENC_FRAME_DURATION_120_MS = 8,
} esp_opus_enc_frame_duration_t;

typedef enum {
    ESP_OPUS_ENC_APPLICATION_ARG      = -1,
    ESP_OPUS_ENC_APPLICATION_VOIP     = 0,
    ESP_OPUS_ENC_APPLICATION_AUDIO    = 1,
    ESP_OPUS_ENC_APPLICATION_LOWDELAY = 2,
} esp_opus_enc_application_t;

typedef struct {
    int                           sample_rate;
    int                           channel;
    int                           bits_per_sample;
    int                           bitrate;
    esp_opus_enc_frame_duration_t frame_duration;
    esp_opus_enc_application_t    application_mode;
    int                           complexity;
    bool                          enable_fec;
    bool                          enable_dtx;
    bool                          enable_vbr;
} esp_opus_enc_config_t;

// ---------- from esp_opus_dec.h ----------

typedef enum {
    ESP_OPUS_DEC_FRAME_DURATION_INVALID = -1,
    ESP_OPUS_DEC_FRAME_DURATION_2_5_MS  = 0,
    ESP_OPUS_DEC_FRAME_DURATION_5_MS    = 1,
    ESP_OPUS_DEC_FRAME_DURATION_10_MS   = 2,
    ESP_OPUS_DEC_FRAME_DURATION_20_MS   = 3,
    ESP_OPUS_DEC_FRAME_DURATION_40_MS   = 4,
    ESP_OPUS_DEC_FRAME_DURATION_60_MS   = 5,
    ESP_OPUS_DEC_FRAME_DURATION_80_MS   = 6,
    ESP_OPUS_DEC_FRAME_DURATION_100_MS  = 7,
    ESP_OPUS_DEC_FRAME_DURATION_120_MS  = 8,
} esp_opus_dec_frame_duration_t;

typedef struct {
    uint32_t                      sample_rate;
    uint8_t                       channel;
    esp_opus_dec_frame_duration_t frame_duration;
    bool                          self_delimited;
} esp_opus_dec_cfg_t;

// ---------- function declarations ----------

#ifdef __cplusplus
extern "C" {
#endif

esp_audio_err_t esp_opus_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);
esp_audio_err_t esp_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);
esp_audio_err_t esp_opus_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame,
                                     esp_audio_enc_out_frame_t *out_frame);
void            esp_opus_enc_close(void *enc_hd);

esp_audio_err_t esp_opus_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle);
esp_audio_err_t esp_opus_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw,
                                    esp_audio_dec_out_frame_t *frame,
                                    esp_audio_dec_info_t *dec_info);
esp_audio_err_t esp_opus_dec_reset(void *dec_handle);
esp_audio_err_t esp_opus_dec_close(void *dec_handle);

#ifdef __cplusplus
}
#endif
