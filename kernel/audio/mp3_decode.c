/* AIOS v2 — MP3 Decoder wrapper (Phase 12)
 *
 * Wraps minimp3 single-header library.
 * Compiled with MP3_CFLAGS: no SSE, MINIMP3_NO_SIMD, warnings suppressed. */

#include "../../include/types.h"
#include "../../include/string.h"

/* Kernel heap */
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

/* minimp3 configuration — MUST be before include */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

struct mp3_decoder {
    mp3dec_t dec;
    const uint8_t *data;
    uint32_t size;
    uint32_t offset;
};

struct mp3_decoder *mp3_decoder_create(const uint8_t *data, uint32_t size) {
    struct mp3_decoder *d = (struct mp3_decoder *)kmalloc(sizeof(struct mp3_decoder));
    if (!d) return NULL;
    mp3dec_init(&d->dec);
    d->data = data;
    d->size = size;
    d->offset = 0;
    return d;
}

int mp3_decoder_decode_frame(struct mp3_decoder *dec, int16_t *pcm,
                              int *out_channels, int *out_sample_rate) {
    if (!dec || dec->offset >= dec->size) return 0;

    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&dec->dec,
                                       dec->data + dec->offset,
                                       (int)(dec->size - dec->offset),
                                       pcm, &info);

    if (info.frame_bytes > 0) {
        dec->offset += (uint32_t)info.frame_bytes;
    } else {
        /* No valid frame found — try skipping a byte */
        dec->offset++;
        return 0;
    }

    if (out_channels) *out_channels = info.channels;
    if (out_sample_rate) *out_sample_rate = info.hz;

    return samples;  /* Number of samples per channel */
}

bool mp3_decoder_eof(struct mp3_decoder *dec) {
    return !dec || dec->offset >= dec->size;
}

void mp3_decoder_destroy(struct mp3_decoder *dec) {
    if (dec) kfree(dec);
}
