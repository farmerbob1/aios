/* AIOS v2 — PS/2 Mouse Driver (Phase 3) */

#include "mouse.h"
#include "input.h"
#include "keyboard.h"
#include "../include/io.h"
#include "../kernel/irq.h"
#include "serial.h"

static int mouse_debug_count = 0;

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

/* Longer timeout for reset self-test (can take hundreds of ms) */
static void mouse_wait_read_slow(void) {
    int timeout = 10000000;
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

    if (mouse_debug_count < 3) {
        serial_printf("[mouse] packet: %02x %02x %02x gui=%d\n",
                      mouse_packet[0], mouse_packet[1], mouse_packet[2],
                      input_is_gui_mode());
        mouse_debug_count++;
    }

    /* Check overflow bits — discard if set */
    if (mouse_packet[0] & 0xC0) return;

    /* Extract deltas with sign extension */
    int dx = mouse_packet[1];
    int dy = mouse_packet[2];
    if (mouse_packet[0] & 0x10) dx |= 0xFFFFFF00;  /* X sign bit */
    if (mouse_packet[0] & 0x20) dy |= 0xFFFFFF00;  /* Y sign bit */

    /* Extract buttons and detect changes */
    uint8_t old_buttons = mouse_buttons_val;
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
        /* Capture keyboard modifier state for mouse events */
        uint16_t mods = 0;
        if (key_state[SC_LCTRL] || key_state[SC_RCTRL])   mods |= KEY_MOD_CTRL;
        if (key_state[SC_LSHIFT] || key_state[SC_RSHIFT]) mods |= KEY_MOD_SHIFT;
        if (key_state[SC_LALT] || key_state[SC_RALT])     mods |= KEY_MOD_ALT;

        input_event_t ev;
        ev.key = mods;
        ev.mouse_x = (int16_t)mouse_x_val;
        ev.mouse_y = (int16_t)mouse_y_val;
        ev.mouse_btn = mouse_buttons_val;

        /* Always push move event if mouse moved */
        if (dx != 0 || dy != 0) {
            ev.type = EVENT_MOUSE_MOVE;
            input_push(&ev);
        }

        /* Push button down/up events on state changes */
        for (int b = 0; b < 3; b++) {
            uint8_t mask = (uint8_t)(1 << b);
            if ((mouse_buttons_val & mask) && !(old_buttons & mask)) {
                ev.type = EVENT_MOUSE_DOWN;
                ev.mouse_btn = (uint8_t)(b + 1);
                input_push(&ev);
            } else if (!(mouse_buttons_val & mask) && (old_buttons & mask)) {
                ev.type = EVENT_MOUSE_UP;
                ev.mouse_btn = (uint8_t)(b + 1);
                input_push(&ev);
            }
        }
    }
}

/* ── Public API ────────────────────────────────────── */

init_result_t mouse_init(void) {
    /* Flush any pending data in the PS/2 controller */
    while (inb(0x64) & 0x01) inb(0x60);

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

    /* Reset mouse — required by some PS/2 controllers / QEMU */
    mouse_cmd(0xFF);
    mouse_wait_read_slow();
    (void)inb(0x60);  /* ACK (0xFA) */
    mouse_wait_read_slow();
    uint8_t selftest = inb(0x60);  /* Self-test result (0xAA) */
    mouse_wait_read_slow();
    uint8_t mouseid = inb(0x60);  /* Mouse ID (0x00) */
    serial_printf("[mouse] reset: selftest=0x%02x id=0x%02x\n", selftest, mouseid);

    /* Send defaults */
    mouse_cmd(0xF6);
    mouse_read_data();  /* ACK */

    /* Enable data reporting */
    mouse_cmd(0xF4);
    mouse_read_data();  /* ACK */

    /* Flush any data generated during init */
    while (inb(0x64) & 0x01) inb(0x60);
    mouse_byte = 0;
    serial_printf("[mouse] init complete\n");

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
