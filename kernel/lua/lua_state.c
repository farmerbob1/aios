/* AIOS v2 — Lua State Management (Phase 7)
 * Creates/destroys fully configured lua_States with AIOS libraries. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* External functions from other lua/ files */
extern void *lua_aios_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
extern void *lua_aios_alloc_tracked(void *ud, void *ptr, size_t osize, size_t nsize);
extern void aios_install_loader(lua_State *L);
extern void aios_register_io(lua_State *L);
extern void aios_register_os(lua_State *L);
extern void aios_register_input(lua_State *L);
extern void aios_register_task(lua_State *L);
extern void aios_register_debug(lua_State *L);
extern void aios_register_chaosgl(lua_State *L);

/* From lua_kaos.c */
struct lua_kaos_binding {
    const char *table_name;
    const char *func_name;
    int (*func)(void *);
    bool active;
};
extern int lua_kaos_get_binding_count(void);
extern struct lua_kaos_binding *lua_kaos_get_binding(int index);

/* Per-state memory stats */
struct lua_mem_stats {
    size_t current_bytes;
    size_t peak_bytes;
    size_t total_allocs;
    size_t total_frees;
    size_t limit_bytes;
};

#define LUA_DEFAULT_MEM_LIMIT (8 * 1024 * 1024)  /* 8MB */

/* Panic handler */
static int lua_aios_panic(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    serial_printf("[lua] PANIC: %s\n", msg ? msg : "(no message)");
    return 0;
}

/* print() override — serial output */
static int aios_print_serial(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) serial_print("\t");
        serial_print(s ? s : "nil");
        lua_pop(L, 1);  /* pop tolstring result */
    }
    serial_print("\n");
    return 0;
}

/* CHAOS_GL_RGB(r, g, b) → BGRX uint32 */
static int aios_chaos_gl_rgb(lua_State *L) {
    int r = (int)luaL_checkinteger(L, 1);
    int g = (int)luaL_checkinteger(L, 2);
    int b = (int)luaL_checkinteger(L, 3);
    uint32_t color = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
    lua_pushinteger(L, (lua_Integer)color);
    return 1;
}

/* Register KAOS module bindings into a lua_State */
static void aios_register_kaos_bindings(lua_State *L) {
    int count = lua_kaos_get_binding_count();
    for (int i = 0; i < count; i++) {
        struct lua_kaos_binding *b = lua_kaos_get_binding(i);
        if (!b || !b->active) continue;

        if (b->table_name) {
            /* Register into a named table */
            lua_getglobal(L, b->table_name);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushvalue(L, -1);
                lua_setglobal(L, b->table_name);
            }
            lua_pushcfunction(L, (lua_CFunction)b->func);
            lua_setfield(L, -2, b->func_name);
            lua_pop(L, 1);
        } else {
            /* Register as global function */
            lua_pushcfunction(L, (lua_CFunction)b->func);
            lua_setglobal(L, b->func_name);
        }
    }
}

/* Create a new fully-configured lua_State */
lua_State *lua_state_create(void) {
    /* Allocate memory stats for tracked allocator */
    struct lua_mem_stats *stats = kmalloc(sizeof(struct lua_mem_stats));
    if (!stats) return NULL;
    memset(stats, 0, sizeof(*stats));
    stats->limit_bytes = LUA_DEFAULT_MEM_LIMIT;

    lua_State *L = lua_newstate(lua_aios_alloc_tracked, stats, 0);
    if (!L) {
        kfree(stats);
        return NULL;
    }

    /* Set panic handler */
    lua_atpanic(L, lua_aios_panic);

    /* Open selected standard libraries using Lua 5.5's luaL_openselectedlibs.
     * Include: base, coroutine, math, string, table, utf8, package (for require).
     * Exclude: debug, io, os (replaced with AIOS versions). */
    int libs = LUA_GLIBK | LUA_LOADLIBK | LUA_COLIBK | LUA_MATHLIBK |
               LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK;
    luaL_openselectedlibs(L, libs, 0);

    /* Install ChaosFS-backed require/dofile/loadfile */
    aios_install_loader(L);

    /* Register AIOS libraries */
    aios_register_io(L);
    aios_register_os(L);
    aios_register_input(L);
    aios_register_task(L);
    aios_register_debug(L);
    aios_register_chaosgl(L);

    /* Register event type constants */
    lua_pushinteger(L, 1); lua_setglobal(L, "EVENT_MOUSE_MOVE");
    lua_pushinteger(L, 2); lua_setglobal(L, "EVENT_MOUSE_DOWN");
    lua_pushinteger(L, 3); lua_setglobal(L, "EVENT_MOUSE_UP");
    lua_pushinteger(L, 4); lua_setglobal(L, "EVENT_MOUSE_WHEEL");
    lua_pushinteger(L, 5); lua_setglobal(L, "EVENT_KEY_DOWN");
    lua_pushinteger(L, 6); lua_setglobal(L, "EVENT_KEY_UP");
    lua_pushinteger(L, 7); lua_setglobal(L, "EVENT_KEY_CHAR");

    /* Register KAOS module bindings */
    aios_register_kaos_bindings(L);

    /* Override print() to serial */
    lua_pushcfunction(L, aios_print_serial);
    lua_setglobal(L, "print");

    /* CHAOS_GL_RGB helper */
    lua_pushcfunction(L, aios_chaos_gl_rgb);
    lua_setglobal(L, "CHAOS_GL_RGB");

    return L;
}

/* Create a minimal state (for testing — no AIOS libs, just core Lua) */
lua_State *lua_state_create_minimal(void) {
    lua_State *L = lua_newstate(lua_aios_alloc, NULL, 0);
    if (!L) return NULL;
    lua_atpanic(L, lua_aios_panic);
    int libs = LUA_GLIBK | LUA_COLIBK | LUA_MATHLIBK |
               LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK;
    luaL_openselectedlibs(L, libs, 0);
    lua_pushcfunction(L, aios_print_serial);
    lua_setglobal(L, "print");
    return L;
}

/* Destroy a lua_State and free associated memory */
void lua_state_destroy(lua_State *L) {
    if (!L) return;
    /* Get the allocator userdata (our mem_stats) before closing */
    void *ud;
    lua_getallocf(L, &ud);
    lua_close(L);
    /* Free the stats struct if it was tracked allocation */
    if (ud) kfree(ud);
}

/* Get memory stats for a state */
struct lua_mem_stats *lua_state_get_memstats(lua_State *L) {
    void *ud;
    lua_getallocf(L, &ud);
    return (struct lua_mem_stats *)ud;
}
