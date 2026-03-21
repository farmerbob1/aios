/* AIOS v2 — Custom Lua Module Loader (Phase 7)
 * Provides require/dofile/loadfile backed by ChaosFS. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../chaos/chaos.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Read an entire file from ChaosFS into a kmalloc'd buffer.
 * Returns size on success, -1 on error. Caller must kfree(*out). */
int aios_load_file_from_chaosfs(const char *path, char **out) {
    int fd = chaos_open(path, CHAOS_O_RDONLY);
    if (fd < 0) return -1;

    struct chaos_stat st;
    if (chaos_fstat(fd, &st) < 0) {
        chaos_close(fd);
        return -1;
    }

    uint32_t size = (uint32_t)st.size;
    char *buf = kmalloc(size + 1);
    if (!buf) {
        chaos_close(fd);
        return -1;
    }

    int total = 0;
    while ((uint32_t)total < size) {
        int n = chaos_read(fd, buf + total, size - (uint32_t)total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    chaos_close(fd);

    *out = buf;
    return total;
}

/* Internal dofile: load + pcall, used by lua_task_entry */
int aios_dofile_internal(lua_State *L, const char *path) {
    char *buf = NULL;
    int len = aios_load_file_from_chaosfs(path, &buf);
    if (len < 0) {
        lua_pushfstring(L, "cannot open '%s'", path);
        return -1;
    }

    int err = luaL_loadbuffer(L, buf, (size_t)len, path);
    kfree(buf);
    if (err) return err;

    err = lua_pcall(L, 0, LUA_MULTRET, 0);
    return err;
}

/* Lua-callable dofile(path) */
static int aios_dofile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char *buf = NULL;
    int len = aios_load_file_from_chaosfs(path, &buf);
    if (len < 0)
        return luaL_error(L, "cannot open '%s'", path);

    int err = luaL_loadbuffer(L, buf, (size_t)len, path);
    kfree(buf);
    if (err)
        return lua_error(L);

    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L);
}

/* Lua-callable loadfile(path) */
static int aios_loadfile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char *buf = NULL;
    int len = aios_load_file_from_chaosfs(path, &buf);
    if (len < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s'", path);
        return 2;
    }

    int err = luaL_loadbuffer(L, buf, (size_t)len, path);
    kfree(buf);
    if (err) {
        lua_pushnil(L);
        lua_insert(L, -2);  /* nil, error message */
        return 2;
    }
    return 1;  /* compiled chunk */
}

/* Custom searcher for require(): searches ChaosFS paths */
static int aios_searcher(lua_State *L) {
    const char *modname = luaL_checkstring(L, 1);
    char path[256];

    /* Search order */
    static const char *prefixes[] = {
        "/system/ui/",
        "/system/layout/",
        "/system/",
        "/apps/",
        "/test/",
        NULL
    };

    /* Try absolute path first */
    if (modname[0] == '/') {
        char *buf = NULL;
        int len = aios_load_file_from_chaosfs(modname, &buf);
        if (len >= 0) {
            int err = luaL_loadbuffer(L, buf, (size_t)len, modname);
            kfree(buf);
            if (err == 0) return 1;
            return lua_error(L);
        }
    }

    /* Try each prefix + modname + .lua */
    for (const char **p = prefixes; *p; p++) {
        size_t plen = strlen(*p);
        size_t mlen = strlen(modname);
        if (plen + mlen + 4 >= sizeof(path)) continue;
        memcpy(path, *p, plen);
        memcpy(path + plen, modname, mlen);
        memcpy(path + plen + mlen, ".lua", 5);

        char *buf = NULL;
        int len = aios_load_file_from_chaosfs(path, &buf);
        if (len >= 0) {
            int err = luaL_loadbuffer(L, buf, (size_t)len, path);
            kfree(buf);
            if (err == 0) return 1;
            return lua_error(L);
        }
    }

    /* Not found */
    lua_pushfstring(L, "\n\tno file on ChaosFS for '%s'", modname);
    return 1;
}

/* Install our custom loader into the Lua state */
void aios_install_loader(lua_State *L) {
    /* Override dofile and loadfile */
    lua_pushcfunction(L, aios_dofile);
    lua_setglobal(L, "dofile");
    lua_pushcfunction(L, aios_loadfile);
    lua_setglobal(L, "loadfile");

    /* Install custom searcher into package.searchers */
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "searchers");
        if (lua_istable(L, -1)) {
            /* Clear existing searchers and add ours */
            int len = (int)lua_rawlen(L, -1);
            /* Keep searcher #1 (preload), replace #2, remove rest */
            lua_pushcfunction(L, aios_searcher);
            lua_rawseti(L, -2, 2);
            /* Remove searchers 3+ */
            for (int i = len; i > 2; i--) {
                lua_pushnil(L);
                lua_rawseti(L, -2, i);
            }
        }
        lua_pop(L, 1); /* pop searchers */
    }
    lua_pop(L, 1); /* pop package */
}
