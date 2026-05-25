/* DRAM pool for speexdsp (NS + AEC + resampler) state.
 *
 * Defined here as a zero-initialized global so it lands in .bss → DRAM at
 * link time, before any heap allocation ever happens.  The bump allocator in
 * os_support_custom.h serves all speex_alloc / speex_alloc_scratch calls from
 * this pool, guaranteeing fast DRAM regardless of runtime heap fragmentation.
 *
 * Pool sizing (48 KB) — fits the *small* states in DRAM and lets AEC overflow
 * to PSRAM:
 *   OpenResampler (24k→16k, ch=2, q=3) :   ~3 KB  → DRAM
 *   OpenNS        (frame=80, 16kHz)     :  ~14 KB  → DRAM
 *   OpenAec       (frame=160, filt=160) :  ~30-40 KB  → spills to PSRAM via
 *                                          os_support_custom.h fallback
 *   kiss_fft scratch (2× FFT)           :   ~3-5 KB  → DRAM
 *
 * Why 48 KB and not 80 KB:
 *   80 KB .bss starves WiFi/lwIP/mbedTLS of SRAM — TLS handshake fails at boot
 *   (observed: free sram=5391, min sram=23 → OTA / MQTT can't connect).
 *   The PSRAM fallback in os_support_custom.h:28 catches AEC overflow at the
 *   cost of slower (3-5×) memory access for AEC inner loops.
 *
 * Mark/rollback: speex_pool_mark() saves the current pool position.
 * speex_pool_rollback() rewinds to that mark, zeroing the reclaimed region.
 * Used by OpenNS::Reset() to reinitialize NS state in the same DRAM region
 * without growing the pool — avoids PSRAM fallback on second session.
 */

#include <string.h>

#define SPEEX_POOL_BYTES 49152u   /* 48 KB — keep in sync with os_support_custom.h */

unsigned char speex_pool_buf_[SPEEX_POOL_BYTES];  /* .bss → DRAM, zero at boot */
unsigned int  speex_pool_pos_;                     /* .bss → 0 at boot */

static unsigned int speex_pool_mark_ = 0;          /* .bss → 0 at boot */

unsigned int speex_pool_usage(void) { return speex_pool_pos_; }
unsigned int speex_pool_capacity(void) { return SPEEX_POOL_BYTES; }

void speex_pool_mark(void) { speex_pool_mark_ = speex_pool_pos_; }

void speex_pool_rollback(void) {
    if (speex_pool_pos_ > speex_pool_mark_) {
        memset(speex_pool_buf_ + speex_pool_mark_, 0,
               speex_pool_pos_ - speex_pool_mark_);
    }
    speex_pool_pos_ = speex_pool_mark_;
}
