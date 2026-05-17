#include "open_opus_codec.h"
#include "opus.h"
#include <esp_log.h>
#include <cstdlib>
#include <cstring>

static const char* TAG = "OpenOpus";

// Frame duration enum to 0.1-ms units (avoids float)
// Index 0..8 = 2.5, 5, 10, 20, 40, 60, 80, 100, 120 ms
static const int kFrameTenthsMs[] = {25, 50, 100, 200, 400, 600, 800, 1000, 1200};

static int frame_size_from_enum(int sample_rate, int e) {
    if (e < 0 || e >= 9) e = 5; // default 60ms
    return sample_rate * kFrameTenthsMs[e] / 10000;
}

static int app_to_opus(esp_opus_enc_application_t app) {
    switch (app) {
        case ESP_OPUS_ENC_APPLICATION_VOIP:     return OPUS_APPLICATION_VOIP;
        case ESP_OPUS_ENC_APPLICATION_LOWDELAY: return OPUS_APPLICATION_RESTRICTED_LOWDELAY;
        default:                                return OPUS_APPLICATION_AUDIO;
    }
}

struct OEncState {
    OpusEncoder *enc;
    int frame_size;   // samples per channel per frame
    int channels;
    int outbuf_size;  // max encoded bytes per frame
};

struct ODecState {
    OpusDecoder *dec;
    int frame_size;   // samples per channel per frame
    int channels;
};

extern "C" {

esp_audio_err_t esp_opus_enc_open(void *cfg_void, uint32_t, void **enc_hd) {
    auto *cfg = static_cast<esp_opus_enc_config_t *>(cfg_void);
    int err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(cfg->sample_rate, cfg->channel,
                                           app_to_opus(cfg->application_mode), &err);
    if (err != OPUS_OK || !enc) {
        ESP_LOGE(TAG, "opus_encoder_create: %s", opus_strerror(err));
        *enc_hd = nullptr;
        return ESP_AUDIO_ERR_FAIL;
    }

    if (cfg->bitrate == ESP_OPUS_BITRATE_AUTO) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(OPUS_AUTO));
    } else if (cfg->bitrate > 0) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(cfg->bitrate));
    }
    // complexity=0 means minimum CPU load; proprietary codec treats 0 as "default"
    // but for libopus on embedded we must use what was configured — 0 is fastest
    int complexity = (cfg->complexity >= 0 && cfg->complexity <= 10) ? cfg->complexity : 5;
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    ESP_LOGI(TAG, "encoder: sr=%d ch=%d complexity=%d", cfg->sample_rate, cfg->channel, complexity);
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(cfg->enable_fec ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_DTX(cfg->enable_dtx ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_VBR(cfg->enable_vbr ? 1 : 0));

    auto *s = new OEncState();
    s->enc = enc;
    s->channels = cfg->channel;
    s->frame_size = frame_size_from_enum(cfg->sample_rate, (int)cfg->frame_duration);
    s->outbuf_size = 4000;
    *enc_hd = s;
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size) {
    auto *s = static_cast<OEncState *>(enc_hd);
    *in_size  = s->frame_size * s->channels * (int)sizeof(int16_t);
    *out_size = s->outbuf_size;
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in,
                                     esp_audio_enc_out_frame_t *out) {
    auto *s = static_cast<OEncState *>(enc_hd);
    int bytes = opus_encode(s->enc, reinterpret_cast<const opus_int16 *>(in->buffer),
                            s->frame_size, out->buffer, (opus_int32)out->len);
    if (bytes < 0) {
        ESP_LOGE(TAG, "opus_encode: %s", opus_strerror(bytes));
        return ESP_AUDIO_ERR_FAIL;
    }
    out->encoded_bytes = (uint32_t)bytes;
    return ESP_AUDIO_ERR_OK;
}

void esp_opus_enc_close(void *enc_hd) {
    if (!enc_hd) return;
    auto *s = static_cast<OEncState *>(enc_hd);
    opus_encoder_destroy(s->enc);
    delete s;
}

esp_audio_err_t esp_opus_dec_open(void *cfg_void, uint32_t, void **dec_handle) {
    auto *cfg = static_cast<esp_opus_dec_cfg_t *>(cfg_void);
    int err = OPUS_OK;
    OpusDecoder *dec = opus_decoder_create((opus_int32)cfg->sample_rate, cfg->channel, &err);
    if (err != OPUS_OK || !dec) {
        ESP_LOGE(TAG, "opus_decoder_create: %s", opus_strerror(err));
        *dec_handle = nullptr;
        return ESP_AUDIO_ERR_FAIL;
    }
    auto *s = new ODecState();
    s->dec = dec;
    s->channels = cfg->channel;
    s->frame_size = frame_size_from_enum((int)cfg->sample_rate, (int)cfg->frame_duration);
    *dec_handle = s;
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw,
                                    esp_audio_dec_out_frame_t *frame,
                                    esp_audio_dec_info_t *) {
    auto *s = static_cast<ODecState *>(dec_handle);
    int max_samples = (int)(frame->len / sizeof(int16_t)) / s->channels;
    int samples = opus_decode(s->dec, raw->buffer, (opus_int32)raw->len,
                              reinterpret_cast<opus_int16 *>(frame->buffer), max_samples, 0);
    if (samples < 0) {
        ESP_LOGE(TAG, "opus_decode: %s", opus_strerror(samples));
        return ESP_AUDIO_ERR_FAIL;
    }
    frame->decoded_size = (uint32_t)(samples * s->channels * (int)sizeof(int16_t));
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_dec_reset(void *dec_handle) {
    if (!dec_handle) return ESP_AUDIO_ERR_OK;
    auto *s = static_cast<ODecState *>(dec_handle);
    opus_decoder_ctl(s->dec, OPUS_RESET_STATE);
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_dec_close(void *dec_handle) {
    if (!dec_handle) return ESP_AUDIO_ERR_OK;
    auto *s = static_cast<ODecState *>(dec_handle);
    opus_decoder_destroy(s->dec);
    delete s;
    return ESP_AUDIO_ERR_OK;
}

} // extern "C"
