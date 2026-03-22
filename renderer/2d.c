/* ChaosGL 2D Primitives — implementation (Phase 5) */

#include "2d.h"
#include "font.h"
#include "ttf_font.h"
#include "math.h"
#include "../include/string.h"

/* ── Internal helpers ─────────────────────────────────── */

static inline void put_pixel(chaos_gl_surface_t* s, int x, int y, uint32_t color) {
    rect_t clip = s->clip_stack[s->clip_depth - 1];
    if (x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h) return;
    s->bufs[1 - s->buf_index][y * s->width + x] = color;
}

static inline void put_pixel_blend(chaos_gl_surface_t* s, int x, int y, uint32_t color) {
    rect_t clip = s->clip_stack[s->clip_depth - 1];
    if (x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h) return;

    uint32_t a = (color >> 24) & 0xFF;
    if (a == 0) return;

    uint32_t* dst = &s->bufs[1 - s->buf_index][y * s->width + x];
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

static inline int int_max(int a, int b) { return a > b ? a : b; }
static inline int int_min(int a, int b) { return a < b ? a : b; }
static inline int int_abs(int a) { return a < 0 ? -a : a; }

/* ── Clip stack ───────────────────────────────────────── */

void chaos_gl_push_clip(rect_t r) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    if (s->clip_depth >= CHAOS_GL_CLIP_STACK_DEPTH) return;

    rect_t current = s->clip_stack[s->clip_depth - 1];
    rect_t clipped = rect_intersect(current, r);
    s->clip_stack[s->clip_depth] = clipped;
    s->clip_depth++;
}

void chaos_gl_pop_clip(void) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    if (s->clip_depth > 1)
        s->clip_depth--;
}

void chaos_gl_reset_clip(void) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->clip_stack[0] = (rect_t){ 0, 0, s->width, s->height };
    s->clip_depth = 1;
}

/* ── Filled rectangle ─────────────────────────────────── */

void chaos_gl_rect(int x, int y, int w, int h, uint32_t color) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    rect_t clip = s->clip_stack[s->clip_depth - 1];

    /* Intersect fill rect with clip rect */
    int x0 = int_max(x, clip.x);
    int y0 = int_max(y, clip.y);
    int x1 = int_min(x + w, clip.x + clip.w);
    int y1 = int_min(y + h, clip.y + clip.h);

    if (x0 >= x1 || y0 >= y1) return;

    uint32_t* buf = s->bufs[1 - s->buf_index];
    int stride = s->width;

    for (int row = y0; row < y1; row++) {
        uint32_t* p = &buf[row * stride + x0];
        int count = x1 - x0;
        for (int i = 0; i < count; i++)
            p[i] = color;
    }
}

/* ── Outline rectangle ────────────────────────────────── */

void chaos_gl_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    int t = thickness;
    /* Top edge */
    chaos_gl_rect(x, y, w, t, color);
    /* Bottom edge */
    chaos_gl_rect(x, y + h - t, w, t, color);
    /* Left edge */
    chaos_gl_rect(x, y + t, t, h - 2 * t, color);
    /* Right edge */
    chaos_gl_rect(x + w - t, y + t, t, h - 2 * t, color);
}

/* ── Rounded rectangle (filled) ───────────────────────── */

void chaos_gl_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    if (radius <= 0) {
        chaos_gl_rect(x, y, w, h, color);
        return;
    }

    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    float rf = (float)r;

    for (int row = y; row < y + h; row++) {
        int dy;
        int x_inset = 0;

        if (row < y + r) {
            /* Top rounded region */
            dy = (y + r) - row - 1;
            float dx = rf - chaos_sqrtf(rf * rf - (float)(dy * dy));
            x_inset = (int)(dx + 0.5f);
        } else if (row >= y + h - r) {
            /* Bottom rounded region */
            dy = row - (y + h - r - 1) - 1;
            float dx = rf - chaos_sqrtf(rf * rf - (float)(dy * dy));
            x_inset = (int)(dx + 0.5f);
        }

        int lx = x + x_inset;
        int rx = x + w - x_inset;
        if (lx < rx) {
            /* Use put_pixel scanline for clipping */
            rect_t clip = s->clip_stack[s->clip_depth - 1];
            int cx0 = int_max(lx, clip.x);
            int cx1 = int_min(rx, clip.x + clip.w);
            if (row >= clip.y && row < clip.y + clip.h && cx0 < cx1) {
                uint32_t* buf = s->bufs[1 - s->buf_index];
                uint32_t* p = &buf[row * s->width + cx0];
                for (int i = 0; i < cx1 - cx0; i++)
                    p[i] = color;
            }
        }
    }
}

/* ── Rounded rectangle outline ────────────────────────── */

void chaos_gl_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    if (radius <= 0) {
        chaos_gl_rect_outline(x, y, w, h, color, thickness);
        return;
    }

    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Draw using midpoint circle for corners and straight lines for edges */
    /* Top edge (between corners) */
    chaos_gl_rect(x + r, y, w - 2 * r, thickness, color);
    /* Bottom edge */
    chaos_gl_rect(x + r, y + h - thickness, w - 2 * r, thickness, color);
    /* Left edge */
    chaos_gl_rect(x, y + r, thickness, h - 2 * r, color);
    /* Right edge */
    chaos_gl_rect(x + w - thickness, y + r, thickness, h - 2 * r, color);

    /* Draw corner arcs using midpoint circle */
    int cx_tl = x + r, cy_tl = y + r;           /* top-left center */
    int cx_tr = x + w - r - 1, cy_tr = y + r;   /* top-right center */
    int cx_bl = x + r, cy_bl = y + h - r - 1;   /* bottom-left center */
    int cx_br = x + w - r - 1, cy_br = y + h - r - 1; /* bottom-right center */

    for (int t = 0; t < thickness; t++) {
        int cr = r - t;
        if (cr <= 0) break;

        int px = 0, py = cr;
        int d = 1 - cr;

        while (px <= py) {
            /* Top-left corner (quadrant: -x, -y) */
            put_pixel(s, cx_tl - px, cy_tl - py, color);
            put_pixel(s, cx_tl - py, cy_tl - px, color);
            /* Top-right corner (quadrant: +x, -y) */
            put_pixel(s, cx_tr + px, cy_tr - py, color);
            put_pixel(s, cx_tr + py, cy_tr - px, color);
            /* Bottom-left corner (quadrant: -x, +y) */
            put_pixel(s, cx_bl - px, cy_bl + py, color);
            put_pixel(s, cx_bl - py, cy_bl + px, color);
            /* Bottom-right corner (quadrant: +x, +y) */
            put_pixel(s, cx_br + px, cy_br + py, color);
            put_pixel(s, cx_br + py, cy_br + px, color);

            if (d < 0) {
                d += 2 * px + 3;
            } else {
                d += 2 * (px - py) + 5;
                py--;
            }
            px++;
        }
    }
}

/* ── Filled circle ────────────────────────────────────── */

void chaos_gl_circle(int cx, int cy, int radius, uint32_t color) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    if (radius <= 0) {
        put_pixel(s, cx, cy, color);
        return;
    }

    int px = 0, py = radius;
    int d = 1 - radius;

    /* Fill horizontal spans using midpoint algorithm */
    while (px <= py) {
        /* Scanlines at cy +/- py */
        for (int i = cx - px; i <= cx + px; i++) {
            put_pixel(s, i, cy + py, color);
            put_pixel(s, i, cy - py, color);
        }
        /* Scanlines at cy +/- px */
        for (int i = cx - py; i <= cx + py; i++) {
            put_pixel(s, i, cy + px, color);
            put_pixel(s, i, cy - px, color);
        }

        if (d < 0) {
            d += 2 * px + 3;
        } else {
            d += 2 * (px - py) + 5;
            py--;
        }
        px++;
    }
}

/* ── Circle outline ───────────────────────────────────── */

void chaos_gl_circle_outline(int cx, int cy, int radius, uint32_t color, int thickness) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    if (thickness <= 1) {
        /* Thin circle: standard midpoint algorithm, 8-way symmetry */
        int px = 0, py = radius;
        int d = 1 - radius;

        while (px <= py) {
            put_pixel(s, cx + px, cy + py, color);
            put_pixel(s, cx - px, cy + py, color);
            put_pixel(s, cx + px, cy - py, color);
            put_pixel(s, cx - px, cy - py, color);
            put_pixel(s, cx + py, cy + px, color);
            put_pixel(s, cx - py, cy + px, color);
            put_pixel(s, cx + py, cy - px, color);
            put_pixel(s, cx - py, cy - px, color);

            if (d < 0) {
                d += 2 * px + 3;
            } else {
                d += 2 * (px - py) + 5;
                py--;
            }
            px++;
        }
    } else {
        /* Thick circle: draw filled outer minus filled inner */
        /* Outer circle */
        chaos_gl_circle(cx, cy, radius, color);

        /* Clear inner circle — need to read and restore, so just draw per-scanline */
        /* Instead, use the annulus approach: for each scanline, fill only the ring */
        /* Re-clear and redraw properly */
        int r_outer = radius;
        int r_inner = radius - thickness;
        if (r_inner < 0) r_inner = 0;

        /* Clear the surface area first, then draw ring scanline by scanline */
        for (int row = cy - r_outer; row <= cy + r_outer; row++) {
            int dy = row - cy;
            /* Outer extent */
            int r2o = r_outer * r_outer;
            int r2i = r_inner * r_inner;
            int dy2 = dy * dy;

            if (dy2 > r2o) continue;

            int xo = (int)chaos_sqrtf((float)(r2o - dy2));
            int xi = 0;
            if (dy2 <= r2i)
                xi = (int)chaos_sqrtf((float)(r2i - dy2));

            /* Left arc */
            for (int col = cx - xo; col <= cx - xi; col++)
                put_pixel(s, col, row, color);
            /* Right arc */
            for (int col = cx + xi; col <= cx + xo; col++)
                put_pixel(s, col, row, color);
        }
    }
}

/* ── Line (Bresenham) ─────────────────────────────────── */

void chaos_gl_line(int x0, int y0, int x1, int y1, uint32_t color) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    int dx = int_abs(x1 - x0);
    int dy = -int_abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        put_pixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ── Single pixel ─────────────────────────────────────── */

void chaos_gl_pixel(int x, int y, uint32_t color) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;
    put_pixel(s, x, y, color);
}

/* ── Alpha-blended pixel ──────────────────────────────── */

void chaos_gl_pixel_blend(int x, int y, uint32_t color_bgra) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;
    put_pixel_blend(s, x, y, color_bgra);
}

/* ── Opaque blit ──────────────────────────────────────── */

void chaos_gl_blit(int x, int y, int w, int h, const uint32_t* src, int src_pitch) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    rect_t clip = s->clip_stack[s->clip_depth - 1];

    int x0 = int_max(x, clip.x);
    int y0 = int_max(y, clip.y);
    int x1 = int_min(x + w, clip.x + clip.w);
    int y1 = int_min(y + h, clip.y + clip.h);

    if (x0 >= x1 || y0 >= y1) return;

    uint32_t* buf = s->bufs[1 - s->buf_index];

    for (int row = y0; row < y1; row++) {
        const uint32_t* srow = &src[(row - y) * src_pitch + (x0 - x)];
        uint32_t* drow = &buf[row * s->width + x0];
        memcpy(drow, srow, (uint32_t)(x1 - x0) * 4);
    }
}

/* ── Color-keyed blit ─────────────────────────────────── */

void chaos_gl_blit_keyed(int x, int y, int w, int h, const uint32_t* src, int src_pitch, uint32_t key_color) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    rect_t clip = s->clip_stack[s->clip_depth - 1];

    int x0 = int_max(x, clip.x);
    int y0 = int_max(y, clip.y);
    int x1 = int_min(x + w, clip.x + clip.w);
    int y1 = int_min(y + h, clip.y + clip.h);

    if (x0 >= x1 || y0 >= y1) return;

    uint32_t* buf = s->bufs[1 - s->buf_index];

    for (int row = y0; row < y1; row++) {
        const uint32_t* srow = &src[(row - y) * src_pitch + (x0 - x)];
        uint32_t* drow = &buf[row * s->width + x0];
        for (int col = 0; col < x1 - x0; col++) {
            if (srow[col] != key_color)
                drow[col] = srow[col];
        }
    }
}

/* ── Alpha blit ───────────────────────────────────────── */

void chaos_gl_blit_alpha(int x, int y, int w, int h, const uint32_t* src, int src_pitch) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    rect_t clip = s->clip_stack[s->clip_depth - 1];

    int x0 = int_max(x, clip.x);
    int y0 = int_max(y, clip.y);
    int x1 = int_min(x + w, clip.x + clip.w);
    int y1 = int_min(y + h, clip.y + clip.h);

    if (x0 >= x1 || y0 >= y1) return;

    uint32_t* buf = s->bufs[1 - s->buf_index];

    for (int row = y0; row < y1; row++) {
        const uint32_t* srow = &src[(row - y) * src_pitch + (x0 - x)];
        uint32_t* drow = &buf[row * s->width + x0];
        for (int col = 0; col < x1 - x0; col++) {
            uint32_t sc = srow[col];
            uint32_t a = (sc >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 255) {
                drow[col] = sc;
                continue;
            }
            uint32_t inv_a = 255 - a;
            uint32_t dc = drow[col];
            uint32_t rb_s = sc & 0x00FF00FF;
            uint32_t g_s  = sc & 0x0000FF00;
            uint32_t rb_d = dc & 0x00FF00FF;
            uint32_t g_d  = dc & 0x0000FF00;
            uint32_t rb = ((rb_s * a + rb_d * inv_a + 0x00800080) >> 8) & 0x00FF00FF;
            uint32_t g  = ((g_s  * a + g_d  * inv_a + 0x00000080) >> 8) & 0x0000FF00;
            drow[col] = rb | g;
        }
    }
}

/* ── Single character ─────────────────────────────────── */

int chaos_gl_char(int x, int y, char c, uint32_t fg, uint32_t bg, uint32_t flags) {
    int sys_font = chaos_gl_get_font();
    if (sys_font >= 0) return chaos_gl_font_char(sys_font, x, y, c, fg);

    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return CLAUDE_MONO_WIDTH;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    uint8_t ch = (uint8_t)c;
    if (ch >= 128) ch = 0; /* Out-of-range: blank glyph */

    const uint8_t* glyph = claude_mono_8x16[ch];
    bool fill_bg = (flags & CHAOS_GL_TEXT_BG_FILL) != 0;

    for (int row = 0; row < CLAUDE_MONO_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < CLAUDE_MONO_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel(s, x + col, y + row, fg);
            } else if (fill_bg) {
                put_pixel(s, x + col, y + row, bg);
            }
        }
    }

    return CLAUDE_MONO_WIDTH;
}

/* ── Text string ──────────────────────────────────────── */

int chaos_gl_text(int x, int y, const char* str, uint32_t fg, uint32_t bg, uint32_t flags) {
    int sys_font = chaos_gl_get_font();
    if (sys_font >= 0) return chaos_gl_font_text(sys_font, x, y, str, fg);

    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return 0;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    int cx = x;
    while (*str) {
        /* Call char directly (it also increments draw_calls, but that's acceptable) */
        chaos_gl_char(cx, y, *str, fg, bg, flags);
        cx += CLAUDE_MONO_WIDTH;
        str++;
    }
    return cx - x;
}

/* ── Word-wrapped text ────────────────────────────────── */

int chaos_gl_text_wrapped(int x, int y, int max_w, const char* str, uint32_t fg, uint32_t bg, uint32_t flags) {
    int sys_font = chaos_gl_get_font();
    if (sys_font >= 0) return chaos_gl_font_text_wrapped(sys_font, x, y, max_w, str, fg);

    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return 0;
    s->stats.draw_calls_2d++;
    s->dirty = true;

    int cx = x;
    int cy = y;
    int cw = CLAUDE_MONO_WIDTH;
    int ch = CLAUDE_MONO_HEIGHT;

    const char* p = str;
    while (*p) {
        /* Find next word boundary */
        const char* word_start = p;
        int word_len = 0;

        if (*p == ' ') {
            /* Space character */
            if (cx + cw > x + max_w && cx > x) {
                cx = x;
                cy += ch;
            }
            chaos_gl_char(cx, cy, ' ', fg, bg, flags);
            cx += cw;
            p++;
            continue;
        }

        if (*p == '\n') {
            cx = x;
            cy += ch;
            p++;
            continue;
        }

        /* Measure word */
        while (*p && *p != ' ' && *p != '\n') {
            word_len++;
            p++;
        }

        int word_w = word_len * cw;

        /* Wrap if word doesn't fit and we're not at line start */
        if (cx > x && cx + word_w > x + max_w) {
            cx = x;
            cy += ch;
        }

        /* Draw word character by character */
        for (int i = 0; i < word_len; i++) {
            /* If single char exceeds line, wrap mid-word */
            if (cx + cw > x + max_w && cx > x) {
                cx = x;
                cy += ch;
            }
            chaos_gl_char(cx, cy, word_start[i], fg, bg, flags);
            cx += cw;
        }
    }

    return (cy - y) + ch;
}

/* ── Text width (unwrapped) ───────────────────────────── */

int chaos_gl_text_width(const char* str) {
    int sys_font = chaos_gl_get_font();
    if (sys_font >= 0) return chaos_gl_font_text_width(sys_font, str);
    return (int)strlen(str) * CLAUDE_MONO_WIDTH;
}

/* ── Text height when word-wrapped ────────────────────── */

int chaos_gl_text_height_wrapped(int max_w, const char* str) {
    int sys_font = chaos_gl_get_font();
    if (sys_font >= 0) return chaos_gl_font_text_height_wrapped(sys_font, max_w, str);

    int cx = 0;
    int lines = 1;
    int cw = CLAUDE_MONO_WIDTH;
    int ch = CLAUDE_MONO_HEIGHT;

    const char* p = str;
    while (*p) {
        if (*p == '\n') {
            cx = 0;
            lines++;
            p++;
            continue;
        }

        if (*p == ' ') {
            if (cx + cw > max_w && cx > 0) {
                cx = 0;
                lines++;
            }
            cx += cw;
            p++;
            continue;
        }

        /* Measure word */
        int word_len = 0;
        while (*p && *p != ' ' && *p != '\n') {
            word_len++;
            p++;
        }

        int word_w = word_len * cw;

        if (cx > 0 && cx + word_w > max_w) {
            cx = 0;
            lines++;
        }

        for (int i = 0; i < word_len; i++) {
            if (cx + cw > max_w && cx > 0) {
                cx = 0;
                lines++;
            }
            cx += cw;
        }
    }

    return lines * ch;
}
