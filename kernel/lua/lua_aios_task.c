/* AIOS v2 — aios.task Lua Library (Phase 7)
 * Task spawn, self, yield, kill, cpu_usage. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../scheduler.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"

/* From lua_task.c */
extern int lua_task_create(const char *script_path, const char *task_name,
                           task_priority_t priority);
extern void lua_task_set_arg(int tid, const char *key, const char *value);

/* aios.task.spawn(path [, name [, args_table]]) → task_id or nil, err */
static int l_task_spawn(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *name = lua_isstring(L, 2) ? lua_tostring(L, 2) : path;
    int tid = lua_task_create(path, name, PRIORITY_NORMAL);
    if (tid < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "failed to create task");
        return 2;
    }
    /* Pass args table (arg 2 or 3) to the new task */
    int args_idx = 0;
    if (lua_istable(L, 3)) args_idx = 3;
    else if (lua_istable(L, 2)) args_idx = 2;
    if (args_idx) {
        lua_pushnil(L);
        while (lua_next(L, args_idx) != 0) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                lua_task_set_arg(tid, lua_tostring(L, -2), lua_tostring(L, -1));
            }
            lua_pop(L, 1);  /* pop value, keep key for next iteration */
        }
    }
    lua_pushinteger(L, (lua_Integer)tid);
    return 1;
}

/* aios.task.self() → {id, name, priority} */
static int l_task_self(lua_State *L) {
    struct task *t = task_get_current();
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)t->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, t->name ? t->name : "");
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)t->priority);
    lua_setfield(L, -2, "priority");
    return 1;
}

/* aios.task.yield() */
static int l_task_yield(lua_State *L) {
    (void)L;
    task_yield();
    return 0;
}

/* aios.task.kill(id) */
static int l_task_kill(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    int r = task_kill(id);
    lua_pushboolean(L, r == 0);
    return 1;
}

/* aios.task.cpu_usage() */
static int l_task_cpu(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)scheduler_get_cpu_usage());
    return 1;
}

static const luaL_Reg task_funcs[] = {
    {"spawn",     l_task_spawn},
    {"self",      l_task_self},
    {"yield",     l_task_yield},
    {"kill",      l_task_kill},
    {"cpu_usage", l_task_cpu},
    {NULL, NULL}
};

void aios_register_task(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, task_funcs, 0);
    lua_setfield(L, -2, "task");
    lua_pop(L, 1);
}
