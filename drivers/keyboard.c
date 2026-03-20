/* AIOS v2 — PS/2 Keyboard Driver (Phase 3) */

#include "keyboard.h"
#include "input.h"
#include "../include/io.h"
#include "../kernel/irq.h"
#include "../drivers/serial.h"

/* ── Key state ─────────────────────────────────────── */

volatile bool key_state[256] = {0};
static bool e0_prefix = false;

/* ── Text-mode ring buffer ─────────────────────────── */

#define KB_BUF_SIZE 64
static volatile char kb_buf[KB_BUF_SIZE];
static volatile int kb_head = 0;
static volatile int kb_tail = 0;

static void kb_buf_push(char c) {
    int next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {
        kb_buf[kb_head] = c;
        kb_head = next;
    }
}

/* ── Scancode-to-ASCII tables (US QWERTY, scan code set 1) ── */

static const char scancode_ascii[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ', 0,
    /* F1-F10, NumLock, ScrollLock, Home, Up, PgUp, -, Left, 5, Right, +, End, Down, PgDn, Ins, Del */
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char scancode_ascii_shift[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ', 0,
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* ── IRQ handler ───────────────────────────────────── */

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);

    /* Handle E0 prefix byte */
    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    /* E0-prefixed scancodes remapped to 128+ range */
    if (e0_prefix) {
        code += 128;
        e0_prefix = false;
    }

    /* Update key state */
    key_state[code] = !is_release;

    if (input_is_gui_mode()) {
        /* GUI mode: push event to input queue */
        uint16_t key_code = code;
        if (key_state[SC_LCTRL] || key_state[SC_RCTRL])   key_code |= KEY_MOD_CTRL;
        if (key_state[SC_LSHIFT] || key_state[SC_RSHIFT]) key_code |= KEY_MOD_SHIFT;
        if (key_state[SC_LALT] || key_state[SC_RALT])     key_code |= KEY_MOD_ALT;

        input_event_t ev;
        ev.type = is_release ? EVENT_KEY_UP : EVENT_KEY_DOWN;
        ev.key = key_code;
        ev.mouse_x = 0;
        ev.mouse_y = 0;
        ev.mouse_btn = 0;
        input_push(&ev);
    } else {
        /* Text mode: translate to ASCII and buffer (key down only) */
        if (!is_release && code < 128) {
            bool shift = key_state[SC_LSHIFT] || key_state[SC_RSHIFT];
            char c = shift ? scancode_ascii_shift[code] : scancode_ascii[code];
            if (c) kb_buf_push(c);
        }
    }
}

/* ── Public API ────────────────────────────────────── */

init_result_t keyboard_init(void) {
    /* Clear key state */
    for (int i = 0; i < 256; i++) key_state[i] = false;
    e0_prefix = false;
    kb_head = kb_tail = 0;

    /* Flush any pending data in the keyboard controller */
    while (inb(0x64) & 0x01) inb(0x60);

    irq_register_handler(1, keyboard_handler);
    irq_unmask(1);

    return INIT_OK;
}

bool keyboard_has_key(void) {
    return kb_head != kb_tail;
}

char keyboard_getchar(void) {
    while (kb_head == kb_tail) {
        __asm__ __volatile__("sti; hlt");
    }
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags));
    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags));
    return c;
}

void keyboard_readline(char* buf, int max_len) {
    int pos = 0;
    while (pos < max_len - 1) {
        char c = keyboard_getchar();
        if (c == '\n') {
            serial_putchar('\n');
            break;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                serial_print("\b \b");
            }
        } else {
            buf[pos++] = c;
            serial_putchar(c);
        }
    }
    buf[pos] = '\0';
}

bool keyboard_is_pressed(uint8_t scancode) {
    return key_state[scancode];
}
