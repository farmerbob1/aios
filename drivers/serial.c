/* AIOS v2 — Serial Debug Output (COM1, 115200 baud, 8N1)
 * Minimal printf supporting: %d, %u, %x, %s, %p, %c, %% */

#include "serial.h"
#include "../include/io.h"
#include "../include/kaos/export.h"

#define PORT SERIAL_COM1

init_result_t serial_init(void) {
    outb(PORT + 1, 0x00);   /* Disable all interrupts */
    outb(PORT + 3, 0x80);   /* Enable DLAB (set baud rate divisor) */
    outb(PORT + 0, 0x01);   /* Divisor lo: 1 = 115200 baud */
    outb(PORT + 1, 0x00);   /* Divisor hi */
    outb(PORT + 3, 0x03);   /* 8 bits, no parity, 1 stop bit */
    outb(PORT + 2, 0xC7);   /* Enable FIFO, clear, 14-byte threshold */
    outb(PORT + 4, 0x0B);   /* IRQs enabled, RTS/DSR set */

    /* Loopback test */
    outb(PORT + 4, 0x1E);   /* Set in loopback mode */
    outb(PORT + 0, 0xAE);   /* Send test byte */
    if (inb(PORT + 0) != 0xAE) {
        return INIT_FAIL;
    }

    /* Disable loopback, set normal operation */
    outb(PORT + 4, 0x0F);
    return INIT_OK;
}

static int serial_is_transmit_empty(void) {
    return inb(PORT + 5) & 0x20;
}

void serial_putchar(char c) {
    while (!serial_is_transmit_empty())
        ;
    outb(PORT, (uint8_t)c);
}

void serial_print(const char* str) {
    while (*str) {
        if (*str == '\n')
            serial_putchar('\r');
        serial_putchar(*str++);
    }
}

/* --- Minimal printf implementation --- */

static void print_uint(uint32_t val, int base, int min_width, char pad) {
    char buf[12]; /* max 10 digits for uint32 + sign + null */
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            uint32_t digit = val % base;
            buf[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
            val /= base;
        }
    }

    /* Pad */
    while (i < min_width) {
        serial_putchar(pad);
        min_width--;
    }

    /* Print reversed */
    while (i > 0)
        serial_putchar(buf[--i]);
}

static void print_int(int32_t val) {
    if (val < 0) {
        serial_putchar('-');
        print_uint((uint32_t)(-(int64_t)val), 10, 0, ' ');
    } else {
        print_uint((uint32_t)val, 10, 0, ' ');
    }
}

void serial_printf(const char* fmt, ...) {
    /* Use GCC builtin va_args (available in freestanding mode) */
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                serial_putchar('\r');
            serial_putchar(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */

        /* Parse optional '0' pad flag */
        char pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }

        /* Parse optional width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
            case 'd':
            case 'i':
                print_int(__builtin_va_arg(args, int32_t));
                break;
            case 'u':
                print_uint(__builtin_va_arg(args, uint32_t), 10, width, pad);
                break;
            case 'x':
                print_uint(__builtin_va_arg(args, uint32_t), 16, width, pad);
                break;
            case 'p':
                serial_print("0x");
                print_uint(__builtin_va_arg(args, uint32_t), 16, 8, '0');
                break;
            case 's': {
                const char* s = __builtin_va_arg(args, const char*);
                serial_print(s ? s : "(null)");
                break;
            }
            case 'c':
                serial_putchar((char)__builtin_va_arg(args, int));
                break;
            case '%':
                serial_putchar('%');
                break;
            default:
                serial_putchar('%');
                serial_putchar(*fmt);
                break;
        }
        fmt++;
    }

    __builtin_va_end(args);
}

KAOS_EXPORT(serial_print)
KAOS_EXPORT(serial_printf)
KAOS_EXPORT(serial_putchar)
