/* AIOS v2 — Lua Runtime Initialization (Phase 7)
 * Called during boot between kaos_init() and the test runner. */

#include "../../include/types.h"
#include "../../include/boot_info.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"

/* From lua_kaos.c */
extern void lua_kaos_init(void);

/* From lua_state.c */
extern lua_State *lua_state_create_minimal(void);
extern void lua_state_destroy(lua_State *L);

init_result_t lua_init(void) {
    /* 1. Zero the KAOS binding table */
    lua_kaos_init();

    /* 2. Create a test lua_State to verify porting is correct */
    lua_State *L = lua_state_create_minimal();
    if (!L) {
        serial_printf("[lua] Failed to create test state\n");
        return INIT_FAIL;
    }

    /* 3. Run a trivial Lua expression to verify the VM works */
    int err = luaL_dostring(L, "return 2 + 2");
    if (err || !lua_isinteger(L, -1) || lua_tointeger(L, -1) != 4) {
        const char *msg = lua_tostring(L, -1);
        serial_printf("[lua] VM self-test failed: %s\n", msg ? msg : "(unknown)");
        lua_state_destroy(L);
        return INIT_FAIL;
    }

    /* 4. Clean up test state */
    lua_state_destroy(L);

    serial_printf("[lua] Lua 5.5 runtime ready\n");
    return INIT_OK;
}
