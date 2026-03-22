/* AIOS v2 — Audio Resampler (Phase 12)
 * Linear interpolation resampler. Good enough for hobby OS. */

#include "resample.h"

uint32_t resample_stereo(const int16_t *in, uint32_t in_frames,
                         uint32_t src_rate, uint32_t dst_rate,
                         int16_t *out, uint32_t out_max) {
    if (!in || !out || in_frames == 0 || src_rate == 0 || dst_rate == 0)
        return 0;

    /* If rates match, just copy */
    if (src_rate == dst_rate) {
        uint32_t n = in_frames < out_max ? in_frames : out_max;
        for (uint32_t i = 0; i < n * 2; i++)
            out[i] = in[i];
        return n;
    }

    /* Fixed-point interpolation (16.16) */
    uint32_t out_frames = 0;
    uint64_t step = ((uint64_t)src_rate << 16) / dst_rate;
    uint64_t pos = 0;

    while (out_frames < out_max) {
        uint32_t idx = (uint32_t)(pos >> 16);
        if (idx + 1 >= in_frames) break;

        uint32_t frac = (uint32_t)(pos & 0xFFFF);
        uint32_t inv = 0x10000 - frac;

        /* Left channel */
        int32_t l0 = in[idx * 2];
        int32_t l1 = in[(idx + 1) * 2];
        out[out_frames * 2] = (int16_t)((l0 * inv + l1 * frac) >> 16);

        /* Right channel */
        int32_t r0 = in[idx * 2 + 1];
        int32_t r1 = in[(idx + 1) * 2 + 1];
        out[out_frames * 2 + 1] = (int16_t)((r0 * inv + r1 * frac) >> 16);

        out_frames++;
        pos += step;
    }

    return out_frames;
}

uint32_t mono_to_stereo(const int16_t *in, uint32_t frames,
                        int16_t *out, uint32_t out_max) {
    uint32_t n = frames < out_max ? frames : out_max;
    for (uint32_t i = 0; i < n; i++) {
        out[i * 2]     = in[i];
        out[i * 2 + 1] = in[i];
    }
    return n;
}

void pcm8_to_pcm16(const uint8_t *in, int16_t *out, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        out[i] = (int16_t)((int16_t)in[i] - 128) << 8;
    }
}
