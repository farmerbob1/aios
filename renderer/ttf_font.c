/* ChaosGL TrueType Font — stb_truetype wrapper + glyph cache + rendering
 *
 * Compiled with STBTT_CFLAGS (SSE2 enabled for float rasterization).
 * Only called from task context where fxsave/fxrstor protects XMM state. */

#include "../include/types.h"
#include "../include/string.h"
#include "ttf_font.h"
#include "surface.h"
#include "font.h"  /* CLAUDE_MONO_WIDTH/HEIGHT for bitmap fallback metrics */

extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);
extern void  serial_printf(const char *fmt, ...);

/* ChaosFS file operations */
#define CHAOS_O_RDONLY 0x01
extern int  chaos_open(const char *path, int flags);
extern int  chaos_read(int fd, void *buf, uint32_t len);
extern void chaos_close(int fd);

struct chaos_stat {
    uint32_t inode;
    uint16_t mode;
    uint64_t size;
    uint32_t block_count;
    uint32_t created_time;
    uint32_t modified_time;
};
extern int chaos_stat(const char *path, struct chaos_stat *st);

/* stb_truetype configuration */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x,u)    ((void)(u), kmalloc(x))
#define STBTT_free(x,u)      ((void)(u), kfree(x))
#define STBTT_assert(x)      ((void)0)

#include "../vendor/stb/stb_truetype.h"

/* ── Data Structures ─────────────────────────────────── */

typedef struct {
    uint8_t *bitmap;        /* kmalloc'd 8-bit alpha bitmap */
    int      width, height;
    int      xoff, yoff;    /* bearing offsets from stbtt */
    int      advance;       /* horizontal advance in pixels */
    bool     cached;
} ttf_glyph_cache_t;

typedef struct {
    stbtt_fontinfo     info;
    uint8_t           *ttf_data;     /* raw TTF file (must persist for stbtt) */
    float              scale;
    int                ascent;       /* pixel ascent */
    int                descent;      /* pixel descent (positive value) */
    int                line_gap;
    int                height_px;    /* total line height */
    ttf_glyph_cache_t  glyphs[CHAOS_GL_GLYPH_COUNT];
    bool               in_use;
} ttf_font_t;

static ttf_font_t fonts[CHAOS_GL_MAX_FONTS];
static int system_font_handle = -1;

/* ── Pixel blending (duplicated from 2d.c for inlining) ── */

static inline void ttf_put_pixel_blend(chaos_gl_surface_t *s, int x, int y, uint32_t color) {
    rect_t clip = s->clip_stack[s->clip_depth - 1];
    if (x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h) return;

    uint32_t a = (color >> 24) & 0xFF;
    if (a == 0) return;

    uint32_t *dst = &s->bufs[1 - s->buf_index][y * s->width + x];
    if (a == 255) {
        *dst = color;
        return;
    }

    uint32_t inv_a = 255 - a;
    uint32_t d = *dst;
    uint32_t rb_src = color & 0x00FF00FF;
    uint32_t g_src  = color & 0x0000FF00;
    uint32_t rb_dst = d & 0x00FF00FF;
    uint32_t g_dst  = d & 0x0000FF00;

    uint32_t rb = ((rb_src * a + rb_dst * inv_a + 0x00800080) >> 8) & 0x00FF00FF;
    uint32_t g  = ((g_src  * a + g_dst  * inv_a + 0x00000080) >> 8) & 0x0000FF00;
    *dst = rb | g;
}

/* ── Helper: resolve handle (-1 = system font) ────────── */

static inline int resolve_handle(int handle) {
    if (handle == -1) return system_font_handle;
    return handle;
}

static inline ttf_font_t *get_font(int handle) {
    handle = resolve_handle(handle);
    if (handle < 0 || handle >= CHAOS_GL_MAX_FONTS) return NULL;
    if (!fonts[handle].in_use) return NULL;
    return &fonts[handle];
}

/* ── Init ────────────────────────────────────────────── */

void chaos_gl_font_init(void) {
    memset(fonts, 0, sizeof(fonts));
    system_font_handle = -1;
    serial_printf("[ttf] font subsystem initialized\n");
}

/* ── Load ────────────────────────────────────────────── */

int chaos_gl_font_load(const char *path, float size_px) {
    /* Find free slot */
    int handle = -1;
    for (int i = 0; i < CHAOS_GL_MAX_FONTS; i++) {
        if (!fonts[i].in_use) {
            handle = i;
            break;
        }
    }
    if (handle < 0) {
        serial_printf("[ttf] no free font slots\n");
        return -1;
    }

    /* Open and read TTF file from ChaosFS */
    int fd = chaos_open(path, CHAOS_O_RDONLY);
    if (fd < 0) {
        serial_printf("[ttf] failed to open '%s'\n", path);
        return -1;
    }

    struct chaos_stat st;
    if (chaos_stat(path, &st) < 0 || st.size == 0) {
        serial_printf("[ttf] failed to stat '%s'\n", path);
        chaos_close(fd);
        return -1;
    }

    uint32_t file_size = (uint32_t)st.size;
    uint8_t *ttf_data = (uint8_t *)kmalloc(file_size);
    if (!ttf_data) {
        serial_printf("[ttf] kmalloc(%u) failed\n", file_size);
        chaos_close(fd);
        return -1;
    }

    uint32_t total_read = 0;
    while (total_read < file_size) {
        int r = chaos_read(fd, ttf_data + total_read, file_size - total_read);
        if (r <= 0) break;
        total_read += (uint32_t)r;
    }
    chaos_close(fd);

    if (total_read < 64) {
        serial_printf("[ttf] file too small (%u bytes)\n", total_read);
        kfree(ttf_data);
        return -1;
    }

    /* Initialize stb_truetype */
    ttf_font_t *font = &fonts[handle];
    memset(font, 0, sizeof(*font));
    font->ttf_data = ttf_data;

    if (!stbtt_InitFont(&font->info, ttf_data, 0)) {
        serial_printf("[ttf] stbtt_InitFont failed for '%s'\n", path);
        kfree(ttf_data);
        memset(font, 0, sizeof(*font));
        return -1;
    }

    /* Compute scale and metrics */
    font->scale = stbtt_ScaleForPixelHeight(&font->info, size_px);

    int raw_ascent, raw_descent, raw_line_gap;
    stbtt_GetFontVMetrics(&font->info, &raw_ascent, &raw_descent, &raw_line_gap);

    font->ascent   = (int)(raw_ascent * font->scale + 0.5f);
    font->descent  = (int)(-raw_descent * font->scale + 0.5f);  /* make positive */
    font->line_gap = (int)(raw_line_gap * font->scale + 0.5f);
    font->height_px = font->ascent + font->descent + font->line_gap;

    /* Pre-rasterize ASCII 32-126 glyphs */
    for (int c = CHAOS_GL_GLYPH_FIRST; c <= CHAOS_GL_GLYPH_LAST; c++) {
        int idx = c - CHAOS_GL_GLYPH_FIRST;
        ttf_glyph_cache_t *g = &font->glyphs[idx];

        int advance_raw, lsb;
        stbtt_GetCodepointHMetrics(&font->info, c, &advance_raw, &lsb);
        g->advance = (int)(advance_raw * font->scale + 0.5f);

        g->bitmap = stbtt_GetCodepointBitmap(&font->info, 0, font->scale,
                                              c, &g->width, &g->height,
                                              &g->xoff, &g->yoff);
        g->cached = true;
    }

    font->in_use = true;

    return handle;
}

/* ── Free ────────────────────────────────────────────── */

void chaos_gl_font_free(int handle) {
    if (handle < 0 || handle >= CHAOS_GL_MAX_FONTS) return;
    ttf_font_t *font = &fonts[handle];
    if (!font->in_use) return;

    /* Free cached glyph bitmaps */
    for (int i = 0; i < CHAOS_GL_GLYPH_COUNT; i++) {
        if (font->glyphs[i].bitmap) {
            kfree(font->glyphs[i].bitmap);
        }
    }

    /* Free TTF data */
    if (font->ttf_data) {
        kfree(font->ttf_data);
    }

    if (system_font_handle == handle) {
        system_font_handle = -1;
    }

    memset(font, 0, sizeof(*font));
}

/* ── System Font ─────────────────────────────────────── */

void chaos_gl_set_font(int handle) {
    system_font_handle = handle;
}

int chaos_gl_get_font(void) {
    return system_font_handle;
}

/* ── Metrics ─────────────────────────────────────────── */

int chaos_gl_font_height(int handle) {
    ttf_font_t *font = get_font(handle);
    if (!font) return CLAUDE_MONO_HEIGHT;  /* bitmap fallback */
    return font->height_px;
}

int chaos_gl_font_ascent(int handle) {
    ttf_font_t *font = get_font(handle);
    if (!font) return CLAUDE_MONO_HEIGHT;
    return font->ascent;
}

int chaos_gl_font_descent(int handle) {
    ttf_font_t *font = get_font(handle);
    if (!font) return 0;
    return font->descent;
}

int chaos_gl_font_text_width(int handle, const char *str) {
    ttf_font_t *font = get_font(handle);
    if (!font) return (int)strlen(str) * CLAUDE_MONO_WIDTH;  /* bitmap fallback */

    int width = 0;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c >= CHAOS_GL_GLYPH_FIRST && c <= CHAOS_GL_GLYPH_LAST) {
            width += font->glyphs[c - CHAOS_GL_GLYPH_FIRST].advance;
        } else {
            /* Unknown char — use space width */
            width += font->glyphs[0].advance;  /* space = glyph 0 */
        }
        str++;
    }
    return width;
}

int chaos_gl_font_char_width(int handle, char c) {
    ttf_font_t *font = get_font(handle);
    if (!font) return CLAUDE_MONO_WIDTH;

    uint8_t ch = (uint8_t)c;
    if (ch >= CHAOS_GL_GLYPH_FIRST && ch <= CHAOS_GL_GLYPH_LAST) {
        return font->glyphs[ch - CHAOS_GL_GLYPH_FIRST].advance;
    }
    return font->glyphs[0].advance;
}

int chaos_gl_font_text_height_wrapped(int handle, int max_w, const char *str) {
    ttf_font_t *font = get_font(handle);
    if (!font) {
        /* Bitmap fallback — replicate existing logic */
        int cx = 0, lines = 1;
        int cw = CLAUDE_MONO_WIDTH, ch = CLAUDE_MONO_HEIGHT;
        const char *p = str;
        while (*p) {
            if (*p == '\n') { cx = 0; lines++; p++; continue; }
            if (*p == ' ') {
                if (cx + cw > max_w && cx > 0) { cx = 0; lines++; }
                cx += cw; p++; continue;
            }
            int wl = 0;
            const char *ws = p;
            while (*p && *p != ' ' && *p != '\n') { wl++; p++; }
            int ww = wl * cw;
            if (cx > 0 && cx + ww > max_w) { cx = 0; lines++; }
            for (int i = 0; i < wl; i++) {
                if (cx + cw > max_w && cx > 0) { cx = 0; lines++; }
                cx += cw;
            }
            (void)ws;
        }
        return lines * ch;
    }

    int cx = 0, lines = 1;
    int line_h = font->height_px;
    const char *p = str;

    while (*p) {
        if (*p == '\n') { cx = 0; lines++; p++; continue; }

        if (*p == ' ') {
            int sw = chaos_gl_font_char_width(handle, ' ');
            if (cx + sw > max_w && cx > 0) { cx = 0; lines++; }
            cx += sw;
            p++;
            continue;
        }

        /* Measure word */
        const char *word_start = p;
        int word_w = 0;
        int word_len = 0;
        while (*p && *p != ' ' && *p != '\n') {
            word_w += chaos_gl_font_char_width(handle, *p);
            word_len++;
            p++;
        }

        /* Wrap word if needed */
        if (cx > 0 && cx + word_w > max_w) { cx = 0; lines++; }

        /* Add chars, wrapping mid-word if necessary */
        for (int i = 0; i < word_len; i++) {
            int cw = chaos_gl_font_char_width(handle, word_start[i]);
            if (cx + cw > max_w && cx > 0) { cx = 0; lines++; }
            cx += cw;
        }
    }

    return lines * line_h;
}

/* ── Rendering ───────────────────────────────────────── */

int chaos_gl_font_char(int handle, int x, int y, char c, uint32_t fg) {
    ttf_font_t *font = get_font(handle);
    if (!font) return CLAUDE_MONO_WIDTH;

    chaos_gl_surface_t *s = chaos_gl_get_bound_surface();
    if (!s) return 0;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    uint8_t ch = (uint8_t)c;
    if (ch < CHAOS_GL_GLYPH_FIRST || ch > CHAOS_GL_GLYPH_LAST) {
        ch = ' ';
    }

    ttf_glyph_cache_t *g = &font->glyphs[ch - CHAOS_GL_GLYPH_FIRST];
    if (!g->bitmap || g->width == 0 || g->height == 0) {
        return g->advance;  /* Space or empty glyph */
    }

    /* Baseline position: y is top of line, add ascent + glyph yoff */
    int draw_x = x + g->xoff;
    int draw_y = y + font->ascent + g->yoff;

    uint32_t fg_rgb = fg & 0x00FFFFFF;

    for (int row = 0; row < g->height; row++) {
        for (int col = 0; col < g->width; col++) {
            uint8_t alpha = g->bitmap[row * g->width + col];
            if (alpha == 0) continue;
            uint32_t color = fg_rgb | ((uint32_t)alpha << 24);
            ttf_put_pixel_blend(s, draw_x + col, draw_y + row, color);
        }
    }

    return g->advance;
}

int chaos_gl_font_text(int handle, int x, int y, const char *str, uint32_t fg) {
    ttf_font_t *font = get_font(handle);
    if (!font) return 0;

    chaos_gl_surface_t *s = chaos_gl_get_bound_surface();
    if (!s) return 0;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    int cx = x;
    while (*str) {
        uint8_t ch = (uint8_t)*str;
        if (ch < CHAOS_GL_GLYPH_FIRST || ch > CHAOS_GL_GLYPH_LAST) {
            ch = ' ';
        }

        ttf_glyph_cache_t *g = &font->glyphs[ch - CHAOS_GL_GLYPH_FIRST];

        if (g->bitmap && g->width > 0 && g->height > 0) {
            int draw_x = cx + g->xoff;
            int draw_y = y + font->ascent + g->yoff;
            uint32_t fg_rgb = fg & 0x00FFFFFF;

            for (int row = 0; row < g->height; row++) {
                for (int col = 0; col < g->width; col++) {
                    uint8_t alpha = g->bitmap[row * g->width + col];
                    if (alpha == 0) continue;
                    uint32_t color = fg_rgb | ((uint32_t)alpha << 24);
                    ttf_put_pixel_blend(s, draw_x + col, draw_y + row, color);
                }
            }
        }

        cx += g->advance;
        str++;
    }

    return cx - x;
}

int chaos_gl_font_text_wrapped(int handle, int x, int y, int max_w,
                                const char *str, uint32_t fg) {
    ttf_font_t *font = get_font(handle);
    if (!font) return 0;

    chaos_gl_surface_t *s = chaos_gl_get_bound_surface();
    if (!s) return 0;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    int cx = x;
    int cy = y;
    int line_h = font->height_px;
    const char *p = str;

    while (*p) {
        if (*p == '\n') { cx = x; cy += line_h; p++; continue; }

        if (*p == ' ') {
            int sw = chaos_gl_font_char_width(handle, ' ');
            if (cx + sw > x + max_w && cx > x) { cx = x; cy += line_h; }
            cx += sw;
            p++;
            continue;
        }

        /* Measure word */
        const char *word_start = p;
        int word_w = 0;
        int word_len = 0;
        while (*p && *p != ' ' && *p != '\n') {
            word_w += chaos_gl_font_char_width(handle, *p);
            word_len++;
            p++;
        }

        /* Wrap word */
        if (cx > x && cx + word_w > x + max_w) { cx = x; cy += line_h; }

        /* Draw word char by char */
        for (int i = 0; i < word_len; i++) {
            int cw = chaos_gl_font_char_width(handle, word_start[i]);
            if (cx + cw > x + max_w && cx > x) { cx = x; cy += line_h; }
            chaos_gl_font_char(handle, cx, cy, word_start[i], fg);
            cx += cw;
        }
    }

    return (cy - y) + line_h;
}

/* ── KAOS exports ────────────────────────────────────── */

#include "../include/kaos/export.h"
KAOS_EXPORT(chaos_gl_font_load)
KAOS_EXPORT(chaos_gl_font_free)
KAOS_EXPORT(chaos_gl_font_text)
KAOS_EXPORT(chaos_gl_font_text_width)
KAOS_EXPORT(chaos_gl_font_height)
