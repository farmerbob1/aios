/* AIOS v2 — aios.io Lua Library (Phase 7)
 * ChaosFS file I/O bindings for Lua. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../chaos/chaos.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"

/* File handle userdata — wraps a ChaosFS fd with __gc for auto-close */
#define AIOS_FILE_META "aios.file"

typedef struct {
    int fd;
    bool closed;
} aios_file_t;

static aios_file_t *check_file(lua_State *L, int idx) {
    return (aios_file_t *)luaL_checkudata(L, idx, AIOS_FILE_META);
}

/* aios.io.open(path, mode) → file_handle or nil, errmsg */
static int l_io_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *mode = luaL_optstring(L, 2, "r");

    int flags = 0;
    if (strcmp(mode, "r") == 0)       flags = CHAOS_O_RDONLY;
    else if (strcmp(mode, "w") == 0)  flags = CHAOS_O_WRONLY | CHAOS_O_CREAT | CHAOS_O_TRUNC;
    else if (strcmp(mode, "a") == 0)  flags = CHAOS_O_WRONLY | CHAOS_O_CREAT | CHAOS_O_APPEND;
    else if (strcmp(mode, "rw") == 0) flags = CHAOS_O_RDWR;
    else if (strcmp(mode, "rw+") == 0) flags = CHAOS_O_RDWR | CHAOS_O_CREAT;
    else return luaL_error(L, "invalid mode '%s'", mode);

    int fd = chaos_open(path, flags);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s'", path);
        return 2;
    }

    aios_file_t *f = (aios_file_t *)lua_newuserdatauv(L, sizeof(aios_file_t), 0);
    f->fd = fd;
    f->closed = false;
    luaL_setmetatable(L, AIOS_FILE_META);
    return 1;
}

/* aios.io.close(file) */
static int l_io_close(lua_State *L) {
    aios_file_t *f = check_file(L, 1);
    if (!f->closed) {
        chaos_close(f->fd);
        f->closed = true;
    }
    return 0;
}

/* __gc metamethod — auto-close on GC */
static int l_io_gc(lua_State *L) {
    aios_file_t *f = (aios_file_t *)luaL_checkudata(L, 1, AIOS_FILE_META);
    if (!f->closed) {
        chaos_close(f->fd);
        f->closed = true;
    }
    return 0;
}

/* aios.io.read(file, mode_or_len) */
static int l_io_read(lua_State *L) {
    aios_file_t *f = check_file(L, 1);
    if (f->closed) return luaL_error(L, "file is closed");

    if (lua_type(L, 2) == LUA_TNUMBER) {
        /* Read N bytes */
        int len = (int)luaL_checkinteger(L, 2);
        if (len <= 0) { lua_pushliteral(L, ""); return 1; }
        char *buf = kmalloc((size_t)len);
        if (!buf) return luaL_error(L, "out of memory");
        int n = chaos_read(f->fd, buf, (uint32_t)len);
        if (n <= 0) { kfree(buf); lua_pushnil(L); return 1; }
        lua_pushlstring(L, buf, (size_t)n);
        kfree(buf);
        return 1;
    }

    const char *mode = luaL_optstring(L, 2, "l");
    if (strcmp(mode, "a") == 0 || strcmp(mode, "*a") == 0) {
        /* Read all */
        struct chaos_stat st;
        chaos_fstat(f->fd, &st);
        int64_t pos = chaos_seek(f->fd, 0, CHAOS_SEEK_CUR);
        uint32_t remaining = (uint32_t)(st.size - (uint64_t)pos);
        if (remaining == 0) { lua_pushliteral(L, ""); return 1; }
        char *buf = kmalloc(remaining);
        if (!buf) return luaL_error(L, "out of memory");
        int total = 0;
        while ((uint32_t)total < remaining) {
            int n = chaos_read(f->fd, buf + total, remaining - (uint32_t)total);
            if (n <= 0) break;
            total += n;
        }
        lua_pushlstring(L, buf, (size_t)total);
        kfree(buf);
        return 1;
    }

    if (strcmp(mode, "l") == 0 || strcmp(mode, "*l") == 0) {
        /* Read line */
        char line[4096];
        int pos = 0;
        char c;
        while (pos < (int)sizeof(line) - 1) {
            int n = chaos_read(f->fd, &c, 1);
            if (n <= 0) break;
            if (c == '\n') break;
            line[pos++] = c;
        }
        if (pos == 0 && chaos_seek(f->fd, 0, CHAOS_SEEK_CUR) >= (int64_t)0) {
            /* Check if we actually hit EOF */
            char tmp;
            int n = chaos_read(f->fd, &tmp, 1);
            if (n <= 0) { lua_pushnil(L); return 1; }
            /* Put it back */
            chaos_seek(f->fd, -1, CHAOS_SEEK_CUR);
        }
        /* Strip trailing \r if present */
        if (pos > 0 && line[pos - 1] == '\r') pos--;
        lua_pushlstring(L, line, (size_t)pos);
        return 1;
    }

    return luaL_error(L, "invalid read mode '%s'", mode);
}

/* aios.io.write(file, data) */
static int l_io_write(lua_State *L) {
    aios_file_t *f = check_file(L, 1);
    if (f->closed) return luaL_error(L, "file is closed");
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int n = chaos_write(f->fd, data, (uint32_t)len);
    lua_pushboolean(L, n >= 0);
    return 1;
}

/* aios.io.seek(file, whence, offset) */
static int l_io_seek(lua_State *L) {
    aios_file_t *f = check_file(L, 1);
    if (f->closed) return luaL_error(L, "file is closed");
    const char *whence = luaL_optstring(L, 2, "cur");
    lua_Integer offset = luaL_optinteger(L, 3, 0);
    int w = CHAOS_SEEK_SET;
    if (strcmp(whence, "cur") == 0) w = CHAOS_SEEK_CUR;
    else if (strcmp(whence, "end") == 0) w = CHAOS_SEEK_END;
    int64_t pos = chaos_seek(f->fd, (int64_t)offset, w);
    if (pos < 0) { lua_pushnil(L); lua_pushliteral(L, "seek error"); return 2; }
    lua_pushinteger(L, (lua_Integer)pos);
    return 1;
}

/* aios.io.stat(path) */
static int l_io_stat(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    struct chaos_stat st;
    if (chaos_stat(path, &st) < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot stat '%s'", path);
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)st.size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, (lua_Integer)st.inode);
    lua_setfield(L, -2, "inode");
    lua_pushboolean(L, (st.mode & CHAOS_TYPE_MASK) == CHAOS_TYPE_DIR);
    lua_setfield(L, -2, "is_dir");
    lua_pushinteger(L, (lua_Integer)st.created_time);
    lua_setfield(L, -2, "created");
    lua_pushinteger(L, (lua_Integer)st.modified_time);
    lua_setfield(L, -2, "modified");
    return 1;
}

/* aios.io.exists(path) */
static int l_io_exists(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    struct chaos_stat st;
    lua_pushboolean(L, chaos_stat(path, &st) == 0);
    return 1;
}

/* aios.io.mkdir(path) */
static int l_io_mkdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int r = chaos_mkdir(path);
    lua_pushboolean(L, r == 0);
    return 1;
}

/* aios.io.rmdir(path) */
static int l_io_rmdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int r = chaos_rmdir(path);
    lua_pushboolean(L, r == 0);
    return 1;
}

/* aios.io.unlink(path) */
static int l_io_unlink(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int r = chaos_unlink(path);
    lua_pushboolean(L, r == 0);
    return 1;
}

/* aios.io.rename(old, new) */
static int l_io_rename(lua_State *L) {
    const char *old = luaL_checkstring(L, 1);
    const char *new_path = luaL_checkstring(L, 2);
    int r = chaos_rename(old, new_path);
    lua_pushboolean(L, r == 0);
    return 1;
}

/* aios.io.listdir(path) */
static int l_io_listdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int dh = chaos_opendir(path);
    if (dh < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open directory '%s'", path);
        return 2;
    }

    /* Build full path prefix for stat calls */
    size_t path_len = strlen(path);

    lua_newtable(L);
    int idx = 1;
    struct chaos_dirent de;
    while (chaos_readdir(dh, &de) == 0) {
        /* Skip . and .. */
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;

        bool is_dir = (de.type == CHAOS_DT_DIR);

        lua_newtable(L);
        lua_pushstring(L, de.name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, is_dir);
        lua_setfield(L, -2, "is_dir");

        /* Stat the entry to get size */
        if (!is_dir) {
            char fullpath[256];
            if (path_len == 1 && path[0] == '/') {
                snprintf(fullpath, sizeof(fullpath), "/%s", de.name);
            } else {
                snprintf(fullpath, sizeof(fullpath), "%s/%s", path, de.name);
            }
            struct chaos_stat st;
            if (chaos_stat(fullpath, &st) == 0) {
                lua_pushinteger(L, (lua_Integer)st.size);
                lua_setfield(L, -2, "size");
            }
        }

        lua_rawseti(L, -2, idx++);
    }
    chaos_closedir(dh);
    return 1;
}

/* aios.io.readfile(path) — convenience */
static int l_io_readfile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int fd = chaos_open(path, CHAOS_O_RDONLY);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s'", path);
        return 2;
    }
    struct chaos_stat st;
    chaos_fstat(fd, &st);
    uint32_t size = (uint32_t)st.size;
    char *buf = kmalloc(size + 1);
    if (!buf) { chaos_close(fd); return luaL_error(L, "out of memory"); }
    int total = 0;
    while ((uint32_t)total < size) {
        int n = chaos_read(fd, buf + total, size - (uint32_t)total);
        if (n <= 0) break;
        total += n;
    }
    chaos_close(fd);
    lua_pushlstring(L, buf, (size_t)total);
    kfree(buf);
    return 1;
}

/* aios.io.writefile(path, data) — convenience */
static int l_io_writefile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int fd = chaos_open(path, CHAOS_O_WRONLY | CHAOS_O_CREAT | CHAOS_O_TRUNC);
    if (fd < 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "cannot open '%s' for writing", path);
        return 2;
    }
    chaos_write(fd, data, (uint32_t)len);
    chaos_close(fd);
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg io_funcs[] = {
    {"open",     l_io_open},
    {"close",    l_io_close},
    {"read",     l_io_read},
    {"write",    l_io_write},
    {"seek",     l_io_seek},
    {"stat",     l_io_stat},
    {"exists",   l_io_exists},
    {"mkdir",    l_io_mkdir},
    {"rmdir",    l_io_rmdir},
    {"unlink",   l_io_unlink},
    {"rename",   l_io_rename},
    {"listdir",  l_io_listdir},
    {"readfile", l_io_readfile},
    {"writefile",l_io_writefile},
    {NULL, NULL}
};

void aios_register_io(lua_State *L) {
    /* Create file handle metatable with __gc */
    luaL_newmetatable(L, AIOS_FILE_META);
    lua_pushcfunction(L, l_io_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* Register aios.io table */
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, io_funcs, 0);
    lua_setfield(L, -2, "io");
    lua_pop(L, 1);
}
