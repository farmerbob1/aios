/* AIOS v2 — WAV Decoder (Phase 12) */

#pragma once
#include "../../include/types.h"

struct wav_info {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_offset;    /* Byte offset to PCM data in file */
    uint32_t data_size;      /* Size of PCM data in bytes */
};

/* Parse WAV header. Returns 0 on success, -1 on error. */
int wav_parse_header(const uint8_t *data, uint32_t len, struct wav_info *info);
