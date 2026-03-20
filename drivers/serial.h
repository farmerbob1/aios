/* AIOS v2 — Serial Debug Output (COM1)
 * Available from earliest kernel entry, before VGA or framebuffer. */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

#define SERIAL_COM1 0x3F8

init_result_t serial_init(void);
void serial_putchar(char c);
void serial_print(const char* str);
void serial_printf(const char* fmt, ...);
