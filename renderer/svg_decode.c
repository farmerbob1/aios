/* ChaosGL NanoSVG wrapper — SVG decode to BGRA pixels
 *
 * Compiled with STB_CFLAGS (SSE2 enabled for float math).
 * Only called from task context where fxsave/fxrstor protects XMM state. */

#include "../include/types.h"
#include "../include/string.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void *kmalloc(size_t size);
extern void *krealloc(void *ptr, size_t size);
extern void  kfree(void *ptr);
extern void  serial_printf(const char *fmt, ...);

/* Minimal sscanf for NanoSVG color parsing:
 * Only needs: "#%2x%2x%2x", "#%1x%1x%1x", "rgb(%u, %u, %u)" */
int sscanf(const char *str, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int count = 0;

    /* "#%2x%2x%2x" — parse 6-digit hex color */
    if (fmt[0] == '#' && fmt[1] == '%' && fmt[2] == '2') {
        if (str[0] != '#') { __builtin_va_end(ap); return 0; }
        unsigned int *r = __builtin_va_arg(ap, unsigned int *);
        unsigned int *g = __builtin_va_arg(ap, unsigned int *);
        unsigned int *b = __builtin_va_arg(ap, unsigned int *);
        char buf[3];
        buf[2] = 0;
        buf[0] = str[1]; buf[1] = str[2];
        *r = (unsigned int)strtoul(buf, NULL, 16); count++;
        buf[0] = str[3]; buf[1] = str[4];
        *g = (unsigned int)strtoul(buf, NULL, 16); count++;
        buf[0] = str[5]; buf[1] = str[6];
        *b = (unsigned int)strtoul(buf, NULL, 16); count++;
    }
    /* "#%1x%1x%1x" — parse 3-digit hex color */
    else if (fmt[0] == '#' && fmt[1] == '%' && fmt[2] == '1') {
        if (str[0] != '#') { __builtin_va_end(ap); return 0; }
        unsigned int *r = __builtin_va_arg(ap, unsigned int *);
        unsigned int *g = __builtin_va_arg(ap, unsigned int *);
        unsigned int *b = __builtin_va_arg(ap, unsigned int *);
        char buf[2];
        buf[1] = 0;
        buf[0] = str[1]; *r = (unsigned int)strtoul(buf, NULL, 16); count++;
        buf[0] = str[2]; *g = (unsigned int)strtoul(buf, NULL, 16); count++;
        buf[0] = str[3]; *b = (unsigned int)strtoul(buf, NULL, 16); count++;
    }
    /* "rgb(%u, %u, %u)" */
    else if (fmt[0] == 'r' && fmt[1] == 'g' && fmt[2] == 'b') {
        const char *p = str;
        if (*p != 'r' || *(p+1) != 'g' || *(p+2) != 'b') { __builtin_va_end(ap); return 0; }
        p += 3;
        while (*p == ' ' || *p == '(') p++;
        unsigned int *r = __builtin_va_arg(ap, unsigned int *);
        unsigned int *g = __builtin_va_arg(ap, unsigned int *);
        unsigned int *b = __builtin_va_arg(ap, unsigned int *);
        *r = (unsigned int)strtoul(p, (char **)&p, 10); count++;
        while (*p == ' ' || *p == ',') p++;
        *g = (unsigned int)strtoul(p, (char **)&p, 10); count++;
        while (*p == ' ' || *p == ',') p++;
        *b = (unsigned int)strtoul(p, (char **)&p, 10); count++;
    }

    __builtin_va_end(ap);
    return count;
}

/* Float math functions NanoSVG needs */
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float acosf(float x) { return (float)acos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float roundf(float x) { return (float)round((double)x); }
float truncf(float x) { return (float)trunc((double)x); }

/* NanoSVG needs realloc, which we have via krealloc */
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#define NSVG_MALLOC(sz)       kmalloc(sz)
#define NSVG_REALLOC(p,newsz) krealloc(p,newsz)
#define NSVG_FREE(p)          kfree(p)

#include "../vendor/nanosvg/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "../vendor/nanosvg/nanosvgrast.h"

/* Decode SVG data to BGRA pixels.
 * Returns kmalloc'd pixel buffer, or NULL on error.
 * Caller must kfree the returned buffer. */
uint8_t *svg_decode_from_memory_bgra(const uint8_t *data, uint32_t len,
                                      int *out_w, int *out_h) {
    if (!data || len == 0) return NULL;

    /* NanoSVG needs a null-terminated mutable string */
    char *svg_str = (char *)kmalloc(len + 1);
    if (!svg_str) return NULL;
    memcpy(svg_str, data, len);
    svg_str[len] = '\0';

    /* Parse SVG */
    NSVGimage *image = nsvgParse(svg_str, "px", 96.0f);
    kfree(svg_str);
    if (!image) return NULL;

    int w = (int)image->width;
    int h = (int)image->height;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        /* Fallback: use reasonable size if SVG has no dimensions */
        if (w <= 0) w = 256;
        if (h <= 0) h = 256;
    }

    /* Rasterize */
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return NULL; }

    uint8_t *rgba = (uint8_t *)kmalloc(w * h * 4);
    if (!rgba) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return NULL;
    }

    nsvgRasterize(rast, image, 0, 0, 1.0f, rgba, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    /* Convert RGBA -> BGRA in-place */
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = rgba + i * 4;
        uint8_t tmp = p[0];
        p[0] = p[2];
        p[2] = tmp;
    }

    *out_w = w;
    *out_h = h;
    return rgba;
}

/* Detect if data is SVG (starts with '<svg' or '<?xml') */
int svg_detect(const uint8_t *data, uint32_t len) {
    if (len < 5) return 0;
    /* Skip whitespace/BOM */
    uint32_t i = 0;
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' ||
                        data[i] == '\r' || data[i] == 0xEF || data[i] == 0xBB ||
                        data[i] == 0xBF)) i++;
    if (i + 4 >= len) return 0;
    if (data[i] == '<' && data[i+1] == 's' && data[i+2] == 'v' && data[i+3] == 'g') return 1;
    if (data[i] == '<' && data[i+1] == '?' && data[i+2] == 'x' && data[i+3] == 'm' && data[i+4] == 'l') return 1;
    return 0;
}
