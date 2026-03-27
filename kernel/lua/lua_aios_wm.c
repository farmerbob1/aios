/* AIOS v2 — Window Manager C Registry (Phase 9)
 * Shared C state for cross-task window management.
 * Registered as aios.wm.* Lua table. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../renderer/chaos_gl.h"

#include "lua.h"
#include "lauxlib.h"

#define WM_MAX_WINDOWS       28
#define WM_EVENT_QUEUE_SIZE  256

typedef struct {
    uint8_t  type;
    uint16_t key;
    int16_t  mouse_x, mouse_y;
    uint8_t  button;
    uint8_t  alt, shift, ctrl;
    int8_t   wheel;
    char     ch;
} wm_event_t;

typedef struct {
    int      surface;
    char     title[64];
    int      icon_tex;
    int      task_id;
    int      z;
    bool     minimized;
    bool     modal;
    bool     in_use;
    wm_event_t events[WM_EVENT_QUEUE_SIZE];
    int      evt_head, evt_tail;
    int      restore_x, restore_y, restore_w, restore_h;
    bool     has_restore;
} wm_window_t;

static wm_window_t windows[WM_MAX_WINDOWS];
static int active_surface = -1;
static int next_z = 1;

/* ── Internal helpers ─────────────────────────────── */

static wm_window_t *find_window(int surface) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].surface == surface)
            return &windows[i];
    }
    return NULL;
}

static wm_window_t *find_free_slot(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) return &windows[i];
    }
    return NULL;
}

static void focus_next_highest(void) {
    int best_z = -1;
    int best_surf = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].in_use && !windows[i].minimized && windows[i].z > best_z) {
            best_z = windows[i].z;
            best_surf = windows[i].surface;
        }
    }
    active_surface = best_surf;
}

static void compact_zorders(void) {
    /* Simple insertion-sort compaction */
    wm_window_t *sorted[WM_MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].in_use && !windows[i].minimized) {
            sorted[count++] = &windows[i];
        }
    }
    /* Sort by z ascending */
    for (int i = 1; i < count; i++) {
        wm_window_t *tmp = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->z > tmp->z) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = tmp;
    }
    for (int i = 0; i < count; i++) {
        sorted[i]->z = i + 1;
        chaos_gl_surface_set_zorder(sorted[i]->surface, i + 1);
    }
    next_z = count;
}

static int evt_count(wm_window_t *w) {
    return (w->evt_tail - w->evt_head + WM_EVENT_QUEUE_SIZE) % WM_EVENT_QUEUE_SIZE;
}

/* ── Lua functions ────────────────────────────────── */

/* aios.wm.register(surface, opts) */
static int l_wm_register(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    if (find_window(surface)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    wm_window_t *w = find_free_slot();
    if (!w) {
        lua_pushboolean(L, 0);
        return 1;
    }

    memset(w, 0, sizeof(*w));
    w->in_use = true;
    w->surface = surface;
    w->icon_tex = -1;
    w->task_id = -1;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "title");
        if (lua_isstring(L, -1)) {
            const char *t = lua_tostring(L, -1);
            strncpy(w->title, t, sizeof(w->title) - 1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "icon");
        if (!lua_isnil(L, -1)) w->icon_tex = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 2, "task_id");
        if (!lua_isnil(L, -1)) w->task_id = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 2, "modal");
        if (lua_toboolean(L, -1)) w->modal = true;
        lua_pop(L, 1);
    }

    next_z++;
    if (next_z > 90) compact_zorders();
    w->z = next_z;
    chaos_gl_surface_set_zorder(surface, next_z);
    active_surface = surface;

    lua_pushboolean(L, 1);
    return 1;
}

/* aios.wm.unregister(surface) */
static int l_wm_unregister(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w) return 0;

    w->in_use = false;
    if (active_surface == surface) {
        active_surface = -1;
        focus_next_highest();
    }
    return 0;
}

/* aios.wm.focus(surface) */
static int l_wm_focus(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w) return 0;

    if (w->minimized) {
        w->minimized = false;
        chaos_gl_surface_set_visible(surface, true);
    }

    next_z++;
    if (next_z > 90) compact_zorders();
    w->z = next_z;
    chaos_gl_surface_set_zorder(surface, next_z);
    active_surface = surface;
    return 0;
}

/* aios.wm.minimize(surface) */
static int l_wm_minimize(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w) return 0;

    w->minimized = true;
    chaos_gl_surface_set_visible(surface, false);
    if (active_surface == surface) {
        active_surface = -1;
        focus_next_highest();
    }
    return 0;
}

/* aios.wm.restore(surface) */
static int l_wm_restore(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w) return 0;

    w->minimized = false;
    chaos_gl_surface_set_visible(surface, true);
    /* Focus it */
    next_z++;
    if (next_z > 90) compact_zorders();
    w->z = next_z;
    chaos_gl_surface_set_zorder(surface, next_z);
    active_surface = surface;
    return 0;
}

/* aios.wm.toggle(surface) */
static int l_wm_toggle(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w) return 0;

    if (w->minimized) {
        w->minimized = false;
        chaos_gl_surface_set_visible(surface, true);
        next_z++;
        if (next_z > 90) compact_zorders();
        w->z = next_z;
        chaos_gl_surface_set_zorder(surface, next_z);
        active_surface = surface;
    } else {
        w->minimized = true;
        chaos_gl_surface_set_visible(surface, false);
        if (active_surface == surface) {
            active_surface = -1;
            focus_next_highest();
        }
    }
    return 0;
}

/* aios.wm.maximize(surface) */
static int l_wm_maximize(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w) return 0;

    /* Save current geometry */
    int x, y, sw, sh;
    chaos_gl_surface_get_position(surface, &x, &y);
    chaos_gl_surface_get_size(surface, &sw, &sh);
    w->restore_x = x;
    w->restore_y = y;
    w->restore_w = sw;
    w->restore_h = sh;
    w->has_restore = true;

    chaos_gl_surface_set_position(surface, 0, 0);
    chaos_gl_surface_resize(surface, 1024, 768 - 32);
    return 0;
}

/* aios.wm.restore_size(surface) */
static int l_wm_restore_size(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w || !w->has_restore) return 0;

    chaos_gl_surface_set_position(surface, w->restore_x, w->restore_y);
    chaos_gl_surface_resize(surface, w->restore_w, w->restore_h);
    w->has_restore = false;
    return 0;
}

/* aios.wm.get_active() */
static int l_wm_get_active(lua_State *L) {
    if (active_surface >= 0) {
        lua_pushinteger(L, active_surface);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* aios.wm.get_windows() */
static int l_wm_get_windows(lua_State *L) {
    lua_newtable(L);
    int idx = 1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        wm_window_t *w = &windows[i];

        lua_newtable(L);
        lua_pushinteger(L, w->surface);     lua_setfield(L, -2, "surface");
        lua_pushstring(L, w->title);        lua_setfield(L, -2, "title");
        lua_pushinteger(L, w->icon_tex);    lua_setfield(L, -2, "icon");
        lua_pushinteger(L, w->task_id);     lua_setfield(L, -2, "task_id");
        lua_pushboolean(L, w->minimized);   lua_setfield(L, -2, "minimized");
        lua_pushboolean(L, w->surface == active_surface);
                                            lua_setfield(L, -2, "active");
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

/* aios.wm.is_registered(surface) */
static int l_wm_is_registered(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, find_window(surface) != NULL);
    return 1;
}

/* aios.wm.push_event(surface, event_table) */
static int l_wm_push_event(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w) return 0;
    if (evt_count(w) >= WM_EVENT_QUEUE_SIZE - 1) return 0;

    luaL_checktype(L, 2, LUA_TTABLE);

    wm_event_t *e = &w->events[w->evt_tail];
    memset(e, 0, sizeof(*e));

    lua_getfield(L, 2, "type");   e->type    = (uint8_t)lua_tointeger(L, -1);   lua_pop(L, 1);
    lua_getfield(L, 2, "key");    e->key     = (uint16_t)lua_tointeger(L, -1);  lua_pop(L, 1);
    lua_getfield(L, 2, "mouse_x");e->mouse_x = (int16_t)lua_tointeger(L, -1);  lua_pop(L, 1);
    lua_getfield(L, 2, "mouse_y");e->mouse_y = (int16_t)lua_tointeger(L, -1);  lua_pop(L, 1);
    lua_getfield(L, 2, "button"); e->button  = (uint8_t)lua_tointeger(L, -1);   lua_pop(L, 1);
    lua_getfield(L, 2, "alt");    e->alt     = lua_toboolean(L, -1);            lua_pop(L, 1);
    lua_getfield(L, 2, "shift");  e->shift   = lua_toboolean(L, -1);            lua_pop(L, 1);
    lua_getfield(L, 2, "ctrl");   e->ctrl    = lua_toboolean(L, -1);            lua_pop(L, 1);
    lua_getfield(L, 2, "wheel");  e->wheel   = (int8_t)lua_tointeger(L, -1);    lua_pop(L, 1);
    lua_getfield(L, 2, "char");
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (s && s[0]) e->ch = s[0];
    }
    lua_pop(L, 1);

    w->evt_tail = (w->evt_tail + 1) % WM_EVENT_QUEUE_SIZE;
    return 0;
}

/* aios.wm.poll_event(surface) */
static int l_wm_poll_event(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    wm_window_t *w = find_window(surface);
    if (!w || w->evt_head == w->evt_tail) {
        lua_pushnil(L);
        return 1;
    }

    wm_event_t *e = &w->events[w->evt_head];
    w->evt_head = (w->evt_head + 1) % WM_EVENT_QUEUE_SIZE;

    lua_newtable(L);
    lua_pushinteger(L, e->type);    lua_setfield(L, -2, "type");
    lua_pushinteger(L, e->key);     lua_setfield(L, -2, "key");
    lua_pushinteger(L, e->mouse_x); lua_setfield(L, -2, "mouse_x");
    lua_pushinteger(L, e->mouse_y); lua_setfield(L, -2, "mouse_y");
    lua_pushinteger(L, e->button);  lua_setfield(L, -2, "button");
    lua_pushboolean(L, e->alt);     lua_setfield(L, -2, "alt");
    lua_pushboolean(L, e->shift);   lua_setfield(L, -2, "shift");
    lua_pushboolean(L, e->ctrl);    lua_setfield(L, -2, "ctrl");
    lua_pushinteger(L, e->wheel);   lua_setfield(L, -2, "wheel");
    if (e->ch) {
        char buf[2] = { e->ch, 0 };
        lua_pushstring(L, buf);     lua_setfield(L, -2, "char");
    }
    return 1;
}

/* aios.wm.clamp_position(surface, x, y) */
static int l_wm_clamp_position(lua_State *L) {
    int surface = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);

    int sw, sh;
    chaos_gl_surface_get_size(surface, &sw, &sh);

    if (x < -sw + 32) x = -sw + 32;
    if (x > 1024 - 32) x = 1024 - 32;
    if (y < 0) y = 0;
    if (y > 768 - 32 - 32) y = 768 - 32 - 32;

    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    return 2;
}

/* ── Registration ────────────────────────────────── */

static const struct luaL_Reg wm_funcs[] = {
    {"register",     l_wm_register},
    {"unregister",   l_wm_unregister},
    {"focus",        l_wm_focus},
    {"minimize",     l_wm_minimize},
    {"restore",      l_wm_restore},
    {"toggle",       l_wm_toggle},
    {"maximize",     l_wm_maximize},
    {"restore_size", l_wm_restore_size},
    {"get_active",   l_wm_get_active},
    {"get_windows",  l_wm_get_windows},
    {"is_registered",l_wm_is_registered},
    {"push_event",   l_wm_push_event},
    {"poll_event",   l_wm_poll_event},
    {"clamp_position",l_wm_clamp_position},
    {NULL, NULL}
};

void aios_register_wm(lua_State *L) {
    /* Create aios table if it doesn't exist */
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }

    /* Create aios.wm sub-table */
    lua_newtable(L);
    luaL_setfuncs(L, wm_funcs, 0);
    lua_setfield(L, -2, "wm");

    lua_pop(L, 1);  /* pop aios table */
}
