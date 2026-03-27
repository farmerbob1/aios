/* AIOS v2 — Power Management (Shutdown & Restart)
 *
 * Shutdown: Proper ACPI — reads PM1a_CNT and SLP_TYP from parsed ACPI tables.
 * Restart:  PS/2 keyboard controller CPU reset pulse (0xFE to port 0x64).
 * Both flush the block cache and disable interrupts before acting. */

#include "../include/types.h"
#include "../include/io.h"
#include "../include/power.h"
#include "../include/acpi.h"
#include "../drivers/serial.h"
#include "chaos/block_cache.h"

#define ACPI_SLP_EN  (1 << 13)  /* bit 13: sleep enable */

/* PS/2 keyboard controller */
#define KB_CMD_PORT   0x64
#define KB_RESET_CMD  0xFE

static void power_prepare(void) {
    serial_print("[power] Flushing block cache...\n");
    block_cache_flush();
    serial_print("[power] Cache flushed. Proceeding.\n");
    __asm__ __volatile__("cli");
}

void system_shutdown(void) {
    power_prepare();

    uint16_t pm1a = acpi_get_pm1a_cnt();
    uint16_t pm1b = acpi_get_pm1b_cnt();
    int slp_typ   = acpi_get_s5_slp_typ();

    if (pm1a && slp_typ >= 0) {
        uint16_t val = ACPI_SLP_EN | ((uint16_t)slp_typ << 10);
        serial_printf("[power] ACPI shutdown: PM1a=0x%x val=0x%x (SLP_TYP=%d)\n",
                      pm1a, val, slp_typ);
        outw(pm1a, val);

        /* Also write to PM1b if present (some chipsets need both) */
        if (pm1b) {
            outw(pm1b, val);
        }

        /* Brief spin to let the hardware act */
        for (volatile int i = 0; i < 1000000; i++) {}
    } else {
        serial_print("[power] ACPI tables not available for shutdown\n");
    }

    /* Last resort: halt forever */
    serial_print("[power] ACPI shutdown failed, halting.\n");
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

void system_restart(void) {
    power_prepare();
    serial_print("[power] CPU reset via keyboard controller (0xFE)\n");

    /* Wait for keyboard controller input buffer to be clear */
    int timeout = 100000;
    while ((inb(KB_CMD_PORT) & 0x02) && --timeout > 0) {
        io_wait();
    }

    /* Send reset command */
    outb(KB_CMD_PORT, KB_RESET_CMD);

    /* Give it a moment */
    for (int i = 0; i < 100000; i++) {
        io_wait();
    }

    /* If keyboard reset didn't work, triple-fault */
    serial_print("[power] Keyboard reset failed, triple-faulting.\n");
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ __volatile__(
        "lidt %0\n\t"
        "int $3\n\t"
        : : "m"(null_idt)
    );

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
