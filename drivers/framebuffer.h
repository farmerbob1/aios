/* AIOS v2 — VESA Framebuffer Driver (Phase 3) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

init_result_t fb_init(struct boot_info* info);
void fb_clear(uint32_t color);
void fb_putpixel(int x, int y, uint32_t color);
void fb_rect(int x, int y, int w, int h, uint32_t color);
void fb_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
void fb_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color);
void fb_line(int x1, int y1, int x2, int y2, uint32_t color);
void fb_circle(int cx, int cy, int radius, uint32_t color);
void fb_char(int x, int y, char c, uint32_t fg, uint32_t bg);
int  fb_text(int x, int y, const char* str, uint32_t fg, uint32_t bg);
int  fb_text_width(const char* str);
int  fb_text_wrapped(int x, int y, int max_w, const char* str, uint32_t fg, uint32_t bg);
void fb_swap(void);
void fb_set_clip(int x, int y, int w, int h);
void fb_clear_clip(void);
int  fb_width(void);
int  fb_height(void);
int  fb_pitch(void);
