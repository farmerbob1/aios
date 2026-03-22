/* AIOS v2 — Lua Task Management (Phase 7)
 * Creates scheduler tasks that run Lua scripts from ChaosFS. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../scheduler.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"

/* External from lua_state.c */
extern lua_State *lua_state_create(void);
extern void lua_state_destroy(lua_State *L);

/* External from lua_loader.c */
extern int aios_dofile_internal(lua_State *L, const char *path);

/* Per-task Lua context */
#define LUA_TASK_MAX_ARGS  8
#define LUA_TASK_ARG_LEN   256

struct lua_task_arg {
    char key[64];
    char value[LUA_TASK_ARG_LEN];
};

struct lua_task_ctx {
    char script_path[256];
    lua_State *L;
    int argc;
    struct lua_task_arg args[LUA_TASK_MAX_ARGS];
};

/* Entry point for Lua tasks */
static void lua_task_entry_wrapper(void) {
    struct task *self = task_get_current();
    struct lua_task_ctx *ctx = (struct lua_task_ctx *)self->userdata;

    if (!ctx) {
        serial_printf("[lua] Task %d has no context\n", self->id);
        task_exit();
        return;
    }

    /* Create isolated Lua state */
    lua_State *L = lua_state_create();
    if (!L) {
        serial_printf("[lua] Failed to create state for %s\n", ctx->script_path);
        kfree(ctx);
        self->userdata = NULL;
        task_exit();
        return;
    }
    ctx->L = L;
    self->lua_state = L;

    /* Set global 'arg' table from spawn arguments */
    if (ctx->argc > 0) {
        lua_newtable(L);
        for (int i = 0; i < ctx->argc; i++) {
            lua_pushstring(L, ctx->args[i].value);
            lua_setfield(L, -2, ctx->args[i].key);
        }
        lua_setglobal(L, "arg");
    }

    /* Load and run the script */
    int err = aios_dofile_internal(L, ctx->script_path);
    if (err) {
        const char *msg = lua_tostring(L, -1);
        serial_printf("[lua] Error in %s: %s\n", ctx->script_path,
                      msg ? msg : "(unknown)");
    }

    /* Cleanup */
    self->lua_state = NULL;
    lua_state_destroy(L);
    kfree(ctx);
    self->userdata = NULL;
    task_exit();
}

/* Spawn a new Lua task from a script path */
int lua_task_create(const char *script_path, const char *task_name,
                    task_priority_t priority) {
    struct lua_task_ctx *ctx = kmalloc(sizeof(struct lua_task_ctx));
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->script_path, script_path, sizeof(ctx->script_path) - 1);

    int tid = task_create(task_name, lua_task_entry_wrapper, priority);
    if (tid < 0) {
        kfree(ctx);
        return -1;
    }

    task_get(tid)->userdata = ctx;
    return tid;
}

void lua_task_set_arg(int tid, const char *key, const char *value) {
    struct task *t = task_get(tid);
    if (!t || !t->userdata) return;
    struct lua_task_ctx *ctx = (struct lua_task_ctx *)t->userdata;
    if (ctx->argc >= LUA_TASK_MAX_ARGS) return;
    strncpy(ctx->args[ctx->argc].key, key, 63);
    ctx->args[ctx->argc].key[63] = '\0';
    strncpy(ctx->args[ctx->argc].value, value, LUA_TASK_ARG_LEN - 1);
    ctx->args[ctx->argc].value[LUA_TASK_ARG_LEN - 1] = '\0';
    ctx->argc++;
}
