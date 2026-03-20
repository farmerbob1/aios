/* AIOS v2 — PS/2 Keyboard Driver (Phase 3) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

/* Normal scancodes (0-127) */
#define SC_ESC       0x01
#define SC_W         0x11
#define SC_A         0x1E
#define SC_S         0x1F
#define SC_D         0x20
#define SC_Q         0x10
#define SC_E         0x12
#define SC_SPACE     0x39
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_LCTRL     0x1D
#define SC_LALT      0x38
#define SC_ENTER     0x1C
#define SC_BACKSPACE 0x0E

/* E0-prefixed scancodes (remapped to 128+) */
#define SC_RCTRL     (0x1D + 128)
#define SC_RALT      (0x38 + 128)
#define SC_UP        (0x48 + 128)
#define SC_DOWN      (0x50 + 128)
#define SC_LEFT      (0x4B + 128)
#define SC_RIGHT     (0x4D + 128)
#define SC_HOME      (0x47 + 128)
#define SC_END       (0x4F + 128)
#define SC_PGUP      (0x49 + 128)
#define SC_PGDN      (0x51 + 128)
#define SC_INSERT    (0x52 + 128)
#define SC_DELETE    (0x53 + 128)

/* Modifier flags for key events */
#define KEY_MOD_CTRL  0x1000
#define KEY_MOD_SHIFT 0x2000
#define KEY_MOD_ALT   0x4000

extern volatile bool key_state[256];

init_result_t keyboard_init(void);
void keyboard_handler(void);
bool keyboard_has_key(void);
char keyboard_getchar(void);
void keyboard_readline(char* buf, int max_len);
bool keyboard_is_pressed(uint8_t scancode);
