/* AIOS v2 — PS/2 Keyboard Driver (Phase 3 STUB) */

#include "keyboard.h"

volatile bool key_state[256] = {0};

init_result_t keyboard_init(void) { return INIT_OK; }
void keyboard_handler(void) {}
bool keyboard_has_key(void) { return false; }
char keyboard_getchar(void) { return 0; }
void keyboard_readline(char* buf, int max_len) { (void)buf; (void)max_len; }
bool keyboard_is_pressed(uint8_t scancode) { (void)scancode; return false; }
