/* AIOS v2 — Kernel Entry Point
 *
 * Called by the bootloader in 32-bit protected mode with:
 *   - Paging OFF (all addresses are physical)
 *   - Interrupts disabled (CLI)
 *   - ESP = 0x90000
 *   - [ESP+4] = pointer to boot_info at 0x10000
 *   - kernel_main must not return */

#include "../include/boot_info.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../drivers/timer.h"
#include "panic.h"
#include "boot_display.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "fpu.h"
#include "rdtsc.h"
#include "scheduler.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/input.h"
#include "../drivers/ata.h"
#include "../drivers/pci.h"
#include "chaos/chaos.h"
#include "../renderer/chaos_gl.h"
#include "kaos/kaos.h"
#include "boot_splash.h"
#include "net/netbuf.h"
#include "net/netif_bridge.h"
#include "net/entropy.h"

#define CHAOS_FS_LBA_START 2048  /* 1MB offset into disk */

/* ================================================================
 * Kernel Main
 * ================================================================ */

void kernel_main(struct boot_info* info) {
    /* ── Early init (no dependencies) ──────────────── */
    serial_init();
    serial_print("\n[AIOS v2] Kernel starting\n");

    /* VGA for boot display */
    vga_init();
    boot_display_banner();

    /* ── Phase 0 validation ────────────────────────── */
    if (info->magic != BOOT_MAGIC) {
        kernel_panic("boot_info magic mismatch!");
    }

    serial_printf("  boot_info @ %p\n", (uint32_t)info);
    serial_printf("  E820 entries: %u, max_phys_addr: 0x%08x (%u MB)\n",
        info->e820_count, info->max_phys_addr, info->max_phys_addr / (1024 * 1024));
    serial_printf("  kernel: 0x%08x - 0x%08x (%u segments)\n",
        info->kernel_phys_start, info->kernel_phys_end, info->kernel_segment_count);

    if (info->fb_addr != 0) {
        serial_printf("  framebuffer: %ux%ux%u @ 0x%08x\n",
            info->fb_width, info->fb_height, (uint32_t)info->fb_bpp, info->fb_addr);
    }

    boot_log("boot_info validation", INIT_OK);

    /* ── Phase 1: Memory ──────────────────────────── */
    init_result_t r;

    r = pmm_init(info);
    boot_log("Physical memory manager", r);
    if (r >= INIT_FAIL) kernel_panic("PMM init failed");

    r = vmm_init(info);
    boot_log("Virtual memory manager", r);
    if (r >= INIT_FAIL) kernel_panic("VMM init failed");

    r = heap_init(info);
    boot_log("Kernel heap (slab + buddy)", r);
    if (r >= INIT_FAIL) kernel_panic("Heap init failed");

    /* Tests removed — use 'make test' for headless test runs */

    /* ── Phase 2: Multitasking ────────────────────── */
    serial_print("\n[AIOS v2] Phase 2: Multitasking init\n");

    r = gdt_init();
    boot_log("GDT with TSS", r);
    if (r >= INIT_FAIL) kernel_panic("GDT init failed");

    r = idt_init();
    boot_log("Interrupt descriptor table", r);
    if (r >= INIT_FAIL) kernel_panic("IDT init failed");

    isr_init();
    boot_log("Exception handlers (ISR 0-31)", INIT_OK);

    irq_init();
    boot_log("Hardware IRQ handlers (PIC)", INIT_OK);

    fpu_init();
    boot_log("FPU/SSE", INIT_OK);

    r = timer_init();
    boot_log("PIT timer (250 Hz)", r);
    if (r >= INIT_FAIL) kernel_panic("Timer init failed");

    r = scheduler_init();
    boot_log("Preemptive scheduler", r);
    if (r >= INIT_FAIL) kernel_panic("Scheduler init failed");

    /* ── Phase 3: Drivers ─────────────────────────── */
    serial_print("\n[AIOS v2] Phase 3: Drivers init\n");

    r = keyboard_init();
    boot_log("PS/2 keyboard", r);

    r = mouse_init();
    boot_log("PS/2 mouse", r);

    r = fb_init(info);
    boot_log("Framebuffer HAL", r);

    r = ata_init();
    boot_log("ATA/IDE PIO disk", r);

    r = pci_init();
    boot_log("PCI bus", r);

    /* ── Phase 4: ChaosFS ─────────────────────────── */
    if (ata_is_present()) {
        r = chaos_mount(CHAOS_FS_LBA_START);
        boot_log("ChaosFS", r >= 0 ? INIT_OK : INIT_FAIL);
    }

    /* ── Phase 5: ChaosGL ─────────────────────────── */
    r = chaos_gl_init();
    boot_log("ChaosGL", r >= 0 ? INIT_OK : INIT_FAIL);

    /* Boot splash — graphical boot screen starts after ChaosGL */
    boot_splash_init();
    boot_splash_status("Initializing modules...");

    /* ── Network infrastructure (before KAOS, so E1000 module can use it) */
    entropy_init();
    netbuf_init();
    netif_bridge_init();

    /* ── Audio infrastructure (before KAOS, so AC97 module can use it) */
    extern void audio_bridge_init(void);
    audio_bridge_init();

    /* ── Phase 6: KAOS ──────────────────────────────── */
    r = kaos_init();
    boot_log("KAOS module system", r);

    boot_splash_status("Loading modules...");
    kaos_load_all("/system/modules/");

    /* ── Phase 11c: lwIP TCP/IP stack ─────────────────────── */
    boot_splash_status("Starting network...");
    extern init_result_t lwip_stack_init(void);
    r = lwip_stack_init();
    boot_log("lwIP TCP/IP", r);

    /* ── Phase 11e: BearSSL TLS ──────────────────────────── */
    boot_splash_status("Initializing TLS...");
    extern void bearssl_init(void);
    bearssl_init();
    boot_log("BearSSL TLS", INIT_OK);

    /* ── Phase 7: Lua Runtime ──────────────────────────── */
    boot_splash_status("Starting Lua runtime...");
    extern init_result_t lua_init(void);
    r = lua_init();
    boot_log("Lua 5.5 runtime", r);

    boot_splash_status("Loading desktop...");

    /* Launch desktop shell as a Lua task */
    extern int lua_task_create(const char *script_path, const char *task_name,
                               task_priority_t priority);
    lua_task_create("/system/desktop.lua", "desktop", PRIORITY_NORMAL);

    /* Enable interrupts — timer starts, preemption begins */
    __asm__ __volatile__("sti");
    serial_print("  Interrupts enabled\n");

    /* Calibrate RDTSC (needs timer running) */
    r = rdtsc_calibrate();
    boot_log("RDTSC calibration", r);

    /* Kernel task: sleep forever, let test runner and idle do the work */
    while (1) {
        task_sleep(60000);
    }
}
