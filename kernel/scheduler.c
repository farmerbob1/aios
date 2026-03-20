/* AIOS v2 — Preemptive Priority Scheduler (Phase 2) */

#include "scheduler.h"
#include "gdt.h"
#include "heap.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"

/* Assembly context switch */
extern void task_switch(uint32_t* old_esp, uint32_t new_esp);

/* ── State ─────────────────────────────────────────── */

static struct task tasks[MAX_TASKS];
static int current_task;
static int task_count;
static bool scheduler_enabled;
static int idle_task_id;

/* CPU accounting */
static uint32_t window_ticks;
static uint32_t idle_ticks;
#define ACCOUNTING_WINDOW 250  /* 1 second at 250Hz */

/* Starvation prevention */
static uint32_t last_ran[MAX_TASKS];
static task_priority_t original_priority[MAX_TASKS];
#define STARVATION_THRESHOLD 250  /* 1 second without running */

/* CPU usage last-completed-window snapshot */
static uint32_t last_window_idle;
static uint32_t last_window_total;

/* Round-robin tracking per priority */
static int rr_next[4];  /* one per priority level */

/* ── FPU save/restore ──────────────────────────────── */

static inline void fpu_save(uint8_t* buf) {
    __asm__ __volatile__("fxsave (%0)" : : "r"(buf) : "memory");
}

static inline void fpu_restore(uint8_t* buf) {
    __asm__ __volatile__("fxrstor (%0)" : : "r"(buf) : "memory");
}

/* ── Task exit wrapper ─────────────────────────────── */

static void task_exit_wrapper(void) {
    task_exit();
}

/* ── Task entry trampoline ─────────────────────────── */

/* New tasks start with IF=0 (schedule() cli's before task_switch).
 * This trampoline enables interrupts before jumping to the real entry.
 * Stack layout when this runs: [entry_addr] [task_exit_wrapper] */
__attribute__((noinline, used))
static void task_entry_trampoline(void) {
    __asm__ __volatile__("sti");
    /* ret will pop entry_addr from stack and jump to it */
}

/* ── Idle task ─────────────────────────────────────── */

static void idle_task_entry(void) {
    while (1) {
        __asm__ __volatile__("sti; hlt");
    }
}

/* ── Find next task ────────────────────────────────── */

static int find_next_task(void) {
    /* Priority scan: HIGH → NORMAL → LOW → IDLE */
    for (int p = PRIORITY_HIGH; p <= PRIORITY_IDLE; p++) {
        int start = rr_next[p];
        int i = start;
        do {
            if (tasks[i].state == TASK_READY &&
                (int)tasks[i].priority == p) {
                rr_next[p] = (i + 1) % MAX_TASKS;
                return i;
            }
            i = (i + 1) % MAX_TASKS;
        } while (i != start);
    }

    /* Should never happen — idle task is always ready */
    return idle_task_id;
}

/* ── Schedule ──────────────────────────────────────── */

__attribute__((noinline))
void schedule(void) {
    if (!scheduler_enabled) return;

    /* Disable interrupts to prevent re-entrant schedule() calls.
     * Timer IRQ can fire while we're in schedule() via task_yield(). */
    uint32_t saved_flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(saved_flags));

    /* 1. Wake sleeping tasks */
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].sleep_until) {
            tasks[i].state = TASK_READY;
        }
    }

    /* 2. Cleanup exited tasks (deferred free) */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_EXITED && tasks[i].needs_cleanup) {
            tasks[i].needs_cleanup = false;
            if (tasks[i].stack_base) {
                kfree((void*)tasks[i].stack_base);
                tasks[i].stack_base = 0;
            }
            if (tasks[i].fpu_state) {
                kfree_aligned(tasks[i].fpu_state);
                tasks[i].fpu_state = NULL;
            }
            tasks[i].state = TASK_UNUSED;
            task_count--;
        }
    }

    /* 3. CPU accounting */
    window_ticks++;
    if (current_task == idle_task_id) {
        idle_ticks++;
    }
    if (window_ticks >= ACCOUNTING_WINDOW) {
        last_window_idle = idle_ticks;
        last_window_total = window_ticks;
        window_ticks = 0;
        idle_ticks = 0;
    }

    /* 4. Starvation prevention */
    last_ran[current_task] = (uint32_t)now;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_READY &&
            (original_priority[i] == PRIORITY_NORMAL ||
             original_priority[i] == PRIORITY_LOW)) {
            if ((uint32_t)now - last_ran[i] >= STARVATION_THRESHOLD) {
                tasks[i].priority = PRIORITY_HIGH;
            }
        }
    }

    /* Mark current as ready (unless exited/sleeping/blocked) */
    if (tasks[current_task].state == TASK_RUNNING) {
        tasks[current_task].state = TASK_READY;
    }

    /* 5. Find next task */
    int next = find_next_task();

    /* Restore all boosted priorities */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].priority != original_priority[i] &&
            tasks[i].state != TASK_UNUSED) {
            tasks[i].priority = original_priority[i];
        }
    }

    /* Track ticks */
    tasks[next].cpu_ticks++;
    tasks[next].total_ticks++;
    tasks[next].state = TASK_RUNNING;

    if (next == current_task) {
        __asm__ __volatile__("pushl %0; popfl" : : "r"(saved_flags));
        return;
    }

    /* 6. FPU save/restore */
    int old = current_task;
    if (tasks[old].fpu_state) {
        fpu_save(tasks[old].fpu_state);
    }

    current_task = next;

    if (tasks[next].fpu_state) {
        if (tasks[next].fpu_initialized) {
            fpu_restore(tasks[next].fpu_state);
        } else {
            __asm__ __volatile__("fninit");
            tasks[next].fpu_initialized = true;
        }
    }

    /* 7. Update TSS esp0 */
    gdt_set_kernel_stack(tasks[next].stack_base + tasks[next].stack_size);

    /* 8. Context switch */
    task_switch(&tasks[old].esp, tasks[next].esp);

    /* Compiler barrier: prevents tail-call optimization of task_switch.
     * When old task resumes, execution continues here. */
    __asm__ __volatile__("" ::: "memory");

    /* Restore interrupt state (when this task resumes) */
    __asm__ __volatile__("pushl %0; popfl" : : "r"(saved_flags));
}

/* ── Scheduler init ────────────────────────────────── */

/* Static FPU buffer for kernel task (16-byte aligned) */
static uint8_t kernel_fpu_buf[512] __attribute__((aligned(16)));

init_result_t scheduler_init(void) {
    memset(tasks, 0, sizeof(tasks));
    memset(last_ran, 0, sizeof(last_ran));
    memset(rr_next, 0, sizeof(rr_next));
    memset(original_priority, 0, sizeof(original_priority));

    /* Task 0: kernel (currently running, uses boot stack at ~0x90000) */
    tasks[0].id       = 0;
    tasks[0].name     = "kernel";
    tasks[0].state    = TASK_RUNNING;
    tasks[0].priority = PRIORITY_NORMAL;
    tasks[0].stack_base = 0x80000;  /* boot stack base (approximate) */
    tasks[0].stack_size = 0x10000;  /* 64KB boot stack */
    tasks[0].fpu_state = kernel_fpu_buf;
    tasks[0].fpu_initialized = true;
    tasks[0].chaos_gl_surface_handle = -1;
    original_priority[0] = PRIORITY_NORMAL;
    /* esp will be saved on first task_switch */

    current_task = 0;
    task_count = 1;

    /* Create idle task */
    idle_task_id = task_create("idle", idle_task_entry, PRIORITY_IDLE);
    if (idle_task_id < 0) return INIT_FAIL;

    scheduler_enabled = true;
    return INIT_OK;
}

/* ── Task creation ─────────────────────────────────── */

int task_create(const char* name, void (*entry)(void), task_priority_t priority) {
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    /* Allocate stack */
    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) return -1;

    /* Allocate 16-byte aligned FPU state */
    uint8_t* fpu = (uint8_t*)kmalloc_aligned(512, 16);
    if (!fpu) {
        kfree(stack);
        return -1;
    }
    memset(fpu, 0, 512);

    /* Set up initial stack frame for task_switch */
    uint32_t stack_top = (uint32_t)stack + TASK_STACK_SIZE;
    stack_top &= ~0xF;  /* 16-byte align */

    uint32_t* sp = (uint32_t*)stack_top;

    /* Return address when entry() returns → task_exit_wrapper */
    *(--sp) = (uint32_t)task_exit_wrapper;
    /* Real entry function — trampoline's ret pops this */
    *(--sp) = (uint32_t)entry;
    /* "Saved EIP" — task_switch's ret jumps to trampoline (enables IF) */
    *(--sp) = (uint32_t)task_entry_trampoline;
    /* Saved EBP */
    *(--sp) = 0;
    /* Saved EBX */
    *(--sp) = 0;
    /* Saved ESI */
    *(--sp) = 0;
    /* Saved EDI */
    *(--sp) = 0;

    tasks[slot].id       = slot;
    tasks[slot].name     = name;
    tasks[slot].state    = TASK_READY;
    tasks[slot].priority = priority;
    tasks[slot].stack_base = (uint32_t)stack;
    tasks[slot].stack_size = TASK_STACK_SIZE;
    tasks[slot].esp      = (uint32_t)sp;
    tasks[slot].fpu_state = fpu;
    tasks[slot].fpu_initialized = false;
    tasks[slot].sleep_until = 0;
    tasks[slot].cpu_ticks = 0;
    tasks[slot].total_ticks = 0;
    tasks[slot].needs_cleanup = false;
    tasks[slot].chaos_gl_surface_handle = -1;
    original_priority[slot] = priority;

    task_count++;
    return slot;
}

/* ── Task operations ───────────────────────────────── */

void task_sleep(uint32_t ms) {
    uint64_t ticks_to_sleep = ((uint64_t)ms * PIT_FREQUENCY) / 1000;
    if (ticks_to_sleep == 0) ticks_to_sleep = 1;

    tasks[current_task].sleep_until = timer_get_ticks() + ticks_to_sleep;
    tasks[current_task].state = TASK_SLEEPING;
    task_yield();
}

void task_yield(void) {
    /* Ensure interrupts are enabled after yield returns */
    __asm__ __volatile__("sti");
    schedule();
}

void task_exit(void) {
    tasks[current_task].state = TASK_EXITED;
    tasks[current_task].needs_cleanup = true;
    while (1) {
        task_yield();
    }
}

int task_kill(int id) {
    if (id < 0 || id >= MAX_TASKS) return -1;
    if (id == 0) return -1;               /* can't kill kernel */
    if (id == idle_task_id) return -1;     /* can't kill idle */
    if (tasks[id].state == TASK_UNUSED) return -1;

    tasks[id].state = TASK_EXITED;
    tasks[id].needs_cleanup = true;
    return 0;
}

struct task* task_get_current(void) {
    return &tasks[current_task];
}

struct task* task_get(int index) {
    if (index < 0 || index >= MAX_TASKS) return NULL;
    if (tasks[index].state == TASK_UNUSED) return NULL;
    return &tasks[index];
}

int task_get_count(void) {
    return task_count;
}

int scheduler_get_cpu_usage(void) {
    /* Snapshot current window state atomically */
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags));
    uint32_t w = window_ticks;
    uint32_t i = idle_ticks;
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags));

    if (w < 50) {
        /* Window too small, use last completed window */
        w = last_window_total;
        i = last_window_idle;
    }
    if (w == 0) return 0;
    return (int)(((w - i) * 100) / w);
}
