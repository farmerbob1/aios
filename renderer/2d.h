/* ChaosGL 2D Primitives — rect, circle, line, text, blit (Phase 5) */

#pragma once

#include "surface.h"

/* Clip stack */
void chaos_gl_push_clip(rect_t r);
void chaos_gl_pop_clip(void);
void chaos_gl_reset_clip(void);

/* Primitives */
void chaos_gl_rect(int x, int y, int w, int h, uint32_t color);
void chaos_gl_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
void chaos_gl_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color);
void chaos_gl_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
void chaos_gl_circle(int cx, int cy, int radius, uint32_t color);
void chaos_gl_circle_outline(int cx, int cy, int radius, uint32_t color, int thickness);
void chaos_gl_line(int x0, int y0, int x1, int y1, uint32_t color);
void chaos_gl_pixel(int x, int y, uint32_t color);
void chaos_gl_pixel_blend(int x, int y, uint32_t color_bgra);

/* Image blit */
void chaos_gl_blit(int x, int y, int w, int h, const uint32_t* src, int src_pitch);
void chaos_gl_blit_keyed(int x, int y, int w, int h, const uint32_t* src, int src_pitch, uint32_t key_color);
void chaos_gl_blit_alpha(int x, int y, int w, int h, const uint32_t* src, int src_pitch);

/* Text -- flags: CHAOS_GL_TEXT_BG_TRANSPARENT (0) or CHAOS_GL_TEXT_BG_FILL (1) */
int chaos_gl_char(int x, int y, char c, uint32_t fg, uint32_t bg, uint32_t flags);
int chaos_gl_text(int x, int y, const char* str, uint32_t fg, uint32_t bg, uint32_t flags);
int chaos_gl_text_wrapped(int x, int y, int max_w, const char* str, uint32_t fg, uint32_t bg, uint32_t flags);
int chaos_gl_text_width(const char* str);
int chaos_gl_text_height_wrapped(int max_w, const char* str);
