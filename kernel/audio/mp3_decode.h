/* AIOS v2 — MP3 Decoder (Phase 12) */

#pragma once
#include "../../include/types.h"

#define MP3_MAX_SAMPLES_PER_FRAME (1152*2)

struct mp3_decoder;

/* Create decoder from file data already loaded in memory */
struct mp3_decoder *mp3_decoder_create(const uint8_t *data, uint32_t size);

/* Decode next frame. Returns number of int16 stereo samples (0 = EOF).
 * pcm buffer must hold at least 1152*2 int16 values. */
int mp3_decoder_decode_frame(struct mp3_decoder *dec, int16_t *pcm,
                              int *out_channels, int *out_sample_rate);

/* Check if decoder has consumed all data */
bool mp3_decoder_eof(struct mp3_decoder *dec);

/* Destroy decoder */
void mp3_decoder_destroy(struct mp3_decoder *dec);
