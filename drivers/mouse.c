/* AIOS v2 — PS/2 Mouse Driver (Phase 3 STUB) */

#include "mouse.h"

init_result_t mouse_init(void) { return INIT_OK; }
void mouse_handler(void) {}
int mouse_get_x(void) { return 0; }
int mouse_get_y(void) { return 0; }
uint8_t mouse_get_buttons(void) { return 0; }
void mouse_set_bounds(int width, int height) { (void)width; (void)height; }
void mouse_set_raw_mode(bool enable) { (void)enable; }
void mouse_get_delta(int* dx, int* dy) { if (dx) *dx = 0; if (dy) *dy = 0; }
