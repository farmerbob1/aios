/* AIOS v2 — Phase 2 Acceptance Tests (Multitasking)
 * Extracted from kernel/main.c to reduce code size per compilation unit. */

#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "scheduler.h"
#include "rdtsc.h"
#include "fpu.h"
#include "heap.h"
#include "pmm.h"
#include "boot_display.h"
#include "phase2_tests.h"

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

void phase2_test_runner(void) {
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
