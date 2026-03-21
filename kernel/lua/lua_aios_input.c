/* AIOS v2 — aios.input Lua Library (Phase 7)
 * Input event polling from keyboard/mouse drivers. */

#include "../../include/types.h"
#include "../../drivers/input.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/serial.h"

#include "lua.h"
#include "lauxlib.h"

/* Scancode-to-ASCII for char field (US QWERTY, basic) */
static const char sc_to_ascii[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ', 0,
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const char sc_to_ascii_shift[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ', 0,
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* aios.input.poll() → event table or nil */
static int l_input_poll(lua_State *L) {
    input_event_t ev;
    if (!input_poll(&ev)) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)ev.type);
    lua_setfield(L, -2, "type");

    /* Extract modifier bits from key field (set by both keyboard and mouse handlers) */
    bool has_ctrl  = (ev.key & KEY_MOD_CTRL)  != 0;
    bool has_shift = (ev.key & KEY_MOD_SHIFT) != 0;
    bool has_alt   = (ev.key & KEY_MOD_ALT)   != 0;
    uint8_t scancode = (uint8_t)(ev.key & 0xFF);

    lua_pushboolean(L, has_ctrl);   lua_setfield(L, -2, "ctrl");
    lua_pushboolean(L, has_shift);  lua_setfield(L, -2, "shift");
    lua_pushboolean(L, has_alt);    lua_setfield(L, -2, "alt");

    if (ev.type == EVENT_KEY_DOWN || ev.type == EVENT_KEY_UP) {
        lua_pushinteger(L, (lua_Integer)scancode);
        lua_setfield(L, -2, "key");

        /* Provide ASCII char for key-down events */
        if (ev.type == EVENT_KEY_DOWN && scancode < 128) {
            char c = has_shift ? sc_to_ascii_shift[scancode] : sc_to_ascii[scancode];
            if (c > 0 && c != '\b' && c != '\t' && c != '\n' && c != 27) {
                char buf[2] = { c, 0 };
                lua_pushstring(L, buf);
                lua_setfield(L, -2, "char");
            }
        }
    }

    if (ev.type == EVENT_MOUSE_MOVE || ev.type == EVENT_MOUSE_DOWN ||
        ev.type == EVENT_MOUSE_UP) {
        lua_pushinteger(L, (lua_Integer)ev.mouse_x);
        lua_setfield(L, -2, "mouse_x");
        lua_pushinteger(L, (lua_Integer)ev.mouse_y);
        lua_setfield(L, -2, "mouse_y");
        lua_pushinteger(L, (lua_Integer)ev.mouse_btn);
        lua_setfield(L, -2, "button");
    }

    return 1;
}

/* aios.input.set_gui_mode(enabled) */
static int l_input_set_gui_mode(lua_State *L) {
    bool enabled = lua_toboolean(L, 1);
    input_set_gui_mode(enabled);
    return 0;
}

static const luaL_Reg input_funcs[] = {
    {"poll", l_input_poll},
    {"set_gui_mode", l_input_set_gui_mode},
    {NULL, NULL}
};

void aios_register_input(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, input_funcs, 0);
    lua_setfield(L, -2, "input");
    lua_pop(L, 1);
}
