/* AIOS v2 — Kernel Panic Handler
 * Prints message to serial + VGA and halts the system. */

#include "panic.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../include/kaos/export.h"

void kernel_panic(const char* msg) {
    /* Serial output */
    serial_print("\n*** KERNEL PANIC: ");
    serial_print(msg);
    serial_print(" ***\n");

    /* VGA output in bright red */
    vga_print_color("\n*** KERNEL PANIC: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print_color(msg, VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print_color(" ***\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));

    /* Halt forever */
    while (1) {
        __asm__ __volatile__("cli; hlt");
    }
}

KAOS_EXPORT(kernel_panic)
