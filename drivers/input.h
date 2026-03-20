/* AIOS v2 — Shared Input Event Queue (Phase 3) */

#pragma once

#include "../include/types.h"

#define INPUT_QUEUE_SIZE 256

typedef enum {
    EVENT_NONE = 0,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
} event_type_t;

typedef struct {
    uint8_t  type;
    uint16_t key;
    int16_t  mouse_x;
    int16_t  mouse_y;
    uint8_t  mouse_btn;
} input_event_t;

void input_push(input_event_t* ev);
bool input_poll(input_event_t* ev);
bool input_has_events(void);
bool input_check_overflow(void);
void input_set_gui_mode(bool enabled);
bool input_is_gui_mode(void);
