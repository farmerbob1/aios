/* AIOS v2 — Audio Resampler (Phase 12) */

#pragma once
#include "../../include/types.h"

/* Resample stereo int16 PCM from src_rate to dst_rate using linear interpolation.
 * in: interleaved stereo (L,R,L,R...), in_frames = number of stereo frame pairs.
 * out: output buffer, out_max = max stereo frame pairs to write.
 * Returns number of output stereo frames written. */
uint32_t resample_stereo(const int16_t *in, uint32_t in_frames,
                         uint32_t src_rate, uint32_t dst_rate,
                         int16_t *out, uint32_t out_max);

/* Convert mono to stereo (duplicate each sample). Returns frame count. */
uint32_t mono_to_stereo(const int16_t *in, uint32_t frames,
                        int16_t *out, uint32_t out_max);

/* Convert 8-bit unsigned to 16-bit signed in-place equivalent.
 * Writes to out (can't be same as in). */
void pcm8_to_pcm16(const uint8_t *in, int16_t *out, uint32_t count);
