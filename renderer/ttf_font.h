/* ChaosGL TrueType Font Subsystem
 * Loads TTF fonts via stb_truetype, caches rasterized glyphs,
 * renders anti-aliased text with alpha blending. */

#pragma once

#include "../include/types.h"

#define CHAOS_GL_MAX_FONTS    16
#define CHAOS_GL_GLYPH_FIRST  32
#define CHAOS_GL_GLYPH_LAST   126
#define CHAOS_GL_GLYPH_COUNT  (CHAOS_GL_GLYPH_LAST - CHAOS_GL_GLYPH_FIRST + 1)

/* Lifecycle */
void   chaos_gl_font_init(void);
int    chaos_gl_font_load(const char *path, float size_px);
void   chaos_gl_font_free(int handle);

/* System font — when set, chaos_gl_text() routes through TTF */
void   chaos_gl_set_font(int handle);   /* -1 to revert to bitmap */
int    chaos_gl_get_font(void);

/* Metrics — pass -1 for system font, or explicit handle */
int    chaos_gl_font_height(int handle);
int    chaos_gl_font_ascent(int handle);
int    chaos_gl_font_descent(int handle);
int    chaos_gl_font_text_width(int handle, const char *str);
int    chaos_gl_font_char_width(int handle, char c);
int    chaos_gl_font_text_height_wrapped(int handle, int max_w, const char *str);

/* Explicit font rendering */
int    chaos_gl_font_text(int handle, int x, int y, const char *str, uint32_t fg);
int    chaos_gl_font_text_wrapped(int handle, int x, int y, int max_w, const char *str, uint32_t fg);
int    chaos_gl_font_char(int handle, int x, int y, char c, uint32_t fg);
