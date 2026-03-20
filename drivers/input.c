/* AIOS v2 — Shared Input Event Queue (Phase 3) */

#include "input.h"

static input_event_t queue[INPUT_QUEUE_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile bool overflow = false;
static bool gui_mode = false;

void input_push(input_event_t* ev) {
    /* Called from IRQ context — interrupts already disabled */
    uint32_t next = (head + 1) % INPUT_QUEUE_SIZE;
    if (next == tail) {
        overflow = true;
        return;  /* drop newest event */
    }
    queue[head] = *ev;
    head = next;
}

bool input_poll(input_event_t* ev) {
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags));
    if (tail == head) {
        __asm__ __volatile__("pushl %0; popfl" : : "r"(flags));
        return false;
    }
    *ev = queue[tail];
    tail = (tail + 1) % INPUT_QUEUE_SIZE;
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags));
    return true;
}

bool input_has_events(void) {
    return head != tail;
}

bool input_check_overflow(void) {
    bool was = overflow;
    overflow = false;
    return was;
}

void input_set_gui_mode(bool enabled) {
    gui_mode = enabled;
}

bool input_is_gui_mode(void) {
    return gui_mode;
}
