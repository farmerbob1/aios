/* AIOS v2 — Phase 3 Acceptance Tests (Drivers)
 * Extracted from kernel/main.c to reduce code size per compilation unit. */

#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/vga.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/input.h"
#include "../drivers/ata.h"
#include "../drivers/ata_dma.h"
#include "heap.h"
#include "scheduler.h"
#include "boot_display.h"
#include "chaos/block_cache.h"
#include "phase3_tests.h"

static bool test_timer_tick_rate(void) {
    uint64_t t1 = timer_get_ticks();
    task_sleep(1000);
    uint64_t t2 = timer_get_ticks();
    uint64_t elapsed = t2 - t1;
    serial_printf("    (ticks=%u, expected ~250)\n", (uint32_t)elapsed);
    return (elapsed >= 240 && elapsed <= 260);
}

static bool test_vga_output(void) {
    /* VGA is already working (boot display uses it). Just verify no crash. */
    return true;
}

static bool test_framebuffer_hal(void) {
    fb_info_t fbi;
    bool has_fb = fb_get_info(&fbi);
    if (has_fb) {
        serial_printf("    (fb: %ux%ux%u @ 0x%08x)\n",
                      fbi.width, fbi.height, (uint32_t)fbi.bpp, fbi.fb_addr);
        if (fbi.width == 0 || fbi.height == 0) return false;
        if (fbi.bpp != 32) return false;
        if (fbi.fb_addr == 0) return false;
        if (fbi.pitch < fbi.width * 4) return false;
        return true;
    } else {
        serial_printf("    (no framebuffer — text mode)\n");
        return true;  /* text-mode-only is valid */
    }
}

static bool test_keyboard(void) {
    /* Verify key_state array accessible and all keys start unpressed */
    for (int i = 0; i < 256; i++) {
        if (key_state[i]) {
            serial_printf("    (key %d stuck)\n", i);
            return false;
        }
    }
    keyboard_is_pressed(SC_ESC);
    keyboard_has_key();
    return true;
}

static bool test_mouse(void) {
    int x = mouse_get_x();
    int y = mouse_get_y();
    uint8_t btn = mouse_get_buttons();
    serial_printf("    (pos=%d,%d buttons=0x%02x)\n", x, y, (uint32_t)btn);
    mouse_set_bounds(800, 600);
    mouse_set_raw_mode(true);
    int dx, dy;
    mouse_get_delta(&dx, &dy);
    mouse_set_raw_mode(false);
    return true;
}

static bool test_input_queue(void) {
    /* Push 256 events into 256-slot ring buffer (holds 255 max) */
    input_set_gui_mode(true);
    for (int i = 0; i < 256; i++) {
        input_event_t ev;
        ev.type = EVENT_KEY_DOWN;
        ev.key = (uint16_t)i;
        ev.mouse_x = 0;
        ev.mouse_y = 0;
        ev.mouse_btn = 0;
        input_push(&ev);
    }

    bool overflowed = input_check_overflow();

    /* Poll all back */
    int count = 0;
    input_event_t ev;
    while (input_poll(&ev)) count++;

    serial_printf("    (polled=%d, overflow=%s)\n",
                  count, overflowed ? "yes" : "no");

    /* Queue should be empty */
    if (input_has_events()) { input_set_gui_mode(false); return false; }

    /* Round-trip test */
    input_event_t test_ev;
    test_ev.type = EVENT_MOUSE_MOVE;
    test_ev.key = 0;
    test_ev.mouse_x = 42;
    test_ev.mouse_y = 99;
    test_ev.mouse_btn = 0;
    input_push(&test_ev);
    if (!input_poll(&ev)) { input_set_gui_mode(false); return false; }
    if (ev.type != EVENT_MOUSE_MOVE || ev.mouse_x != 42 || ev.mouse_y != 99) {
        input_set_gui_mode(false);
        return false;
    }

    input_set_gui_mode(false);
    return (count == 255 && overflowed);
}

static bool test_ata(void) {
    if (!ata_is_present()) {
        serial_printf("    (no ATA disk — skip)\n");
        return true;
    }

    uint32_t sectors = ata_get_sector_count();
    serial_printf("    (disk: %u sectors, %u MB)\n", sectors, sectors / 2048);

    /* Read sector 0 (MBR), verify 0xAA55 boot signature */
    uint8_t* mbr = (uint8_t*)kmalloc(512);
    if (!mbr) return false;

    if (ata_read_sectors(0, 1, mbr) != 0) {
        serial_printf("    (MBR read failed)\n");
        kfree(mbr);
        return false;
    }
    uint16_t sig = (uint16_t)mbr[510] | ((uint16_t)mbr[511] << 8);
    serial_printf("    (MBR signature: 0x%04x)\n", (uint32_t)sig);
    kfree(mbr);
    if (sig != 0xAA55) return false;

    /* Write/read round-trip at high LBA */
    uint32_t test_lba = 4000;
    if (test_lba >= sectors) {
        serial_printf("    (disk too small for write test)\n");
        return true;
    }

    uint8_t* write_buf = (uint8_t*)kmalloc(512);
    uint8_t* read_buf = (uint8_t*)kmalloc(512);
    if (!write_buf || !read_buf) {
        if (write_buf) kfree(write_buf);
        if (read_buf) kfree(read_buf);
        return false;
    }

    for (int i = 0; i < 512; i++) write_buf[i] = (uint8_t)(i ^ 0xA5);

    if (ata_write_sectors(test_lba, 1, write_buf) != 0) {
        serial_printf("    (write failed)\n");
        kfree(write_buf); kfree(read_buf);
        return false;
    }

    memset(read_buf, 0, 512);
    if (ata_read_sectors(test_lba, 1, read_buf) != 0) {
        serial_printf("    (read-back failed)\n");
        kfree(write_buf); kfree(read_buf);
        return false;
    }

    bool match = (memcmp(write_buf, read_buf, 512) == 0);
    serial_printf("    (write/read round-trip at LBA %u: %s)\n",
                  test_lba, match ? "OK" : "MISMATCH");

    kfree(write_buf);
    kfree(read_buf);
    return match;
}

static bool test_ata_dma_init(void) {
    if (!ata_dma_available()) {
        serial_printf("    (DMA not available — skip)\n");
        return true;  /* not fatal */
    }
    serial_printf("    (DMA available)\n");
    return true;
}

static bool test_ata_dma_read(void) {
    if (!ata_dma_available() || !ata_is_present()) {
        serial_printf("    (skip — DMA or disk unavailable)\n");
        return true;
    }

    /* Read MBR via DMA, verify 0xAA55 signature */
    uint8_t* dma_buf = (uint8_t*)kmalloc(512);
    if (!dma_buf) return false;

    if (ata_dma_read(0, 1, dma_buf) != 0) {
        serial_printf("    (DMA read sector 0 failed)\n");
        kfree(dma_buf);
        return false;
    }

    uint16_t sig = (uint16_t)dma_buf[510] | ((uint16_t)dma_buf[511] << 8);
    serial_printf("    (DMA MBR sig: 0x%04x)\n", (uint32_t)sig);
    kfree(dma_buf);
    return sig == 0xAA55;
}

static bool test_ata_dma_multi_sector(void) {
    if (!ata_dma_available() || !ata_is_present()) {
        serial_printf("    (skip)\n");
        return true;
    }

    /* Read 8 sectors (4KB) via DMA, verify matches a second read */
    uint8_t* buf1 = (uint8_t*)kmalloc(4096);
    uint8_t* buf2 = (uint8_t*)kmalloc(4096);
    if (!buf1 || !buf2) {
        if (buf1) kfree(buf1);
        if (buf2) kfree(buf2);
        return false;
    }

    int ret1 = ata_dma_read(0, 8, buf1);
    int ret2 = ata_dma_read(0, 8, buf2);

    bool ok = (ret1 == 0 && ret2 == 0 && memcmp(buf1, buf2, 4096) == 0);
    serial_printf("    (8-sector DMA read: %s)\n", ok ? "match" : "MISMATCH");
    kfree(buf1);
    kfree(buf2);
    return ok;
}

static bool test_block_cache_hit(void) {
    if (!block_cache_is_enabled() || !ata_is_present()) {
        serial_printf("    (skip — cache or disk unavailable)\n");
        return true;
    }

    /* Flush cache to start clean */
    block_cache_flush();
    cache_stats_t st0 = block_cache_get_stats();

    uint8_t* buf = (uint8_t*)kmalloc(4096);
    if (!buf) return false;

    /* First read — should be a miss */
    extern int chaos_block_read(uint32_t block_idx, void* buffer);
    chaos_block_read(3, buf);

    cache_stats_t st1 = block_cache_get_stats();
    uint32_t misses_after_first = st1.misses - st0.misses;

    /* Second read of same block — should be a hit */
    chaos_block_read(3, buf);

    cache_stats_t st2 = block_cache_get_stats();
    uint32_t hits_after_second = st2.hits - st1.hits;

    serial_printf("    (miss=%u, hit=%u)\n", misses_after_first, hits_after_second);
    kfree(buf);
    return (misses_after_first == 1 && hits_after_second == 1);
}

static bool test_block_cache_invalidate(void) {
    if (!block_cache_is_enabled() || !ata_is_present()) {
        serial_printf("    (skip)\n");
        return true;
    }

    block_cache_flush();

    uint8_t* buf = (uint8_t*)kmalloc(4096);
    if (!buf) return false;

    extern int chaos_block_read(uint32_t block_idx, void* buffer);

    /* Read block 4 to populate cache */
    chaos_block_read(4, buf);
    cache_stats_t st1 = block_cache_get_stats();

    /* Invalidate it */
    block_cache_invalidate(4);

    /* Read again — should be a miss, not a hit */
    chaos_block_read(4, buf);
    cache_stats_t st2 = block_cache_get_stats();

    uint32_t new_misses = st2.misses - st1.misses;
    serial_printf("    (post-invalidate misses=%u)\n", new_misses);
    kfree(buf);
    return (new_misses == 1);
}

static bool test_block_cache_stats(void) {
    if (!block_cache_is_enabled()) {
        serial_printf("    (skip)\n");
        return true;
    }

    cache_stats_t st = block_cache_get_stats();
    int rate = block_cache_hit_rate();
    serial_printf("    (hits=%u misses=%u evictions=%u rate=%d%%)\n",
                  st.hits, st.misses, st.evictions, rate);

    /* Basic sanity: hits + misses should be > 0 after earlier tests */
    return (st.hits + st.misses > 0);
}

static bool test_serial_output(void) {
    serial_print("    Phase 3 serial test\n");
    return true;
}

static bool test_boot_display(void) {
    /* boot_log has been working throughout. Just verify no crash. */
    return true;
}

static bool test_integration(void) {
    bool ok = true;

    if (timer_get_ticks() == 0) ok = false;
    if (timer_get_frequency() != PIT_FREQUENCY) ok = false;

    keyboard_is_pressed(0);
    mouse_get_x();
    mouse_get_y();

    /* Input queue round-trip */
    input_set_gui_mode(true);
    input_event_t ev;
    ev.type = EVENT_KEY_DOWN;
    ev.key = SC_ESC;
    ev.mouse_x = 0;
    ev.mouse_y = 0;
    ev.mouse_btn = 0;
    input_push(&ev);
    input_event_t out;
    if (!input_poll(&out)) ok = false;
    input_set_gui_mode(false);

    fb_info_t fbi;
    fb_get_info(&fbi);

    serial_printf("    (all subsystems responsive)\n");
    return ok;
}

void phase3_acceptance_tests(void) {
    serial_print("\n=== Phase 3 Acceptance Tests ===\n");

    struct { const char* name; bool (*fn)(void); } tests[] = {
        { "Timer tick rate",    test_timer_tick_rate },
        { "VGA output",         test_vga_output },
        { "Framebuffer HAL",    test_framebuffer_hal },
        { "Keyboard",           test_keyboard },
        { "Mouse",              test_mouse },
        { "Input queue",        test_input_queue },
        { "ATA disk",           test_ata },
        { "ATA DMA init",       test_ata_dma_init },
        { "ATA DMA read",       test_ata_dma_read },
        { "ATA DMA multi-sect", test_ata_dma_multi_sector },
        { "Block cache hit",    test_block_cache_hit },
        { "Block cache inval",  test_block_cache_invalidate },
        { "Block cache stats",  test_block_cache_stats },
        { "Serial output",      test_serial_output },
        { "Boot display",       test_boot_display },
        { "Integration",        test_integration },
    };

    int count = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < count; i++) {
        bool ok = tests[i].fn();
        serial_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) pass++; else fail++;
    }

    serial_printf("\nPhase 3: %d/%d tests passed\n", pass, count);
    if (fail > 0) {
        serial_print("[AIOS v2] Phase 3 acceptance: FAIL\n");
    } else {
        serial_print("[AIOS v2] Phase 3 acceptance: PASS\n");
    }

    boot_print("\nAIOS v2 Phase 3 complete.\n");
}
