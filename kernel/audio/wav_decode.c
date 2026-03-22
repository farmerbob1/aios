/* AIOS v2 — WAV Header Parser (Phase 12) */

#include "wav_decode.h"
#include "../../include/string.h"

static uint16_t read16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t read32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

int wav_parse_header(const uint8_t *data, uint32_t len, struct wav_info *info) {
    if (!data || !info || len < 44) return -1;

    /* RIFF header */
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F')
        return -1;
    if (data[8] != 'W' || data[9] != 'A' || data[10] != 'V' || data[11] != 'E')
        return -1;

    /* Search for "fmt " and "data" chunks (handles non-standard ordering) */
    uint32_t pos = 12;
    bool found_fmt = false;
    bool found_data = false;

    while (pos + 8 <= len) {
        uint32_t chunk_id = read32(data + pos);
        uint32_t chunk_size = read32(data + pos + 4);

        if (chunk_id == 0x20746D66) {  /* "fmt " */
            if (pos + 8 + 16 > len) return -1;
            uint16_t format = read16(data + pos + 8);
            if (format != 1) return -1;  /* Only PCM supported */
            info->channels = read16(data + pos + 10);
            info->sample_rate = read32(data + pos + 12);
            info->bits_per_sample = read16(data + pos + 22);
            found_fmt = true;
        } else if (chunk_id == 0x61746164) {  /* "data" */
            info->data_offset = pos + 8;
            info->data_size = chunk_size;
            found_data = true;
        }

        if (found_fmt && found_data) break;
        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++;  /* Chunks are word-aligned */
    }

    if (!found_fmt || !found_data) return -1;
    if (info->channels < 1 || info->channels > 2) return -1;
    if (info->bits_per_sample != 8 && info->bits_per_sample != 16) return -1;

    return 0;
}
