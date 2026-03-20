/* AIOS v2 — VGA Text Mode Driver (80x25 at 0xB8000) */

#include "vga.h"
#include "../include/io.h"
#include "../include/string.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_BUFFER  ((volatile uint16_t*)0xB8000)

static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t current_color = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);

static void update_hardware_cursor(void) {
    uint16_t pos = cursor_y * VGA_WIDTH + cursor_x;
    outb(0x3D4, 14);
    outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

static void scroll(void) {
    if (cursor_y < VGA_HEIGHT) return;

    /* Move all lines up by one */
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        VGA_BUFFER[i] = VGA_BUFFER[i + VGA_WIDTH];
    }

    /* Clear the last line */
    uint16_t blank = (uint16_t)current_color << 8 | ' ';
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        VGA_BUFFER[i] = blank;
    }

    cursor_y = VGA_HEIGHT - 1;
}

init_result_t vga_init(void) {
    vga_clear();
    return INIT_OK;
}

void vga_putchar(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            VGA_BUFFER[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)current_color << 8 | ' ';
        }
    } else {
        VGA_BUFFER[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)current_color << 8 | (uint8_t)c;
        cursor_x++;
    }

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }

    scroll();
    update_hardware_cursor();
}

void vga_print(const char* str) {
    while (*str)
        vga_putchar(*str++);
}

void vga_print_color(const char* str, uint8_t color) {
    uint8_t old = current_color;
    current_color = color;
    vga_print(str);
    current_color = old;
}

void vga_clear(void) {
    uint16_t blank = (uint16_t)current_color << 8 | ' ';
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }
    cursor_x = 0;
    cursor_y = 0;
    update_hardware_cursor();
}

void vga_set_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
    update_hardware_cursor();
}

int vga_get_cursor_x(void) { return cursor_x; }
int vga_get_cursor_y(void) { return cursor_y; }
