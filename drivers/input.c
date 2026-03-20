/* AIOS v2 — Shared Input Event Queue (Phase 3 STUB) */

#include "input.h"

static bool gui_mode = false;

void input_push(input_event_t* ev) { (void)ev; }
bool input_poll(input_event_t* ev) { (void)ev; return false; }
bool input_has_events(void) { return false; }
bool input_check_overflow(void) { return false; }
void input_set_gui_mode(bool enabled) { gui_mode = enabled; }
bool input_is_gui_mode(void) { return gui_mode; }
