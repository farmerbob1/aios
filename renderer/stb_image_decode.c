/* ChaosGL stb_image wrapper — PNG/JPEG/BMP/GIF decode to BGRA
 *
 * Compiled with STB_CFLAGS (SSE2 enabled for float math).
 * Only called from task context where fxsave/fxrstor protects XMM state. */

#include "../include/types.h"
#include "../include/string.h"
#include "stb_image_decode.h"

extern void *kmalloc(size_t size);
extern void *krealloc(void *ptr, size_t size);
extern void  kfree(void *ptr);
extern void  serial_printf(const char *fmt, ...);

/* stb_image configuration */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS             /* No TLS in freestanding kernel */
#define STBI_MALLOC(sz)       kmalloc(sz)
#define STBI_REALLOC(p,newsz) krealloc(p,newsz)
#define STBI_FREE(p)          kfree(p)
#define STBI_ASSERT(x)        ((void)0)

#include "../vendor/stb/stb_image.h"

int stbi_detect_format(const uint8_t *buf, int len) {
    if (len < 4) return STBI_FMT_UNKNOWN;

    /* RAWT: magic 0x52415754 (little-endian: 'T','W','A','R') */
    if (buf[0] == 0x54 && buf[1] == 0x57 && buf[2] == 0x41 && buf[3] == 0x52)
        return STBI_FMT_RAWT;

    /* PNG: 89 50 4E 47 0D 0A 1A 0A */
    if (len >= 8 && buf[0] == 0x89 && buf[1] == 0x50 &&
        buf[2] == 0x4E && buf[3] == 0x47)
        return STBI_FMT_PNG;

    /* JPEG: FF D8 FF */
    if (len >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)
        return STBI_FMT_JPEG;

    /* BMP: 'BM' */
    if (buf[0] == 0x42 && buf[1] == 0x4D)
        return STBI_FMT_BMP;

    /* GIF: 'GIF' */
    if (len >= 3 && buf[0] == 0x47 && buf[1] == 0x49 && buf[2] == 0x46)
        return STBI_FMT_GIF;

    return STBI_FMT_UNKNOWN;
}

void stbi_rgba_to_bgra(uint8_t *pixels, int w, int h) {
    int count = w * h;
    for (int i = 0; i < count; i++) {
        uint8_t *p = pixels + i * 4;
        uint8_t tmp = p[0];  /* R */
        p[0] = p[2];         /* B -> byte 0 */
        p[2] = tmp;          /* R -> byte 2 */
    }
}

uint8_t *stbi_decode_from_memory_bgra(const uint8_t *data, int len,
                                       int *out_w, int *out_h, int *out_has_alpha) {
    int w, h, channels;

    /* Query original channel count to determine if alpha exists */
    if (!stbi_info_from_memory(data, len, &w, &h, &channels)) {
        serial_printf("[stbi] failed to read image info\n");
        return NULL;
    }

    int has_alpha = (channels == 4 || channels == 2); /* RGBA or grey+alpha */
    if (out_has_alpha) *out_has_alpha = has_alpha;

    /* Decode requesting 4 channels (RGBA) always */
    uint8_t *pixels = stbi_load_from_memory(data, len, &w, &h, &channels, 4);
    if (!pixels) {
        serial_printf("[stbi] decode failed: %s\n", stbi_failure_reason());
        return NULL;
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    /* Convert RGBA -> BGRA in-place */
    stbi_rgba_to_bgra(pixels, w, h);

    return pixels;
}

/* KAOS exports — let modules decode images independently */
#include "../include/kaos/export.h"
KAOS_EXPORT(stbi_decode_from_memory_bgra)
KAOS_EXPORT(stbi_rgba_to_bgra)
KAOS_EXPORT(stbi_detect_format)
