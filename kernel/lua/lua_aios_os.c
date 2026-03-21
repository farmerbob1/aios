/* AIOS v2 — aios.os Lua Library (Phase 7)
 * Timer, sleep, memory info, filesystem info. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../pmm.h"
#include "../scheduler.h"
#include "../chaos/chaos.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"

#include "lua.h"
#include "lauxlib.h"

/* aios.os.ticks() */
static int l_os_ticks(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)timer_get_ticks());
    return 1;
}

/* aios.os.millis() */
static int l_os_millis(lua_State *L) {
    uint64_t ticks = timer_get_ticks();
    uint32_t freq = timer_get_frequency();
    uint64_t ms = (ticks * 1000) / freq;
    lua_pushinteger(L, (lua_Integer)ms);
    return 1;
}

/* aios.os.frequency() */
static int l_os_frequency(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)timer_get_frequency());
    return 1;
}

/* aios.os.sleep(ms) */
static int l_os_sleep(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    if (ms > 0) task_sleep((uint32_t)ms);
    return 0;
}

/* aios.os.exit() */
static int l_os_exit(lua_State *L) {
    (void)L;
    task_exit();
    return 0;  /* unreachable */
}

/* aios.os.meminfo() */
static int l_os_meminfo(lua_State *L) {
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)heap_get_free());
    lua_setfield(L, -2, "heap_free");
    lua_pushinteger(L, (lua_Integer)heap_get_used());
    lua_setfield(L, -2, "heap_used");
    lua_pushinteger(L, (lua_Integer)pmm_get_free_pages());
    lua_setfield(L, -2, "pmm_free_pages");
    lua_pushinteger(L, (lua_Integer)pmm_get_total_pages());
    lua_setfield(L, -2, "pmm_total_pages");
    return 1;
}

/* aios.os.fsinfo() */
static int l_os_fsinfo(lua_State *L) {
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)chaos_free_blocks());
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)chaos_total_blocks());
    lua_setfield(L, -2, "total_blocks");
    lua_pushinteger(L, (lua_Integer)chaos_free_inodes());
    lua_setfield(L, -2, "free_inodes");
    const char *label = chaos_label();
    lua_pushstring(L, label ? label : "");
    lua_setfield(L, -2, "label");
    return 1;
}

static const luaL_Reg os_funcs[] = {
    {"ticks",     l_os_ticks},
    {"millis",    l_os_millis},
    {"frequency", l_os_frequency},
    {"sleep",     l_os_sleep},
    {"exit",      l_os_exit},
    {"meminfo",   l_os_meminfo},
    {"fsinfo",    l_os_fsinfo},
    {NULL, NULL}
};

void aios_register_os(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, os_funcs, 0);
    lua_setfield(L, -2, "os");
    lua_pop(L, 1);
}
