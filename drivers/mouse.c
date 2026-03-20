/* AIOS v2 — PS/2 Mouse Driver (Phase 3) */

#include "mouse.h"
#include "input.h"
#include "../include/io.h"
#include "../kernel/irq.h"

/* ── State ─────────────────────────────────────────── */

static int mouse_x_val = 0, mouse_y_val = 0;
static int mouse_max_x = 1024, mouse_max_y = 768;
static uint8_t mouse_buttons_val = 0;
static volatile bool raw_mode = false;
static volatile int raw_dx = 0, raw_dy = 0;

/* 3-byte packet assembly */
static uint8_t mouse_packet[3];
static int mouse_byte = 0;

/* ── PS/2 controller helpers ───────────────────────── */

static void mouse_wait_write(void) {
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout > 0);
}

static void mouse_wait_read(void) {
    int timeout = 100000;
    while (!(inb(0x64) & 0x01) && --timeout > 0);
}

static void mouse_cmd(uint8_t cmd) {
    mouse_wait_write();
    outb(0x64, 0xD4);   /* next byte goes to mouse */
    mouse_wait_write();
    outb(0x60, cmd);
}

static uint8_t mouse_read_data(void) {
    mouse_wait_read();
    return inb(0x60);
}

/* ── IRQ12 handler ─────────────────────────────────── */

void mouse_handler(void) {
    uint8_t data = inb(0x60);

    /* Sync recovery: byte 0 must have bit 3 set */
    if (mouse_byte == 0 && !(data & 0x08)) {
        return;  /* out of sync, discard */
    }

    mouse_packet[mouse_byte++] = data;
    if (mouse_byte < 3) return;
    mouse_byte = 0;

    /* Check overflow bits — discard if set */
    if (mouse_packet[0] & 0xC0) return;

    /* Extract deltas with sign extension */
    int dx = mouse_packet[1];
    int dy = mouse_packet[2];
    if (mouse_packet[0] & 0x10) dx |= 0xFFFFFF00;  /* X sign bit */
    if (mouse_packet[0] & 0x20) dy |= 0xFFFFFF00;  /* Y sign bit */

    /* Extract buttons */
    mouse_buttons_val = mouse_packet[0] & 0x07;

    if (raw_mode) {
        raw_dx += dx;
        raw_dy += dy;
    } else {
        /* Desktop mode: PS/2 Y is inverted vs screen coords */
        mouse_x_val += dx;
        mouse_y_val -= dy;

        /* Clamp to bounds */
        if (mouse_x_val < 0) mouse_x_val = 0;
        if (mouse_y_val < 0) mouse_y_val = 0;
        if (mouse_x_val >= mouse_max_x) mouse_x_val = mouse_max_x - 1;
        if (mouse_y_val >= mouse_max_y) mouse_y_val = mouse_max_y - 1;
    }

    /* Push input events if in GUI mode */
    if (input_is_gui_mode()) {
        input_event_t ev;
        ev.type = EVENT_MOUSE_MOVE;
        ev.key = 0;
        ev.mouse_x = (int16_t)mouse_x_val;
        ev.mouse_y = (int16_t)mouse_y_val;
        ev.mouse_btn = mouse_buttons_val;
        input_push(&ev);
    }
}

/* ── Public API ────────────────────────────────────── */

init_result_t mouse_init(void) {
    /* Enable auxiliary device */
    mouse_wait_write();
    outb(0x64, 0xA8);

    /* Enable IRQ12 in the PS/2 controller's compaq status byte */
    mouse_wait_write();
    outb(0x64, 0x20);  /* read compaq status */
    mouse_wait_read();
    uint8_t status = inb(0x60);
    status |= 0x02;    /* enable IRQ12 */
    status &= ~0x20;   /* enable mouse clock */
    mouse_wait_write();
    outb(0x64, 0x60);  /* write compaq status */
    mouse_wait_write();
    outb(0x60, status);

    /* Send defaults */
    mouse_cmd(0xF6);
    mouse_read_data();  /* ACK */

    /* Enable data reporting */
    mouse_cmd(0xF4);
    mouse_read_data();  /* ACK */

    /* Register IRQ handler */
    irq_register_handler(12, mouse_handler);
    irq_unmask(12);

    return INIT_OK;
}

int mouse_get_x(void) { return mouse_x_val; }
int mouse_get_y(void) { return mouse_y_val; }
uint8_t mouse_get_buttons(void) { return mouse_buttons_val; }

void mouse_set_bounds(int width, int height) {
    mouse_max_x = width;
    mouse_max_y = height;
    if (mouse_x_val >= mouse_max_x) mouse_x_val = mouse_max_x - 1;
    if (mouse_y_val >= mouse_max_y) mouse_y_val = mouse_max_y - 1;
}

void mouse_set_raw_mode(bool enable) {
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags));
    raw_mode = enable;
    raw_dx = raw_dy = 0;
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags));
}

void mouse_get_delta(int* dx, int* dy) {
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags));
    if (dx) *dx = raw_dx;
    if (dy) *dy = raw_dy;
    raw_dx = raw_dy = 0;
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags));
}
