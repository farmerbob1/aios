/* AIOS v2 — Preemptive Priority Scheduler (Phase 2) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

#define MAX_TASKS       32
#define TASK_STACK_SIZE 16384

typedef enum {
    PRIORITY_HIGH   = 0,
    PRIORITY_NORMAL = 1,
    PRIORITY_LOW    = 2,
    PRIORITY_IDLE   = 3
} task_priority_t;

typedef enum {
    TASK_UNUSED  = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_EXITED
} task_state_t;

struct task {
    uint32_t esp;
    int      id;
    const char* name;
    task_state_t state;
    task_priority_t priority;
    uint32_t stack_base;
    uint32_t stack_size;
    uint8_t* fpu_state;
    bool     fpu_initialized;
    uint64_t sleep_until;
    uint32_t cpu_ticks;
    uint32_t total_ticks;
    bool     needs_cleanup;
    int      chaos_gl_surface_handle;  /* Phase 5: per-task surface binding, -1 = none */
    void*    lua_state;               /* Phase 7: lua_State* for Lua tasks, NULL otherwise */
    void*    userdata;                /* Phase 7: generic per-task data (e.g. lua_task_ctx) */
};

init_result_t scheduler_init(void);
void schedule(void);
int  task_create(const char* name, void (*entry)(void), task_priority_t priority);
void task_sleep(uint32_t ms);
void task_yield(void);
void task_exit(void);
int  task_kill(int id);
struct task* task_get_current(void);
struct task* task_get(int index);
int  task_get_count(void);
int  scheduler_get_cpu_usage(void);
bool scheduler_is_running(void);
