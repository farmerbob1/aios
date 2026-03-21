/* AIOS v2 — Phase 10 Acceptance Tests: Desktop & Applications
 * Tests that desktop shell + apps load without errors. */

#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "heap.h"
#include "phase10_tests.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

extern lua_State *lua_state_create(void);
extern void lua_state_destroy(lua_State *L);

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *name, bool cond) {
    if (cond) {
        pass_count++;
    } else {
        fail_count++;
        serial_printf("  FAIL: %s\n", name);
    }
}

static int run_lua(lua_State *L, const char *code) {
    int r = luaL_dostring(L, code);
    if (r != 0) {
        const char *err = lua_tostring(L, -1);
        serial_printf("  Lua error: %s\n", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
    return r;
}

static lua_Integer get_lua_int(lua_State *L, const char *var) {
    lua_getglobal(L, var);
    lua_Integer v = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}

/* ── Desktop Shell Tests ─────────────────────────────── */

static void test_wm_module_loads(lua_State *L) {
    int r = run_lua(L,
        "local ok, wm = pcall(require, 'wm')\n"
        "_wm_loaded = ok and 1 or 0\n"
        "_wm_has_init = (type(wm) == 'table' and type(wm.init) == 'function') and 1 or 0\n"
    );
    check("WM module loads via require",
        r == 0 && get_lua_int(L, "_wm_loaded") == 1
              && get_lua_int(L, "_wm_has_init") == 1);
}

static void test_wm_has_route_functions(lua_State *L) {
    int r = run_lua(L,
        "local wm = require('wm')\n"
        "_has_route_mouse = type(wm.route_mouse) == 'function' and 1 or 0\n"
        "_has_route_kb = type(wm.route_keyboard) == 'function' and 1 or 0\n"
        "_has_translate = type(wm.translate_event) == 'function' and 1 or 0\n"
    );
    check("WM has routing functions",
        r == 0 && get_lua_int(L, "_has_route_mouse") == 1
              && get_lua_int(L, "_has_route_kb") == 1
              && get_lua_int(L, "_has_translate") == 1);
}

/* ── Theme Tests ─────────────────────────────────────── */

static void test_dark_theme_wm_keys(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "ui.load_theme('/system/themes/dark.lua')\n"
        "_dt_taskbar = theme.taskbar_bg\n"
        "_dt_desktop = theme.desktop_bg\n"
        "_dt_has_warning = theme.text_warning ~= nil and 1 or 0\n"
    );
    check("Dark theme has WM keys",
        r == 0 && get_lua_int(L, "_dt_taskbar") == 0x002A2A2A
              && get_lua_int(L, "_dt_desktop") == 0x00283040
              && get_lua_int(L, "_dt_has_warning") == 1);
}

static void test_light_theme_wm_keys(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "ui.load_theme('/system/themes/light.lua')\n"
        "_lt_taskbar = theme.taskbar_bg\n"
        "_lt_desktop = theme.desktop_bg\n"
        "-- Restore dark theme\n"
        "ui.load_theme('/system/themes/dark.lua')\n"
    );
    check("Light theme has WM keys",
        r == 0 && get_lua_int(L, "_lt_taskbar") == 0x00D8D8D8
              && get_lua_int(L, "_lt_desktop") == 0x00B0C4DE);
}

/* ── App Metadata Tests ──────────────────────────────── */

static void test_app_metadata(lua_State *L) {
    int r = run_lua(L,
        "-- Read first line of terminal app for @app comment\n"
        "local ok, content = pcall(aios.io.readfile, '/apps/terminal.lua')\n"
        "_app_read_ok = ok and 1 or 0\n"
        "if ok and content then\n"
        "    local first_line = content:match('^([^\\n]*)')\n"
        "    _has_app_tag = first_line and first_line:match('@app') and 1 or 0\n"
        "else\n"
        "    _has_app_tag = 0\n"
        "end\n"
    );
    check("App metadata @app tag in terminal.lua",
        r == 0 && get_lua_int(L, "_app_read_ok") == 1
              && get_lua_int(L, "_has_app_tag") == 1);
}

/* ── Terminal Tests ──────────────────────────────────── */

static void test_terminal_eval(lua_State *L) {
    int r = run_lua(L,
        "local fn = load('return 2+2')\n"
        "local ok, result = pcall(fn)\n"
        "_eval_ok = ok and 1 or 0\n"
        "_eval_result = result\n"
    );
    check("Terminal eval 2+2=4",
        r == 0 && get_lua_int(L, "_eval_ok") == 1
              && get_lua_int(L, "_eval_result") == 4);
}

static void test_terminal_error_handling(lua_State *L) {
    int r = run_lua(L,
        "local fn, err = load('return undefined_var.foo')\n"
        "if fn then\n"
        "    local ok, result = pcall(fn)\n"
        "    _err_caught = (not ok) and 1 or 0\n"
        "else\n"
        "    _err_caught = 1\n"
        "end\n"
    );
    check("Terminal error handling",
        r == 0 && get_lua_int(L, "_err_caught") == 1);
}

/* ── File Browser Tests ──────────────────────────────── */

static void test_listdir_root(lua_State *L) {
    int r = run_lua(L,
        "local ok, entries = pcall(aios.io.listdir, '/')\n"
        "_ld_ok = ok and 1 or 0\n"
        "_ld_count = entries and #entries or 0\n"
    );
    check("File browser listdir root",
        r == 0 && get_lua_int(L, "_ld_ok") == 1
              && get_lua_int(L, "_ld_count") > 0);
}

/* ── Desktop Icons Tests ─────────────────────────────── */

static void test_desktop_icons_config(lua_State *L) {
    int r = run_lua(L,
        "local ok, icons = pcall(require, 'desktop_icons')\n"
        "_di_ok = ok and 1 or 0\n"
        "_di_count = (type(icons) == 'table') and #icons or 0\n"
        "if ok and type(icons) == 'table' and #icons > 0 then\n"
        "    _di_first_name = icons[1].name or ''\n"
        "    _di_first_app = icons[1].app or ''\n"
        "else\n"
        "    _di_first_name = ''\n"
        "    _di_first_app = ''\n"
        "end\n"
    );
    lua_getglobal(L, "_di_first_name");
    const char *name = lua_tostring(L, -1);
    lua_pop(L, 1);
    check("Desktop icons config loads",
        r == 0 && get_lua_int(L, "_di_ok") == 1
              && get_lua_int(L, "_di_count") >= 4
              && name && strcmp(name, "Files") == 0);
}

/* ── Multi-App Lifecycle ─────────────────────────────── */

static void test_multi_app_lifecycle(lua_State *L) {
    int r = run_lua(L,
        "local s1 = chaos_gl.surface_create(200, 150, false)\n"
        "local s2 = chaos_gl.surface_create(200, 150, false)\n"
        "aios.wm.register(s1, {title='TestApp1', task_id=101})\n"
        "aios.wm.register(s2, {title='TestApp2', task_id=102})\n"
        "_mal_count = #aios.wm.get_windows()\n"
        "-- Push events to both\n"
        "aios.wm.push_event(s1, {type=5, key=1})\n"
        "aios.wm.push_event(s2, {type=5, key=2})\n"
        "local ev1 = aios.wm.poll_event(s1)\n"
        "local ev2 = aios.wm.poll_event(s2)\n"
        "_mal_k1 = ev1 and ev1.key or -1\n"
        "_mal_k2 = ev2 and ev2.key or -1\n"
        "aios.wm.unregister(s1)\n"
        "aios.wm.unregister(s2)\n"
        "_mal_after = #aios.wm.get_windows()\n"
        "chaos_gl.surface_destroy(s1)\n"
        "chaos_gl.surface_destroy(s2)\n"
    );
    check("Multi-app lifecycle (spawn, events, unregister)",
        r == 0 && get_lua_int(L, "_mal_count") == 2
              && get_lua_int(L, "_mal_k1") == 1
              && get_lua_int(L, "_mal_k2") == 2
              && get_lua_int(L, "_mal_after") == 0);
}

/* ── Runner ──────────────────────────────────────────── */

void phase10_acceptance_tests(void) {
    serial_print("\n[Phase 10] Desktop & Applications acceptance tests\n");
    pass_count = 0;
    fail_count = 0;

    lua_State *L = lua_state_create();
    if (!L) {
        serial_print("  FAIL: Could not create Lua state\n");
        serial_printf("Phase 10: 0/10 tests passed\n");
        return;
    }

    test_wm_module_loads(L);
    test_wm_has_route_functions(L);
    test_dark_theme_wm_keys(L);
    test_light_theme_wm_keys(L);
    test_app_metadata(L);
    test_terminal_eval(L);
    test_terminal_error_handling(L);
    test_listdir_root(L);
    test_desktop_icons_config(L);
    test_multi_app_lifecycle(L);

    lua_state_destroy(L);

    int total = pass_count + fail_count;
    serial_printf("Phase 10: %d/%d tests passed\n", pass_count, total);
}
