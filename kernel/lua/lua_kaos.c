/* AIOS v2 — KAOS Module → Lua Binding Bridge (Phase 7)
 * Allows KAOS modules to register Lua functions via kaos_lua_register(). */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../include/kaos/export.h"

/* Forward declare lua_CFunction type to avoid including Lua headers
 * in this file which is compiled with kernel CFLAGS */
typedef int (*lua_CFunction_t)(void *);

#define LUA_MAX_KAOS_BINDINGS 128

struct lua_kaos_binding {
    const char    *table_name;
    const char    *func_name;
    lua_CFunction_t func;
    bool           active;
};

static struct lua_kaos_binding kaos_bindings[LUA_MAX_KAOS_BINDINGS];
static int kaos_binding_count = 0;

void lua_kaos_init(void) {
    memset(kaos_bindings, 0, sizeof(kaos_bindings));
    kaos_binding_count = 0;
}

int kaos_lua_register(const char *table_name, const char *func_name,
                      lua_CFunction_t func) {
    if (kaos_binding_count >= LUA_MAX_KAOS_BINDINGS) return -1;
    struct lua_kaos_binding *b = &kaos_bindings[kaos_binding_count++];
    b->table_name = table_name;
    b->func_name  = func_name;
    b->func       = func;
    b->active     = true;
    serial_printf("[lua] KAOS binding registered: %s.%s\n",
                  table_name ? table_name : "(global)", func_name);
    return 0;
}

int kaos_lua_unregister(const char *table_name, const char *func_name) {
    for (int i = 0; i < kaos_binding_count; i++) {
        struct lua_kaos_binding *b = &kaos_bindings[i];
        if (!b->active) continue;
        bool table_match = (table_name == NULL && b->table_name == NULL) ||
                           (table_name && b->table_name && strcmp(table_name, b->table_name) == 0);
        if (table_match && strcmp(func_name, b->func_name) == 0) {
            b->active = false;
            return 0;
        }
    }
    return -1;
}

/* Called by lua_state.c during state creation */
int lua_kaos_get_binding_count(void) { return kaos_binding_count; }

struct lua_kaos_binding *lua_kaos_get_binding(int index) {
    if (index < 0 || index >= kaos_binding_count) return NULL;
    return &kaos_bindings[index];
}

KAOS_EXPORT(kaos_lua_register)
KAOS_EXPORT(kaos_lua_unregister)
