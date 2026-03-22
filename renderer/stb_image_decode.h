/* ChaosGL stb_image integration — PNG/JPEG/BMP/GIF decode
 *
 * Wraps stb_image to decode standard image formats into BGRA pixels
 * (ChaosGL's native format). Compiled with RENDERER_CFLAGS (SSE2). */

#pragma once
#include "../include/types.h"

/* Image format detection results */
#define STBI_FMT_UNKNOWN  0
#define STBI_FMT_PNG      1
#define STBI_FMT_JPEG     2
#define STBI_FMT_BMP      3
#define STBI_FMT_GIF      4
#define STBI_FMT_RAWT     5

/* Detect image format from magic bytes at the start of a file.
 * buf must contain at least 8 bytes. */
int stbi_detect_format(const uint8_t *buf, int len);

/* Decode image from memory buffer, output BGRA pixels (ChaosGL native).
 * Returns kmalloc'd pixel buffer (caller must kfree), or NULL on error.
 * Sets *out_w, *out_h to image dimensions.
 * Sets *out_has_alpha to 1 if source image has alpha channel, 0 otherwise. */
uint8_t *stbi_decode_from_memory_bgra(const uint8_t *data, int len,
                                       int *out_w, int *out_h, int *out_has_alpha);

/* In-place RGBA -> BGRA channel swap for w*h pixels. */
void stbi_rgba_to_bgra(uint8_t *pixels, int w, int h);
