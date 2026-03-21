/* AIOS v2 — aios.debug Lua Library (Phase 7)
 * Serial debug output. */

#include "../../include/types.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"

/* aios.debug.print(str) */
static int l_debug_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    serial_print(s);
    return 0;
}

/* aios.debug.printf(fmt, ...) */
static int l_debug_printf(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 1) return 0;

    /* Use Lua's string.format for formatting */
    lua_getglobal(L, "string");
    lua_getfield(L, -1, "format");
    lua_remove(L, -2);  /* remove string table */

    /* Push all arguments */
    for (int i = 1; i <= n; i++)
        lua_pushvalue(L, i);

    lua_call(L, n, 1);
    const char *s = lua_tostring(L, -1);
    if (s) serial_print(s);
    return 0;
}

static const luaL_Reg debug_funcs[] = {
    {"print",  l_debug_print},
    {"printf", l_debug_printf},
    {NULL, NULL}
};

void aios_register_debug(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, debug_funcs, 0);
    lua_setfield(L, -2, "debug");
    lua_pop(L, 1);
}
