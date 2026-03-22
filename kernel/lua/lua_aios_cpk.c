/* AIOS — Lua CPK Archive Bindings
 *
 * Provides aios.cpk.* API for Lua scripts:
 *   aios.cpk.open(path)            → handle or nil, err
 *   aios.cpk.list(handle)          → [{path, size, compressed_size}, ...]
 *   aios.cpk.read(handle, path)    → data string or nil, err
 *   aios.cpk.close(handle)
 *   aios.cpk.install(cpk_path, install_dir) → file_count or nil, err
 */

#include "../compression/cpk.h"
#include "../compression/lz4.h"
#include "../chaos/chaos.h"
#include "../heap.h"
#include "../../include/types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"

static int l_cpk_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int h = cpk_open(path);
    if (h < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to open cpk archive");
        return 2;
    }
    lua_pushinteger(L, h);
    return 1;
}

static int l_cpk_list(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    int count = cpk_file_count(h);
    if (count < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid handle");
        return 2;
    }

    lua_createtable(L, count, 0);
    struct cpk_entry e;
    for (int i = 0; i < count; i++) {
        if (cpk_get_entry(h, i, &e) < 0) continue;
        lua_createtable(L, 0, 3);
        lua_pushstring(L, e.path);
        lua_setfield(L, -2, "path");
        lua_pushinteger(L, e.size_original);
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, e.size_compressed);
        lua_setfield(L, -2, "compressed_size");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_cpk_read(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    const char *path = luaL_checkstring(L, 2);

    int idx = cpk_find(h, path);
    if (idx < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "file not found in archive: %s", path);
        return 2;
    }

    struct cpk_entry e;
    if (cpk_get_entry(h, idx, &e) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to get entry");
        return 2;
    }

    void *buf = kmalloc(e.size_original);
    if (!buf) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    int n = cpk_extract(h, idx, buf, e.size_original);
    if (n < 0) {
        kfree(buf);
        lua_pushnil(L);
        lua_pushstring(L, "extraction failed");
        return 2;
    }

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    kfree(buf);
    return 1;
}

static int l_cpk_close(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    cpk_close(h);
    return 0;
}

/* Helper: create all parent directories for a path */
static void ensure_parent_dirs(const char *full_path) {
    char tmp[256];
    size_t len = strlen(full_path);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, full_path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            chaos_mkdir(tmp);  /* ignore errors — may already exist */
            tmp[i] = '/';
        }
    }
}

static int l_cpk_install(lua_State *L) {
    const char *cpk_path = luaL_checkstring(L, 1);
    const char *install_dir = luaL_checkstring(L, 2);

    int h = cpk_open(cpk_path);
    if (h < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to open cpk archive");
        return 2;
    }

    int count = cpk_file_count(h);
    int installed = 0;

    for (int i = 0; i < count; i++) {
        struct cpk_entry e;
        if (cpk_get_entry(h, i, &e) < 0) continue;

        /* Build full output path: install_dir/entry.path */
        char out_path[256];
        int plen = strlen(install_dir);
        int elen = strlen(e.path);
        if (plen + 1 + elen >= (int)sizeof(out_path)) continue;

        memcpy(out_path, install_dir, plen);
        out_path[plen] = '/';
        memcpy(out_path + plen + 1, e.path, elen + 1);

        /* Create parent directories */
        ensure_parent_dirs(out_path);

        /* Extract file data */
        void *buf = kmalloc(e.size_original);
        if (!buf) continue;

        int n = cpk_extract(h, i, buf, e.size_original);
        if (n < 0) {
            kfree(buf);
            continue;
        }

        /* Write to ChaosFS */
        int fd = chaos_open(out_path, CHAOS_O_WRONLY | CHAOS_O_CREAT | CHAOS_O_TRUNC);
        if (fd >= 0) {
            chaos_write(fd, buf, (uint32_t)n);
            chaos_close(fd);
            installed++;
        }

        kfree(buf);
    }

    cpk_close(h);
    lua_pushinteger(L, installed);
    return 1;
}

static const luaL_Reg cpk_funcs[] = {
    {"open",    l_cpk_open},
    {"list",    l_cpk_list},
    {"read",    l_cpk_read},
    {"close",   l_cpk_close},
    {"install", l_cpk_install},
    {NULL, NULL}
};

void aios_register_cpk(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, cpk_funcs, 0);
    lua_setfield(L, -2, "cpk");
    lua_pop(L, 1);
}
