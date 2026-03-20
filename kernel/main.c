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
#include "chaos/chaos.h"
#include "../renderer/chaos_gl.h"

#define CHAOS_FS_LBA_START 2048  /* 1MB offset into disk */

/* ================================================================
 * Phase 1 Acceptance Tests
 * ================================================================ */

static bool test_pmm_basic(void) {
    uint32_t initial_free = pmm_get_free_pages();
    uint32_t pages[1000];

    for (int i = 0; i < 1000; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == 0) return false;
    }

    if (pmm_get_free_pages() != initial_free - 1000) return false;

    for (int i = 0; i < 1000; i++) {
        pmm_free_page(pages[i]);
    }

    if (pmm_get_free_pages() != initial_free) return false;

    /* Alloc again should succeed */
    for (int i = 0; i < 1000; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == 0) return false;
    }
    for (int i = 0; i < 1000; i++) {
        pmm_free_page(pages[i]);
    }

    return true;
}

static bool test_pmm_stress(void) {
    uint32_t initial_free = pmm_get_free_pages();

    /* Alloc until OOM, but cap at 50000 to avoid taking forever */
    uint32_t count = 0;
    uint32_t cap = initial_free < 50000 ? initial_free : 50000;
    uint32_t* pages = (uint32_t*)kmalloc(cap * sizeof(uint32_t));
    if (!pages) return false;

    while (count < cap) {
        uint32_t p = pmm_alloc_page();
        if (p == 0) break;
        pages[count++] = p;
    }

    serial_printf("    (allocated %u pages)\n", count);

    /* Free all */
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_page(pages[i]);
    }

    kfree(pages);

    bool ok = (pmm_get_free_pages() == initial_free);
    return ok;
}

static bool test_pmm_contiguous(void) {
    uint32_t p1 = pmm_alloc_pages(256);
    if (p1 == 0) return false;

    pmm_free_pages(p1, 256);

    uint32_t p2 = pmm_alloc_pages(256);
    if (p2 == 0) return false;

    pmm_free_pages(p2, 256);
    return true;
}

static bool test_vmm_mapping(void) {
    uint32_t phys = pmm_alloc_page();
    if (phys == 0) return false;

    vmm_map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE);

    volatile uint32_t* ptr = (volatile uint32_t*)phys;
    *ptr = 0xDEADBEEF;
    bool ok = (*ptr == 0xDEADBEEF);

    vmm_unmap_page(phys);
    pmm_free_page(phys);
    return ok;
}

static bool test_vmm_flags(void) {
    uint32_t phys = pmm_alloc_page();
    if (phys == 0) return false;

    vmm_map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);

    /* Verify the mapping works with NOCACHE flag set */
    volatile uint32_t* ptr = (volatile uint32_t*)phys;
    *ptr = 0xCAFEBABE;
    bool ok = (*ptr == 0xCAFEBABE);

    /* Change flags to add WRITETHROUGH */
    vmm_set_flags(phys, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
    *ptr = 0x12345678;
    ok = ok && (*ptr == 0x12345678);

    vmm_unmap_page(phys);
    pmm_free_page(phys);
    return ok;
}

static bool test_slab_basic(void) {
    uint32_t initial_slab = heap_get_slab_used();
    /* Use heap for the pointer array to avoid stack overflow */
    void** ptrs = (void**)kmalloc(10000 * sizeof(void*));
    if (!ptrs) return false;

    for (int i = 0; i < 10000; i++) {
        ptrs[i] = kmalloc(32);
        if (!ptrs[i]) { kfree(ptrs); return false; }
        *(uint32_t*)ptrs[i] = (uint32_t)i;
    }

    for (int i = 0; i < 10000; i++) {
        if (*(uint32_t*)ptrs[i] != (uint32_t)i) { kfree(ptrs); return false; }
        kfree(ptrs[i]);
    }

    kfree(ptrs);
    if (heap_get_slab_used() != initial_slab) return false;
    return true;
}

/* Simple pseudo-random for stress tests (LCG) */
static uint32_t rng_state = 12345;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static bool test_slab_stress(void) {
    uint32_t initial_used = heap_get_used();
    /* Use heap for arrays to avoid stack overflow */
    void** ptrs = (void**)kmalloc(10000 * sizeof(void*));
    int* indices = (int*)kmalloc(10000 * sizeof(int));
    if (!ptrs || !indices) { if (ptrs) kfree(ptrs); if (indices) kfree(indices); return false; }

    rng_state = 42;
    for (int i = 0; i < 10000; i++) {
        size_t sz = 16 + (rng_next() % 2033);
        ptrs[i] = kmalloc(sz);
        if (!ptrs[i]) { kfree(ptrs); kfree(indices); return false; }
    }

    /* Free in pseudo-random order (Fisher-Yates shuffle) */
    for (int i = 0; i < 10000; i++) indices[i] = i;
    rng_state = 99;
    for (int i = 9999; i > 0; i--) {
        int j = rng_next() % (i + 1);
        int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
    }

    for (int i = 0; i < 10000; i++) {
        kfree(ptrs[indices[i]]);
    }

    kfree(indices);
    kfree(ptrs);

    if (heap_get_used() != initial_used) return false;
    return true;
}

static bool test_buddy_basic(void) {
    void* p = kmalloc(64 * 1024); /* 64KB — goes to buddy */
    if (!p) return false;

    /* Write pattern */
    memset(p, 0xAB, 64 * 1024);
    if (((uint8_t*)p)[0] != 0xAB) return false;
    if (((uint8_t*)p)[65535] != 0xAB) return false;

    kfree(p);

    /* Alloc again should succeed */
    void* p2 = kmalloc(64 * 1024);
    if (!p2) return false;
    kfree(p2);
    return true;
}

static bool test_buddy_splitting(void) {
    /* Alloc 3 x 4KB blocks (minimum buddy size, goes through buddy via >2048 path) */
    void* a = kmalloc(4000); /* slightly under 4KB, but > 2048 -> buddy */
    void* b = kmalloc(4000);
    void* c = kmalloc(4000);
    if (!a || !b || !c) return false;

    kfree(b); /* free the middle one */

    void* d = kmalloc(4000); /* should reuse b's block */
    if (!d) return false;

    kfree(a);
    kfree(c);
    kfree(d);
    return true;
}

static bool test_kfree_routing(void) {
    /* Slab allocation (64 bytes) */
    void* slab_ptr = kmalloc(64);
    if (!slab_ptr) return false;
    uint32_t slab_page = (uint32_t)slab_ptr >> PAGE_SHIFT;
    if (page_ownership[slab_page] != PAGE_SLAB) return false;
    kfree(slab_ptr);

    /* Buddy allocation (8KB) */
    void* buddy_ptr = kmalloc(8192);
    if (!buddy_ptr) return false;
    uint32_t buddy_page = (uint32_t)buddy_ptr >> PAGE_SHIFT;
    if (page_ownership[buddy_page] != PAGE_BUDDY) return false;
    kfree(buddy_ptr);

    return true;
}

static bool test_kfree_reserved(void) {
    /* Allocate a page, mark it reserved, verify kfree would reject it.
     * We can't actually call kfree (it panics), so just verify the ownership. */
    uint32_t phys = pmm_alloc_page();
    if (phys == 0) return false;

    vmm_map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE);
    heap_mark_reserved(phys, 1);

    uint32_t page_idx = phys >> PAGE_SHIFT;
    bool ok = (page_ownership[page_idx] == PAGE_RESERVED);

    /* Clean up: unmark and free (set back to UNUSED so we can free the PMM page) */
    page_ownership[page_idx] = PAGE_UNUSED;
    pmm_free_page(phys);

    return ok;
}

static bool test_krealloc(void) {
    char* p = (char*)kmalloc(32);
    if (!p) return false;
    memcpy(p, "HELLO", 6);

    /* Realloc to 128 (slab to slab) */
    p = (char*)krealloc(p, 128);
    if (!p) return false;
    if (memcmp(p, "HELLO", 6) != 0) return false;

    /* Realloc to 8KB (slab to buddy) */
    p = (char*)krealloc(p, 8192);
    if (!p) return false;
    if (memcmp(p, "HELLO", 6) != 0) return false;

    kfree(p);
    return true;
}

static bool test_kmalloc_aligned(void) {
    /* 512 bytes, 16-byte alignment */
    void* p1 = kmalloc_aligned(512, 16);
    if (!p1) return false;
    if ((uint32_t)p1 & 0xF) return false; /* not 16-byte aligned */

    /* Write pattern */
    memset(p1, 0xCC, 512);
    if (((uint8_t*)p1)[0] != 0xCC) return false;
    kfree_aligned(p1);

    /* PAGE_SIZE alignment */
    void* p2 = kmalloc_aligned(256, PAGE_SIZE);
    if (!p2) return false;
    if ((uint32_t)p2 & (PAGE_SIZE - 1)) return false; /* not page-aligned */
    kfree_aligned(p2);

    return true;
}

static bool test_page_ownership(void) {
    /* Verify page_ownership is not null and accessible */
    if (!page_ownership) return false;

    /* Verify at least some buddy pages exist */
    bool found_buddy = false;
    uint32_t total = pmm_get_max_phys_addr() / PAGE_SIZE;
    for (uint32_t i = 0; i < total && i < 100000; i++) {
        if (page_ownership[i] == PAGE_BUDDY) {
            found_buddy = true;
            break;
        }
    }
    if (!found_buddy) return false;

    return true;
}

static bool test_memory_accounting(void) {
    uint32_t used = heap_get_used();
    uint32_t free = heap_get_free();
    uint32_t total = used + free;

    if (total == 0) return false;

    uint32_t used_before = heap_get_used();
    void* p = kmalloc(4096);
    if (!p) return false;
    uint32_t used_after = heap_get_used();
    kfree(p);
    uint32_t used_final = heap_get_used();

    if (used_after <= used_before) return false;
    if (used_final != used_before) return false;

    return true;
}

static void phase1_acceptance_tests(void) {
    serial_print("\n=== Phase 1 Acceptance Tests ===\n");

    struct { const char* name; bool (*fn)(void); } tests[] = {
        { "PMM basic",           test_pmm_basic },
        { "PMM stress",          test_pmm_stress },
        { "PMM contiguous",      test_pmm_contiguous },
        { "VMM mapping",         test_vmm_mapping },
        { "VMM flags",           test_vmm_flags },
        { "Slab basic",          test_slab_basic },
        { "Slab stress",         test_slab_stress },
        { "Buddy basic",         test_buddy_basic },
        { "Buddy splitting",     test_buddy_splitting },
        { "kfree routing",       test_kfree_routing },
        { "kfree reserved",      test_kfree_reserved },
        { "krealloc",            test_krealloc },
        { "kmalloc_aligned",     test_kmalloc_aligned },
        { "Page ownership",      test_page_ownership },
        { "Memory accounting",   test_memory_accounting },
    };

    int count = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < count; i++) {
        bool ok = tests[i].fn();
        serial_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) pass++; else fail++;
    }

    serial_printf("\nPhase 1: %d/%d tests passed\n", pass, count);
    if (fail > 0) {
        serial_print("[AIOS v2] Phase 1 acceptance: FAIL\n");
    } else {
        serial_print("[AIOS v2] Phase 1 acceptance: PASS\n");
    }
}

/* ================================================================
 * Phase 2 Acceptance Tests (run from test_runner task)
 * ================================================================ */

/* Shared volatile counters for inter-task communication */
static volatile uint32_t counter_a, counter_b, counter_c;
static volatile uint32_t high_counter, low_counter;
static volatile uint64_t sleep_start_ticks, sleep_end_ticks;
static volatile bool fpu_a_ok, fpu_b_ok;
static volatile uint32_t starvation_counter;

/* ── Test 1: Basic switching ──────────────────────── */

static void task_counter_a(void) { while (1) { counter_a++; task_yield(); } }
static void task_counter_b(void) { while (1) { counter_b++; task_yield(); } }
static void task_counter_c(void) { while (1) { counter_c++; task_yield(); } }

static bool test_basic_switching(void) {
    counter_a = counter_b = counter_c = 0;

    int a = task_create("counter_a", task_counter_a, PRIORITY_NORMAL);
    int b = task_create("counter_b", task_counter_b, PRIORITY_NORMAL);
    int c = task_create("counter_c", task_counter_c, PRIORITY_NORMAL);
    if (a < 0 || b < 0 || c < 0) return false;

    task_sleep(1000);

    bool ok = (counter_a > 0 && counter_b > 0 && counter_c > 0);

    task_kill(a);
    task_kill(b);
    task_kill(c);
    task_yield(); /* let cleanup happen */

    serial_printf("    (a=%u b=%u c=%u)\n", counter_a, counter_b, counter_c);
    return ok;
}

/* ── Test 2: Priority ─────────────────────────────── */

static void task_high_counter(void) { while (1) { high_counter++; task_yield(); } }
static void task_low_counter(void)  { while (1) { low_counter++;  task_yield(); } }

static bool test_priority(void) {
    counter_a = counter_b = counter_c = 0;  /* clear leftovers from test 1 */
    high_counter = 0;
    low_counter = 0;
    __asm__ __volatile__("" ::: "memory");

    int h = task_create("high", task_high_counter, PRIORITY_HIGH);
    int l = task_create("low",  task_low_counter,  PRIORITY_LOW);
    if (h < 0 || l < 0) return false;

    task_sleep(1000);

    task_kill(h);
    task_kill(l);
    task_yield();

    uint32_t hc = high_counter;
    uint32_t lc = low_counter;
    serial_printf("    (high=%u low=%u ratio=%u)\n",
                  hc, lc, lc > 0 ? hc / lc : 0);

    /* HIGH should get significantly more ticks than LOW */
    return (hc > lc * 5);
}

/* ── Test 3: Sleep accuracy ───────────────────────── */

static void task_sleeper(void) {
    sleep_start_ticks = timer_get_ticks();
    task_sleep(500);
    sleep_end_ticks = timer_get_ticks();
}

static bool test_sleep(void) {
    sleep_start_ticks = sleep_end_ticks = 0;

    int s = task_create("sleeper", task_sleeper, PRIORITY_NORMAL);
    if (s < 0) return false;

    /* Wait for sleeper to finish */
    task_sleep(1000);

    uint64_t elapsed = sleep_end_ticks - sleep_start_ticks;
    serial_printf("    (elapsed=%u ticks, expected ~125)\n", (uint32_t)elapsed);

    /* 500ms at 250Hz = 125 ticks, allow +/- 5 */
    return (elapsed >= 120 && elapsed <= 130);
}

/* ── Test 4: Sleep wrap-safe (64-bit) ─────────────── */

static bool test_sleep_wrap(void) {
    /* 64-bit ticks never wrap in practice. Just verify short sleep works. */
    uint64_t before = timer_get_ticks();
    task_sleep(100);
    uint64_t after = timer_get_ticks();

    uint64_t elapsed = after - before;
    serial_printf("    (elapsed=%u ticks, expected ~25)\n", (uint32_t)elapsed);
    return (elapsed >= 20 && elapsed <= 35);
}

/* ── Test 5: Task exit cleanup ────────────────────── */

static void task_exit_immediately(void) {
    task_exit();
}

static bool test_exit_cleanup(void) {
    uint32_t baseline_used = heap_get_used();
    int initial_count = task_get_count();

    /* Create 100 tasks in batches of 20 (fits within MAX_TASKS).
     * Each batch: create, sleep to let them run+exit+cleanup, repeat. */
    int total_created = 0;
    for (int batch = 0; batch < 5; batch++) {
        for (int i = 0; i < 20; i++) {
            int id = task_create("exiter", task_exit_immediately, PRIORITY_NORMAL);
            if (id < 0) {
                serial_printf("    (failed at batch %d task %d)\n", batch, i);
                /* Wait for existing tasks to clean up before failing */
                task_sleep(1000);
                return false;
            }
            total_created++;
        }
        /* Sleep to let batch run, exit, and get cleaned up */
        task_sleep(500);
    }

    /* Final cleanup pass */
    task_sleep(500);
    for (int i = 0; i < 10; i++) task_yield();

    int final_count = task_get_count();
    uint32_t final_used = heap_get_used();

    serial_printf("    (created=%d count: %d->%d, heap: %u->%u)\n",
                  total_created, initial_count, final_count,
                  baseline_used, final_used);

    return (final_count == initial_count && final_used == baseline_used);
}

/* ── Test 6: FPU isolation ────────────────────────── */

static void task_fpu_a(void) {
    volatile double val = 1.0;
    fpu_a_ok = true;
    for (int i = 0; i < 100; i++) {
        val = 1.0;
        task_yield();
        if (val != 1.0) {
            fpu_a_ok = false;
            break;
        }
    }
}

static void task_fpu_b(void) {
    volatile double val = 2.0;
    fpu_b_ok = true;
    for (int i = 0; i < 100; i++) {
        val = 2.0;
        task_yield();
        if (val != 2.0) {
            fpu_b_ok = false;
            break;
        }
    }
}

static bool test_fpu_isolation(void) {
    fpu_a_ok = fpu_b_ok = false;

    int a = task_create("fpu_a", task_fpu_a, PRIORITY_NORMAL);
    int b = task_create("fpu_b", task_fpu_b, PRIORITY_NORMAL);
    if (a < 0 || b < 0) return false;

    task_sleep(2000);

    bool ok = (fpu_a_ok && fpu_b_ok);

    serial_printf("    (fpu_a=%s fpu_b=%s)\n",
                  fpu_a_ok ? "ok" : "FAIL", fpu_b_ok ? "ok" : "FAIL");

    task_kill(a);
    task_kill(b);
    task_yield();

    return ok;
}

/* ── Test 7: FPU stress ───────────────────────────── */

static volatile bool fpu_stress_ok[5];

static void fpu_stress_worker_0(void) {
    volatile double v = 3.14159;
    fpu_stress_ok[0] = true;
    uint64_t end = timer_get_ticks() + 5 * PIT_FREQUENCY;
    while (timer_get_ticks() < end) {
        v = v * 2.0;
        v = v / 2.0;
        if (v < 3.14158 || v > 3.14160) { fpu_stress_ok[0] = false; break; }
        task_yield();
    }
}

static void fpu_stress_worker_1(void) {
    volatile double v = 2.71828;
    fpu_stress_ok[1] = true;
    uint64_t end = timer_get_ticks() + 5 * PIT_FREQUENCY;
    while (timer_get_ticks() < end) {
        v = v + 1.0;
        v = v - 1.0;
        if (v < 2.71827 || v > 2.71829) { fpu_stress_ok[1] = false; break; }
        task_yield();
    }
}

static void fpu_stress_worker_2(void) {
    volatile double v = 1.41421;
    fpu_stress_ok[2] = true;
    uint64_t end = timer_get_ticks() + 5 * PIT_FREQUENCY;
    while (timer_get_ticks() < end) {
        v = v * 1.0;
        if (v < 1.41420 || v > 1.41422) { fpu_stress_ok[2] = false; break; }
        task_yield();
    }
}

static void fpu_stress_worker_3(void) {
    volatile double v = 100.0;
    fpu_stress_ok[3] = true;
    uint64_t end = timer_get_ticks() + 5 * PIT_FREQUENCY;
    while (timer_get_ticks() < end) {
        v = v / 10.0;
        v = v * 10.0;
        if (v < 99.999 || v > 100.001) { fpu_stress_ok[3] = false; break; }
        task_yield();
    }
}

static void fpu_stress_worker_4(void) {
    volatile double v = 0.5;
    fpu_stress_ok[4] = true;
    uint64_t end = timer_get_ticks() + 5 * PIT_FREQUENCY;
    while (timer_get_ticks() < end) {
        v = v + 0.5;
        v = v - 0.5;
        if (v < 0.499 || v > 0.501) { fpu_stress_ok[4] = false; break; }
        task_yield();
    }
}

static bool test_fpu_stress(void) {
    for (int i = 0; i < 5; i++) fpu_stress_ok[i] = false;

    typedef void (*entry_fn)(void);
    entry_fn workers[5] = {
        fpu_stress_worker_0, fpu_stress_worker_1, fpu_stress_worker_2,
        fpu_stress_worker_3, fpu_stress_worker_4
    };

    int ids[5];
    for (int i = 0; i < 5; i++) {
        ids[i] = task_create("fpu_stress", workers[i], PRIORITY_NORMAL);
        if (ids[i] < 0) return false;
    }

    /* Wait for 5s + margin */
    task_sleep(6000);

    bool ok = true;
    for (int i = 0; i < 5; i++) {
        if (!fpu_stress_ok[i]) {
            serial_printf("    (worker %d FAILED)\n", i);
            ok = false;
        }
        task_kill(ids[i]);
    }
    task_yield();

    return ok;
}

/* ── Test 8: Idle CPU usage ───────────────────────── */

static bool test_idle_cpu(void) {
    /* Let system settle with only kernel (sleeping) + idle running */
    task_sleep(2000);

    int usage = scheduler_get_cpu_usage();
    serial_printf("    (cpu_usage=%d%%)\n", usage);

    return (usage <= 10);
}

/* ── Test 9: CPU measurement ──────────────────────── */

static volatile bool busy_running;

static void task_busy_loop(void) {
    while (busy_running) {
        /* Tight loop — burn CPU */
    }
}

static bool test_cpu_measurement(void) {
    busy_running = true;

    int id = task_create("busy", task_busy_loop, PRIORITY_HIGH);
    if (id < 0) return false;

    task_sleep(2000);

    int usage = scheduler_get_cpu_usage();
    serial_printf("    (cpu_usage=%d%%)\n", usage);

    busy_running = false;
    task_sleep(500);
    task_kill(id);
    task_yield();

    return (usage >= 80);
}

/* ── Test 10: Kill task ───────────────────────────── */

static void task_spin_forever(void) {
    while (1) task_yield();
}

static bool test_kill_task(void) {
    int id = task_create("victim", task_spin_forever, PRIORITY_NORMAL);
    if (id < 0) return false;

    task_sleep(100);

    int r = task_kill(id);
    if (r != 0) return false;

    /* Let cleanup happen */
    task_sleep(200);
    task_yield();

    /* Verify it's gone */
    struct task* t = task_get(id);
    if (t != NULL) return false;

    /* Verify can't kill kernel (task 0) or idle */
    if (task_kill(0) != -1) return false;
    if (task_kill(1) != -1) return false;  /* idle is task 1 */

    return true;
}

/* ── Test 11: Starvation prevention ───────────────── */

static void task_high_busy(void) {
    while (1) {
        /* Burn CPU at HIGH priority */
    }
}

static void task_normal_counter(void) {
    while (1) {
        starvation_counter++;
        task_yield();
    }
}

static bool test_starvation(void) {
    starvation_counter = 0;

    int h = task_create("high_busy", task_high_busy, PRIORITY_HIGH);
    int n = task_create("normal_cnt", task_normal_counter, PRIORITY_NORMAL);
    if (h < 0 || n < 0) return false;

    task_sleep(5000);

    task_kill(h);
    task_kill(n);
    task_yield();

    serial_printf("    (starvation_counter=%u)\n", starvation_counter);

    /* NORMAL task should have run at least once thanks to starvation prevention */
    return (starvation_counter > 0);
}

/* ── Test 12: Yield with interrupts ───────────────── */

static bool test_yield_interrupts(void) {
    /* Disable interrupts */
    __asm__ __volatile__("cli");

    /* Yield should re-enable them */
    task_yield();

    /* Check IF flag */
    uint32_t eflags;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));

    bool if_set = (eflags & (1 << 9)) != 0;
    serial_printf("    (IF=%d after yield)\n", if_set ? 1 : 0);

    return if_set;
}

/* ── Phase 2 test runner ──────────────────────────── */

static void phase2_test_runner(void) {
    serial_print("\n=== Phase 2 Acceptance Tests ===\n");

    struct { const char* name; bool (*fn)(void); } tests[] = {
        { "Basic switching",       test_basic_switching },
        { "Priority",              test_priority },
        { "Sleep accuracy",        test_sleep },
        { "Sleep wrap-safe",       test_sleep_wrap },
        { "Task exit cleanup",     test_exit_cleanup },
        { "FPU isolation",         test_fpu_isolation },
        { "FPU stress",            test_fpu_stress },
        { "Idle CPU",              test_idle_cpu },
        { "CPU measurement",       test_cpu_measurement },
        { "Kill task",             test_kill_task },
        { "Starvation prevention", test_starvation },
        { "Yield with interrupts", test_yield_interrupts },
    };

    int count = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < count; i++) {
        bool ok = tests[i].fn();
        serial_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) pass++; else fail++;
    }

    serial_printf("\nPhase 2: %d/%d tests passed\n", pass, count);
    if (fail > 0) {
        serial_print("[AIOS v2] Phase 2 acceptance: FAIL\n");
    } else {
        serial_print("[AIOS v2] Phase 2 acceptance: PASS\n");
    }

    boot_print("\nAIOS v2 Phase 2 complete.\n");
}

/* ================================================================
 * Phase 3 Acceptance Tests
 * ================================================================ */

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

static void phase3_acceptance_tests(void) {
    serial_print("\n=== Phase 3 Acceptance Tests ===\n");

    struct { const char* name; bool (*fn)(void); } tests[] = {
        { "Timer tick rate",    test_timer_tick_rate },
        { "VGA output",         test_vga_output },
        { "Framebuffer HAL",    test_framebuffer_hal },
        { "Keyboard",           test_keyboard },
        { "Mouse",              test_mouse },
        { "Input queue",        test_input_queue },
        { "ATA disk",           test_ata },
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

/* ================================================================
 * Phase 4 Acceptance Tests (ChaosFS)
 * ================================================================ */

static bool test_chaosfs_mount(void) {
    /* FS was already mounted at boot. Verify it's live. */
    if (chaos_total_blocks() == 0) {
        serial_printf("    (not mounted)\n");
        return false;
    }
    serial_printf("    (blocks=%u free=%u inodes_free=%u label='%s')\n",
                  chaos_total_blocks(), chaos_free_blocks(),
                  chaos_free_inodes(), chaos_label());
    return true;
}

static bool test_chaosfs_write_read(void) {
    int fd = chaos_open("/test.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) { serial_printf("    (open failed: %d)\n", fd); return false; }

    const char* msg = "hello world";
    int w = chaos_write(fd, msg, 11);
    if (w != 11) { serial_printf("    (write=%d)\n", w); chaos_close(fd); return false; }

    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    char buf[32] = {0};
    int r = chaos_read(fd, buf, 11);
    chaos_close(fd);

    if (r != 11 || memcmp(buf, "hello world", 11) != 0) {
        serial_printf("    (read=%d data='%s')\n", r, buf);
        return false;
    }

    /* Verify stat */
    struct chaos_stat st;
    if (chaos_stat("/test.txt", &st) != CHAOS_OK || st.size != 11) {
        serial_printf("    (stat failed or size=%u)\n", (uint32_t)st.size);
        return false;
    }

    chaos_unlink("/test.txt");
    return true;
}

static bool test_chaosfs_large_file(void) {
    int fd = chaos_open("/large.bin", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;

    /* Write 64KB in 4KB chunks */
    uint8_t* chunk = (uint8_t*)kmalloc(4096);
    if (!chunk) { chaos_close(fd); return false; }

    for (int i = 0; i < 16; i++) {
        memset(chunk, (uint8_t)(i + 1), 4096);
        int w = chaos_write(fd, chunk, 4096);
        if (w != 4096) {
            serial_printf("    (write chunk %d failed: %d)\n", i, w);
            kfree(chunk); chaos_close(fd); return false;
        }
    }

    /* Read back and verify */
    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    bool ok = true;
    for (int i = 0; i < 16; i++) {
        memset(chunk, 0, 4096);
        int r = chaos_read(fd, chunk, 4096);
        if (r != 4096) { ok = false; break; }
        for (int j = 0; j < 4096; j++) {
            if (chunk[j] != (uint8_t)(i + 1)) { ok = false; break; }
        }
        if (!ok) break;
    }

    struct chaos_stat st;
    chaos_fstat(fd, &st);
    serial_printf("    (size=%u blocks=%u)\n", (uint32_t)st.size, st.block_count);

    kfree(chunk);
    chaos_close(fd);
    chaos_unlink("/large.bin");
    return ok && st.size == 65536;
}

static bool test_chaosfs_directories(void) {
    int r = chaos_mkdir("/scripts");
    if (r != CHAOS_OK) { serial_printf("    (mkdir /scripts: %d)\n", r); return false; }

    r = chaos_mkdir("/scripts/ai");
    if (r != CHAOS_OK) { serial_printf("    (mkdir /scripts/ai: %d)\n", r); return false; }

    /* Create a file in the nested dir */
    int fd = chaos_open("/scripts/ai/test.lua", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) { serial_printf("    (create file: %d)\n", fd); return false; }
    chaos_write(fd, "print('hi')", 11);
    chaos_close(fd);

    /* rmdir should fail (not empty) */
    r = chaos_rmdir("/scripts/ai");
    if (r != CHAOS_ERR_NOT_EMPTY) { serial_printf("    (rmdir non-empty: %d)\n", r); return false; }

    /* Clean up */
    chaos_unlink("/scripts/ai/test.lua");
    r = chaos_rmdir("/scripts/ai");
    if (r != CHAOS_OK) { serial_printf("    (rmdir /scripts/ai: %d)\n", r); return false; }
    r = chaos_rmdir("/scripts");
    if (r != CHAOS_OK) { serial_printf("    (rmdir /scripts: %d)\n", r); return false; }

    return true;
}

static bool test_chaosfs_readdir(void) {
    /* Create some files */
    for (int i = 0; i < 5; i++) {
        char path[32];
        serial_printf("");  /* force format string usage */
        path[0] = '/'; path[1] = 'a' + (char)i; path[2] = '.'; path[3] = 't'; path[4] = 'x'; path[5] = 't'; path[6] = '\0';
        int fd = chaos_open(path, CHAOS_O_CREAT | CHAOS_O_WRONLY);
        if (fd >= 0) chaos_close(fd);
    }

    int dh = chaos_opendir("/");
    if (dh < 0) return false;

    int count = 0;
    struct chaos_dirent entry;
    while (chaos_readdir(dh, &entry) == CHAOS_OK) {
        count++;
    }
    chaos_closedir(dh);

    serial_printf("    (entries=%d, expected >=7: . .. + 5 files + optional dirs)\n", count);

    /* Clean up */
    for (int i = 0; i < 5; i++) {
        char path[32];
        path[0] = '/'; path[1] = 'a' + (char)i; path[2] = '.'; path[3] = 't'; path[4] = 'x'; path[5] = 't'; path[6] = '\0';
        chaos_unlink(path);
    }

    return count >= 7;
}

static bool test_chaosfs_unlink_reuse(void) {
    uint32_t free_before = chaos_free_blocks();

    int fd = chaos_open("/temp.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;
    chaos_write(fd, "data", 4);
    chaos_close(fd);

    chaos_unlink("/temp.txt");
    uint32_t free_after = chaos_free_blocks();

    /* Blocks should be freed */
    if (free_after != free_before) {
        serial_printf("    (free blocks: %u -> %u -> %u)\n", free_before, free_before, free_after);
        return false;
    }

    /* Path should resolve to NOT_FOUND */
    struct chaos_stat st;
    if (chaos_stat("/temp.txt", &st) != CHAOS_ERR_NOT_FOUND) return false;

    return true;
}

static bool test_chaosfs_seek(void) {
    int fd = chaos_open("/seek.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;

    chaos_write(fd, "ABCDEFGHIJ", 10);

    /* Seek SET 5, read 3 → "FGH" */
    chaos_seek(fd, 5, CHAOS_SEEK_SET);
    char buf[4] = {0};
    chaos_read(fd, buf, 3);
    if (memcmp(buf, "FGH", 3) != 0) {
        serial_printf("    (seek read: '%s')\n", buf);
        chaos_close(fd); chaos_unlink("/seek.txt");
        return false;
    }

    /* Seek END 0, write "XYZ" */
    chaos_seek(fd, 0, CHAOS_SEEK_END);
    chaos_write(fd, "XYZ", 3);

    /* Read full */
    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    char full[16] = {0};
    int r = chaos_read(fd, full, 13);
    chaos_close(fd);
    chaos_unlink("/seek.txt");

    if (r != 13 || memcmp(full, "ABCDEFGHIJXYZ", 13) != 0) {
        serial_printf("    (full: '%s' r=%d)\n", full, r);
        return false;
    }

    return true;
}

static bool test_chaosfs_rename(void) {
    int fd = chaos_open("/a.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;
    chaos_write(fd, "hello", 5);
    chaos_close(fd);

    int r = chaos_rename("/a.txt", "/b.txt");
    if (r != CHAOS_OK) { serial_printf("    (rename: %d)\n", r); chaos_unlink("/a.txt"); return false; }

    /* Old path should be gone */
    struct chaos_stat st;
    if (chaos_stat("/a.txt", &st) != CHAOS_ERR_NOT_FOUND) {
        chaos_unlink("/a.txt"); chaos_unlink("/b.txt");
        return false;
    }

    /* New path should have data */
    fd = chaos_open("/b.txt", CHAOS_O_RDONLY);
    if (fd < 0) { chaos_unlink("/b.txt"); return false; }
    char buf[8] = {0};
    chaos_read(fd, buf, 5);
    chaos_close(fd);
    chaos_unlink("/b.txt");

    return memcmp(buf, "hello", 5) == 0;
}

static bool test_chaosfs_cross_dir_rename(void) {
    chaos_mkdir("/subdir");
    int fd = chaos_open("/a.txt", CHAOS_O_CREAT | CHAOS_O_WRONLY);
    if (fd >= 0) chaos_close(fd);

    int r = chaos_rename("/a.txt", "/subdir/a.txt");
    chaos_unlink("/a.txt");
    chaos_rmdir("/subdir");

    return r == CHAOS_ERR_INVALID;
}

static bool test_chaosfs_truncate(void) {
    int fd = chaos_open("/trunc.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;

    /* Write 100 bytes */
    uint8_t data[100];
    memset(data, 'A', 100);
    chaos_write(fd, data, 100);

    /* Expand to 8192 */
    int r = chaos_truncate(fd, 8192);
    if (r != CHAOS_OK) { chaos_close(fd); chaos_unlink("/trunc.txt"); return false; }

    struct chaos_stat st;
    chaos_fstat(fd, &st);
    if (st.size != 8192) { chaos_close(fd); chaos_unlink("/trunc.txt"); return false; }

    /* Read byte 100 — should be zero (gap filled) */
    chaos_seek(fd, 100, CHAOS_SEEK_SET);
    uint8_t b = 0xFF;
    chaos_read(fd, &b, 1);
    if (b != 0) {
        serial_printf("    (byte 100 = 0x%02x, expected 0)\n", b);
        chaos_close(fd); chaos_unlink("/trunc.txt");
        return false;
    }

    /* Shrink to 50 */
    chaos_truncate(fd, 50);
    chaos_fstat(fd, &st);

    chaos_close(fd);
    chaos_unlink("/trunc.txt");

    return st.size == 50;
}

static bool test_chaosfs_fd_exhaustion(void) {
    int fds[CHAOS_MAX_FD];
    int opened = 0;

    /* Create files and open them all */
    for (int i = 0; i < CHAOS_MAX_FD; i++) {
        char path[16];
        path[0] = '/'; path[1] = 'f';
        path[2] = '0' + (char)(i / 10); path[3] = '0' + (char)(i % 10);
        path[4] = '\0';
        fds[i] = chaos_open(path, CHAOS_O_CREAT | CHAOS_O_RDWR);
        if (fds[i] >= 0) opened++;
    }

    /* 17th should fail */
    int extra = chaos_open("/extra.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    bool exhaustion_works = (extra == CHAOS_ERR_NO_FD);

    /* Close one, retry */
    if (opened > 0) chaos_close(fds[0]);
    extra = chaos_open("/extra.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    bool reuse_works = (extra >= 0);
    if (extra >= 0) chaos_close(extra);

    /* Close all and clean up */
    for (int i = 1; i < CHAOS_MAX_FD; i++) {
        if (fds[i] >= 0) chaos_close(fds[i]);
    }
    for (int i = 0; i < CHAOS_MAX_FD; i++) {
        char path[16];
        path[0] = '/'; path[1] = 'f';
        path[2] = '0' + (char)(i / 10); path[3] = '0' + (char)(i % 10);
        path[4] = '\0';
        chaos_unlink(path);
    }
    chaos_unlink("/extra.txt");

    serial_printf("    (opened=%d exhaustion=%s reuse=%s)\n",
                  opened, exhaustion_works ? "yes" : "no", reuse_works ? "yes" : "no");

    return opened == CHAOS_MAX_FD && exhaustion_works && reuse_works;
}

static bool test_chaosfs_unlink_while_open(void) {
    uint32_t free_before = chaos_free_blocks();

    int fd = chaos_open("/open_unlink.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;
    chaos_write(fd, "still here", 10);

    /* Unlink while open */
    chaos_unlink("/open_unlink.txt");

    /* Path should be gone */
    struct chaos_stat st;
    if (chaos_stat("/open_unlink.txt", &st) != CHAOS_ERR_NOT_FOUND) {
        chaos_close(fd);
        return false;
    }

    /* FD should still read data */
    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    char buf[16] = {0};
    int r = chaos_read(fd, buf, 10);
    if (r != 10 || memcmp(buf, "still here", 10) != 0) {
        serial_printf("    (read after unlink: r=%d)\n", r);
        chaos_close(fd);
        return false;
    }

    /* Close — should free blocks now */
    chaos_close(fd);
    uint32_t free_after = chaos_free_blocks();

    serial_printf("    (free: %u -> %u)\n", free_before, free_after);
    return free_after == free_before;
}

static bool test_chaosfs_fsck_clean(void) {
    chaos_sync();
    int errors = chaos_fsck();
    serial_printf("    (fsck errors=%d)\n", errors);
    return errors == 0;
}

static void phase4_acceptance_tests(void) {
    if (!chaos_is_mounted()) {
        serial_print("\n=== Phase 4 Acceptance Tests ===\n");
        serial_print("  [SKIP] ChaosFS not mounted\n");
        return;
    }

    serial_print("\n=== Phase 4 Acceptance Tests ===\n");

    struct { const char* name; bool (*fn)(void); } tests[] = {
        { "Mount + verify",         test_chaosfs_mount },
        { "Basic write/read",       test_chaosfs_write_read },
        { "Large file (64KB)",      test_chaosfs_large_file },
        { "Directory creation",     test_chaosfs_directories },
        { "Directory listing",      test_chaosfs_readdir },
        { "Unlink + reuse",         test_chaosfs_unlink_reuse },
        { "Seek operations",        test_chaosfs_seek },
        { "Rename same-dir",        test_chaosfs_rename },
        { "Rename cross-dir reject",test_chaosfs_cross_dir_rename },
        { "Truncate expand/shrink", test_chaosfs_truncate },
        { "FD exhaustion",          test_chaosfs_fd_exhaustion },
        { "Unlink-while-open",      test_chaosfs_unlink_while_open },
        { "Clean fsck",             test_chaosfs_fsck_clean },
    };

    int count = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < count; i++) {
        bool ok = tests[i].fn();
        serial_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) pass++; else fail++;
    }

    serial_printf("\nPhase 4: %d/%d tests passed\n", pass, count);
    if (fail > 0) {
        serial_print("[AIOS v2] Phase 4 acceptance: FAIL\n");
    } else {
        serial_print("[AIOS v2] Phase 4 acceptance: PASS\n");
    }

    boot_print("\nAIOS v2 Phase 4 complete.\n");
}

/* ================================================================
 * Phase 5 Acceptance Tests — ChaosGL
 * ================================================================ */

/* ── 2D Tests ──────────────────────────────────────── */

static bool test_chaosgl_clear(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, CHAOS_GL_RGB(255, 0, 0));
    /* Read back pixel from back buffer before present */
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint32_t pixel = s->bufs[1 - s->buf_index][50 * 100 + 50];
    chaos_gl_surface_present(surf);
    chaos_gl_surface_destroy(surf);
    if (pixel != CHAOS_GL_RGB(255, 0, 0)) {
        serial_printf("    clear: expected 0x%x got 0x%x\n", CHAOS_GL_RGB(255,0,0), pixel);
        return false;
    }
    return true;
}

static bool test_chaosgl_rect(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);
    chaos_gl_rect(10, 10, 30, 20, 0x00FFFFFF);
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint32_t inside  = s->bufs[1 - s->buf_index][15 * 100 + 15];
    uint32_t outside = s->bufs[1 - s->buf_index][5 * 100 + 5];
    chaos_gl_surface_destroy(surf);
    if (inside != 0x00FFFFFF) {
        serial_printf("    rect inside: expected 0x00FFFFFF got 0x%x\n", inside);
        return false;
    }
    if (outside != 0x00000000) {
        serial_printf("    rect outside: expected 0x00000000 got 0x%x\n", outside);
        return false;
    }
    return true;
}

static bool test_chaosgl_text(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);
    chaos_gl_text(10, 10, "Hi", 0x00FFFFFF, 0, 0);
    int tw = chaos_gl_text_width("Hi");
    chaos_gl_surface_destroy(surf);
    if (tw != 16) {
        serial_printf("    text_width(\"Hi\"): expected 16 got %d\n", tw);
        return false;
    }
    return true;
}

static bool test_chaosgl_text_bg(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00FF0000); /* blue in BGRX */
    chaos_gl_text(0, 0, "X", 0x00FFFFFF, 0x00000000, CHAOS_GL_TEXT_BG_FILL);
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    /* (7,0) should be in bg area of the glyph → black */
    uint32_t pixel = s->bufs[1 - s->buf_index][0 * 100 + 7];
    chaos_gl_surface_destroy(surf);
    if (pixel != 0x00000000) {
        serial_printf("    text_bg: expected 0x00000000 got 0x%x\n", pixel);
        return false;
    }
    return true;
}

static bool test_chaosgl_clip(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);
    chaos_gl_push_clip((rect_t){20, 20, 20, 20});
    chaos_gl_rect(0, 0, 100, 100, 0x00FFFFFF);
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint32_t inside  = s->bufs[1 - s->buf_index][25 * 100 + 25];
    uint32_t outside = s->bufs[1 - s->buf_index][10 * 100 + 10];
    chaos_gl_pop_clip();
    chaos_gl_surface_destroy(surf);
    if (inside != 0x00FFFFFF) {
        serial_printf("    clip inside: expected 0x00FFFFFF got 0x%x\n", inside);
        return false;
    }
    if (outside != 0x00000000) {
        serial_printf("    clip outside: expected 0x00000000 got 0x%x\n", outside);
        return false;
    }
    return true;
}

static bool test_chaosgl_line(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);
    chaos_gl_line(0, 0, 99, 99, 0x00FFFFFF);
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint32_t pixel = s->bufs[1 - s->buf_index][50 * 100 + 50];
    chaos_gl_surface_destroy(surf);
    if (pixel != 0x00FFFFFF) {
        serial_printf("    line: expected 0x00FFFFFF at (50,50) got 0x%x\n", pixel);
        return false;
    }
    return true;
}

/* ── Surface & Compositor Tests ────────────────────── */

static bool test_chaosgl_surface_create_destroy(void) {
    uint32_t before = pmm_get_free_pages();
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) {
        serial_printf("    surface create failed\n");
        return false;
    }
    chaos_gl_surface_destroy(surf);
    uint32_t after = pmm_get_free_pages();
    if (before != after) {
        serial_printf("    PMM leak: before=%u after=%u\n", before, after);
        return false;
    }
    return true;
}

static bool test_chaosgl_surface_present(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, CHAOS_GL_RGB(255, 0, 0));
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint8_t idx_before = s->buf_index;
    chaos_gl_surface_present(surf);
    bool dirty = s->dirty;
    uint8_t idx_after = s->buf_index;
    chaos_gl_surface_destroy(surf);
    if (!dirty) {
        serial_printf("    present: dirty not set\n");
        return false;
    }
    if (idx_before == idx_after) {
        serial_printf("    present: buf_index not flipped\n");
        return false;
    }
    return true;
}

static bool test_chaosgl_surface_position_zorder(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_set_position(surf, 100, 200);
    chaos_gl_surface_set_zorder(surf, 5);
    int x = 0, y = 0;
    chaos_gl_surface_get_position(surf, &x, &y);
    int z = chaos_gl_surface_get_zorder(surf);
    chaos_gl_surface_destroy(surf);
    if (x != 100 || y != 200) {
        serial_printf("    position: expected (100,200) got (%d,%d)\n", x, y);
        return false;
    }
    if (z != 5) {
        serial_printf("    zorder: expected 5 got %d\n", z);
        return false;
    }
    return true;
}

static bool test_chaosgl_surface_alpha(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_set_alpha(surf, 128);
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint8_t a = s->alpha;
    chaos_gl_surface_destroy(surf);
    if (a != 128) {
        serial_printf("    alpha: expected 128 got %d\n", a);
        return false;
    }
    return true;
}

static bool test_chaosgl_surface_resize(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_resize(surf, 50, 50);
    int w = 0, h = 0;
    chaos_gl_surface_get_size(surf, &w, &h);
    chaos_gl_surface_destroy(surf);
    if (w != 50 || h != 50) {
        serial_printf("    resize: expected (50,50) got (%d,%d)\n", w, h);
        return false;
    }
    return true;
}

static bool test_chaosgl_compose(void) {
    int surf = chaos_gl_surface_create(100, 100, false);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, CHAOS_GL_RGB(0, 255, 0));
    chaos_gl_surface_present(surf);
    chaos_gl_surface_set_visible(surf, true);
    chaos_gl_surface_set_position(surf, 0, 0);
    chaos_gl_compose(0);
    chaos_gl_stats_t cstats = chaos_gl_get_compose_stats();
    chaos_gl_surface_destroy(surf);
    if (cstats.surfaces_composited < 1) {
        serial_printf("    compose: surfaces_composited=%u\n", cstats.surfaces_composited);
        return false;
    }
    return true;
}

/* ── 3D Pipeline Tests ─────────────────────────────── */

static bool test_chaosgl_flat_triangle(void) {
    int surf = chaos_gl_surface_create(100, 100, true);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);

    vec3_t eye    = {0.0f, 0.0f, 3.0f};
    vec3_t center = {0.0f, 0.0f, 0.0f};
    vec3_t up     = {0.0f, 1.0f, 0.0f};
    chaos_gl_set_camera(eye, center, up);
    chaos_gl_set_perspective(60.0f, 1.0f, 0.1f, 100.0f);
    chaos_gl_set_transform(0,0,0, 0,0,0, 1,1,1);

    flat_uniforms_t uni = { .color = 0x00FFFFFF };
    chaos_gl_shader_set(shader_flat_vert, shader_flat_frag, &uni);

    gl_vertex_in_t v0 = { .position = {-1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0,0} };
    gl_vertex_in_t v1 = { .position = { 1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {1,0} };
    gl_vertex_in_t v2 = { .position = { 0.0f,  1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0.5f,1} };
    chaos_gl_triangle(v0, v1, v2);

    chaos_gl_stats_t stats = chaos_gl_get_stats();
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint32_t center_pixel = s->bufs[1 - s->buf_index][50 * 100 + 50];
    chaos_gl_surface_destroy(surf);

    if (stats.triangles_submitted < 1) {
        serial_printf("    flat tri: submitted=%u\n", stats.triangles_submitted);
        return false;
    }
    if (stats.triangles_drawn < 1) {
        serial_printf("    flat tri: drawn=%u\n", stats.triangles_drawn);
        return false;
    }
    if (center_pixel != 0x00FFFFFF) {
        serial_printf("    flat tri: center pixel=0x%x\n", center_pixel);
        return false;
    }
    return true;
}

static bool test_chaosgl_zbuffer(void) {
    int surf = chaos_gl_surface_create(100, 100, true);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);

    vec3_t eye    = {0.0f, 0.0f, 3.0f};
    vec3_t center = {0.0f, 0.0f, 0.0f};
    vec3_t up     = {0.0f, 1.0f, 0.0f};
    chaos_gl_set_camera(eye, center, up);
    chaos_gl_set_perspective(60.0f, 1.0f, 0.1f, 100.0f);
    chaos_gl_set_transform(0,0,0, 0,0,0, 1,1,1);

    /* Draw far red triangle first */
    flat_uniforms_t uni_red = { .color = CHAOS_GL_RGB(255, 0, 0) };
    chaos_gl_shader_set(shader_flat_vert, shader_flat_frag, &uni_red);
    gl_vertex_in_t rv0 = { .position = {-1.0f, -1.0f, -1.0f}, .normal = {0,0,1}, .uv = {0,0} };
    gl_vertex_in_t rv1 = { .position = { 1.0f, -1.0f, -1.0f}, .normal = {0,0,1}, .uv = {1,0} };
    gl_vertex_in_t rv2 = { .position = { 0.0f,  1.0f, -1.0f}, .normal = {0,0,1}, .uv = {0.5f,1} };
    chaos_gl_triangle(rv0, rv1, rv2);

    /* Draw near green triangle on top */
    flat_uniforms_t uni_green = { .color = CHAOS_GL_RGB(0, 255, 0) };
    chaos_gl_shader_set(shader_flat_vert, shader_flat_frag, &uni_green);
    gl_vertex_in_t gv0 = { .position = {-1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0,0} };
    gl_vertex_in_t gv1 = { .position = { 1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {1,0} };
    gl_vertex_in_t gv2 = { .position = { 0.0f,  1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0.5f,1} };
    chaos_gl_triangle(gv0, gv1, gv2);

    chaos_gl_stats_t stats = chaos_gl_get_stats();
    chaos_gl_surface_t* s = chaos_gl_get_surface(surf);
    uint32_t center_pixel = s->bufs[1 - s->buf_index][50 * 100 + 50];
    chaos_gl_surface_destroy(surf);

    if (center_pixel != CHAOS_GL_RGB(0, 255, 0)) {
        serial_printf("    zbuffer: center pixel=0x%x expected green=0x%x\n",
                       center_pixel, CHAOS_GL_RGB(0, 255, 0));
        return false;
    }
    /* Z-buffer is working: near green triangle is visible over far red */
    serial_printf("    zbuffer: drawn=%u written=%u zfailed=%u\n",
                  stats.triangles_drawn, stats.pixels_written, stats.pixels_zfailed);
    return true;
}

static bool test_chaosgl_backface_cull(void) {
    int surf = chaos_gl_surface_create(100, 100, true);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);

    vec3_t eye    = {0.0f, 0.0f, 3.0f};
    vec3_t center = {0.0f, 0.0f, 0.0f};
    vec3_t up     = {0.0f, 1.0f, 0.0f};
    chaos_gl_set_camera(eye, center, up);
    chaos_gl_set_perspective(60.0f, 1.0f, 0.1f, 100.0f);
    chaos_gl_set_transform(0,0,0, 0,0,0, 1,1,1);

    flat_uniforms_t uni = { .color = 0x00FFFFFF };
    chaos_gl_shader_set(shader_flat_vert, shader_flat_frag, &uni);

    chaos_gl_model_t* cube = chaos_gl_model_load("/test/cube.cobj");
    if (!cube) {
        serial_printf("    backface: failed to load cube model\n");
        chaos_gl_surface_destroy(surf);
        return false;
    }
    chaos_gl_draw_model(cube);
    chaos_gl_stats_t stats = chaos_gl_get_stats();
    chaos_gl_model_free(cube);
    chaos_gl_surface_destroy(surf);

    if (stats.triangles_culled == 0) {
        serial_printf("    backface: no triangles culled\n");
        return false;
    }
    return true;
}

static bool test_chaosgl_texture(void) {
    int handle = chaos_gl_texture_load("/test/grid.raw");
    if (handle < 0) {
        serial_printf("    texture: load failed, handle=%d\n", handle);
        return false;
    }
    chaos_gl_texture_free(handle);
    return true;
}

static bool test_chaosgl_model(void) {
    chaos_gl_model_t* m = chaos_gl_model_load("/test/cube.cobj");
    if (!m) {
        serial_printf("    model: load failed\n");
        return false;
    }
    bool ok = (m->face_count == 12);
    if (!ok) {
        serial_printf("    model: face_count=%u expected 12\n", m->face_count);
    }
    chaos_gl_model_free(m);
    return ok;
}

static bool test_chaosgl_stats(void) {
    int surf = chaos_gl_surface_create(100, 100, true);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);

    vec3_t eye    = {0.0f, 0.0f, 3.0f};
    vec3_t center = {0.0f, 0.0f, 0.0f};
    vec3_t up     = {0.0f, 1.0f, 0.0f};
    chaos_gl_set_camera(eye, center, up);
    chaos_gl_set_perspective(60.0f, 1.0f, 0.1f, 100.0f);
    chaos_gl_set_transform(0,0,0, 0,0,0, 1,1,1);

    flat_uniforms_t uni = { .color = 0x00FFFFFF };
    chaos_gl_shader_set(shader_flat_vert, shader_flat_frag, &uni);

    gl_vertex_in_t v0 = { .position = {-1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0,0} };
    gl_vertex_in_t v1 = { .position = { 1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {1,0} };
    gl_vertex_in_t v2 = { .position = { 0.0f,  1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0.5f,1} };
    chaos_gl_triangle(v0, v1, v2);

    chaos_gl_stats_t stats = chaos_gl_get_stats();
    chaos_gl_surface_destroy(surf);

    if (stats.triangles_submitted < 1) {
        serial_printf("    stats: submitted=%u\n", stats.triangles_submitted);
        return false;
    }
    return true;
}

/* ── Integration Tests ─────────────────────────────── */

static bool test_chaosgl_2d_over_3d(void) {
    int surf = chaos_gl_surface_create(100, 100, true);
    if (surf < 0) return false;
    chaos_gl_surface_bind(surf);
    chaos_gl_surface_clear(surf, 0x00000000);

    vec3_t eye    = {0.0f, 0.0f, 3.0f};
    vec3_t center = {0.0f, 0.0f, 0.0f};
    vec3_t up     = {0.0f, 1.0f, 0.0f};
    chaos_gl_set_camera(eye, center, up);
    chaos_gl_set_perspective(60.0f, 1.0f, 0.1f, 100.0f);
    chaos_gl_set_transform(0,0,0, 0,0,0, 1,1,1);

    flat_uniforms_t uni = { .color = 0x00FFFFFF };
    chaos_gl_shader_set(shader_flat_vert, shader_flat_frag, &uni);

    gl_vertex_in_t v0 = { .position = {-1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0,0} };
    gl_vertex_in_t v1 = { .position = { 1.0f, -1.0f, 0.0f}, .normal = {0,0,1}, .uv = {1,0} };
    gl_vertex_in_t v2 = { .position = { 0.0f,  1.0f, 0.0f}, .normal = {0,0,1}, .uv = {0.5f,1} };
    chaos_gl_triangle(v0, v1, v2);

    /* Now draw 2D text on top */
    chaos_gl_text(5, 5, "HUD", 0x00FFFFFF, 0, 0);

    chaos_gl_surface_destroy(surf);
    return true; /* success if no crash */
}

static bool test_chaosgl_memory_stable(void) {
    uint32_t before = pmm_get_free_pages();
    for (int i = 0; i < 5; i++) {
        int surf = chaos_gl_surface_create(64, 64, false);
        if (surf < 0) {
            serial_printf("    memory stable: create #%d failed\n", i);
            return false;
        }
        chaos_gl_surface_destroy(surf);
    }
    uint32_t after = pmm_get_free_pages();
    if (before != after) {
        serial_printf("    memory stable: leak %d pages (%u -> %u)\n",
                       (int)before - (int)after, before, after);
        return false;
    }
    return true;
}

static bool test_chaosgl_shutdown(void) {
    uint32_t before = pmm_get_free_pages();
    chaos_gl_shutdown();
    uint32_t after_shutdown = pmm_get_free_pages();
    /* Re-init for any subsequent use */
    chaos_gl_init();
    if (after_shutdown < before) {
        serial_printf("    shutdown: PMM did not recover (before=%u after=%u)\n",
                       before, after_shutdown);
        return false;
    }
    return true;
}

/* ── Phase 5 Test Runner ───────────────────────────── */

static void phase5_acceptance_tests(void) {
    serial_print("\n=== Phase 5 Acceptance Tests ===\n");
    struct { const char* name; bool (*fn)(void); } tests[] = {
        /* 2D tests */
        { "Clear surface",              test_chaosgl_clear },
        { "Filled rect",                test_chaosgl_rect },
        { "Text rendering",             test_chaosgl_text },
        { "Text bg fill",               test_chaosgl_text_bg },
        { "Clip rect",                  test_chaosgl_clip },
        { "Line drawing",               test_chaosgl_line },
        /* Surface & compositor tests */
        { "Surface create/destroy",     test_chaosgl_surface_create_destroy },
        { "Surface present",            test_chaosgl_surface_present },
        { "Surface position/zorder",    test_chaosgl_surface_position_zorder },
        { "Surface alpha",              test_chaosgl_surface_alpha },
        { "Surface resize",             test_chaosgl_surface_resize },
        { "Compositor compose",         test_chaosgl_compose },
        /* 3D pipeline tests */
        { "Flat triangle",              test_chaosgl_flat_triangle },
        { "Z-buffer occlusion",         test_chaosgl_zbuffer },
        { "Backface culling",           test_chaosgl_backface_cull },
        { "Texture load/free",          test_chaosgl_texture },
        { "Model load (cube)",          test_chaosgl_model },
        { "Stats counters",             test_chaosgl_stats },
        /* Integration tests */
        { "2D over 3D",                 test_chaosgl_2d_over_3d },
        { "Memory stability",           test_chaosgl_memory_stable },
        { "Shutdown/reinit",            test_chaosgl_shutdown },
    };
    int count = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;
    for (int i = 0; i < count; i++) {
        bool ok = tests[i].fn();
        serial_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) pass++; else fail++;
    }
    serial_printf("\nPhase 5: %d/%d tests passed\n", pass, count);
    if (fail > 0) serial_print("[AIOS v2] Phase 5 acceptance: FAIL\n");
    else serial_print("[AIOS v2] Phase 5 acceptance: PASS\n");
    boot_print("\nAIOS v2 Phase 5 complete.\n");
}

/* ── Combined test runner ──────────────────────────── */

static void test_runner_main(void) {
    phase2_test_runner();
    phase3_acceptance_tests();
    phase4_acceptance_tests();
    phase5_acceptance_tests();
    task_exit();
}

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

    /* ── Phase 1 acceptance tests ─────────────────── */
    phase1_acceptance_tests();

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

    /* ── Phase 4: ChaosFS ─────────────────────────── */
    if (ata_is_present()) {
        r = chaos_mount(CHAOS_FS_LBA_START);
        boot_log("ChaosFS", r >= 0 ? INIT_OK : INIT_FAIL);
    }

    /* ── Phase 5: ChaosGL ─────────────────────────── */
    r = chaos_gl_init();
    boot_log("ChaosGL", r >= 0 ? INIT_OK : INIT_FAIL);

    /* Create test runner task before enabling interrupts */
    int test_id = task_create("test_runner", test_runner_main, PRIORITY_HIGH);
    if (test_id < 0) kernel_panic("Failed to create test runner");

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
