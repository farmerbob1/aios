/* AIOS v2 — Boot Display
 * Unified boot logging with honest status reporting.
 * Uses VGA text mode. Mirrors all output to serial. */

#include "boot_display.h"
#include "../drivers/vga.h"
#include "../drivers/serial.h"

void boot_log(const char* component, init_result_t result) {
    boot_log_detail(component, result, NULL);
}

void boot_log_detail(const char* component, init_result_t result, const char* detail) {
    /* VGA output */
    vga_print("[BOOT] ");
    vga_print(component);
    vga_print("... ");

    switch (result) {
        case INIT_OK:
            vga_print_color("OK", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            break;
        case INIT_WARN:
            vga_print_color("WARN", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            break;
        case INIT_FAIL:
            vga_print_color("FAIL", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            break;
        case INIT_FATAL:
            vga_print_color("FATAL", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print("\n\nBoot halted. Cannot continue.\n");
            serial_print("[BOOT] FATAL: ");
            serial_print(component);
            serial_print("\n");
            while (1) __asm__ __volatile__("hlt");
    }

    if (detail) {
        vga_print("  ");
        vga_print(detail);
    }
    vga_putchar('\n');

    /* Mirror to serial */
    serial_print("[BOOT] ");
    serial_print(component);
    switch (result) {
        case INIT_OK:   serial_print(" OK"); break;
        case INIT_WARN: serial_print(" WARN"); break;
        case INIT_FAIL: serial_print(" FAIL"); break;
        case INIT_FATAL: serial_print(" FATAL"); break;
    }
    if (detail) {
        serial_print("  ");
        serial_print(detail);
    }
    serial_print("\n");
}

void boot_print(const char* msg) {
    vga_print(msg);
    serial_print(msg);
}

void boot_display_banner(void) {
    const char* banner =
        "    _    ___ ___  ____          ____  \n"
        "   / \\  |_ _/ _ \\/ ___|  __   |___ \\ \n"
        "  / _ \\  | | | | \\___ \\  \\ \\ / / __) |\n"
        " / ___ \\ | | |_| |___) |  \\ V / / __/ \n"
        "/_/   \\_\\___\\___/|____/    \\_/ |_____|\n"
        "\n";
    vga_print(banner);
    serial_print(banner);
}
