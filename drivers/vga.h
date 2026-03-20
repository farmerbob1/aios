/* AIOS v2 — VGA Text Mode Driver
 * Writes to 0xB8000, 80x25, used during boot and as fallback display. */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

/* VGA colors */
#define VGA_BLACK        0x0
#define VGA_BLUE         0x1
#define VGA_GREEN        0x2
#define VGA_CYAN         0x3
#define VGA_RED          0x4
#define VGA_MAGENTA      0x5
#define VGA_BROWN        0x6
#define VGA_LIGHT_GREY   0x7
#define VGA_DARK_GREY    0x8
#define VGA_LIGHT_BLUE   0x9
#define VGA_LIGHT_GREEN  0xA
#define VGA_LIGHT_CYAN   0xB
#define VGA_LIGHT_RED    0xC
#define VGA_LIGHT_MAGENTA 0xD
#define VGA_YELLOW       0xE
#define VGA_WHITE        0xF

#define VGA_COLOR(fg, bg) ((bg) << 4 | (fg))

init_result_t vga_init(void);
void vga_putchar(char c);
void vga_print(const char* str);
void vga_print_color(const char* str, uint8_t color);
void vga_clear(void);
void vga_set_cursor(int x, int y);
int  vga_get_cursor_x(void);
int  vga_get_cursor_y(void);
