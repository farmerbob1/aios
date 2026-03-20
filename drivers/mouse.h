/* AIOS v2 — PS/2 Mouse Driver (Phase 3) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

init_result_t mouse_init(void);
void    mouse_handler(void);
int     mouse_get_x(void);
int     mouse_get_y(void);
uint8_t mouse_get_buttons(void);
void    mouse_set_bounds(int width, int height);
void    mouse_set_raw_mode(bool enable);
void    mouse_get_delta(int* dx, int* dy);
