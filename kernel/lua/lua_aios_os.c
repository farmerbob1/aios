/* AIOS v2 — aios.os Lua Library (Phase 7)
 * Timer, sleep, memory info, filesystem info. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../pmm.h"
#include "../scheduler.h"
#include "../chaos/chaos.h"
#include "../chaos/block_cache.h"
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
    lua_pushinteger(L, (lua_Integer)pmm_get_usable_ram_pages());
    lua_setfield(L, -2, "pmm_ram_pages");
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

/* aios.os.version() */
static int l_os_version(lua_State *L) {
    lua_pushstring(L, "2.0");
    return 1;
}

/* aios.os.cache_stats() */
static int l_os_cache_stats(lua_State *L) {
    cache_stats_t st = block_cache_get_stats();
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)st.hits);
    lua_setfield(L, -2, "hits");
    lua_pushinteger(L, (lua_Integer)st.misses);
    lua_setfield(L, -2, "misses");
    lua_pushinteger(L, (lua_Integer)st.evictions);
    lua_setfield(L, -2, "evictions");
    lua_pushinteger(L, (lua_Integer)st.write_throughs);
    lua_setfield(L, -2, "write_throughs");
    lua_pushinteger(L, (lua_Integer)block_cache_hit_rate());
    lua_setfield(L, -2, "hit_rate");
    return 1;
}

/* aios.os.cache_flush() */
static int l_os_cache_flush(lua_State *L) {
    (void)L;
    block_cache_flush();
    return 0;
}

static const luaL_Reg os_funcs[] = {
    {"ticks",        l_os_ticks},
    {"millis",       l_os_millis},
    {"frequency",    l_os_frequency},
    {"sleep",        l_os_sleep},
    {"exit",         l_os_exit},
    {"meminfo",      l_os_meminfo},
    {"fsinfo",       l_os_fsinfo},
    {"version",      l_os_version},
    {"cache_stats",  l_os_cache_stats},
    {"cache_flush",  l_os_cache_flush},
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
