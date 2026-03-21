/* AIOS v2 — Phase 9 Acceptance Tests: Window Manager
 * Tests boot splash, WM registry, input routing, event queues. */

#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "heap.h"
#include "boot_splash.h"
#include "phase9_tests.h"

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

/* ── Boot Splash Tests ───────────────────────────────── */

static void test_boot_splash_init_destroy(void) {
    /* Boot splash init/destroy should not crash */
    boot_splash_init();
    boot_splash_status("Test status");
    boot_splash_destroy();
    check("Boot splash init/destroy cycle", true);
}

static void test_boot_splash_module(void) {
    boot_splash_init();
    boot_splash_module("test_mod", 1, 3);
    boot_splash_module("test_mod2", 2, 3);
    boot_splash_module("test_mod3", 3, 3);
    boot_splash_destroy();
    check("Boot splash module parade", true);
}

/* ── WM Registry Tests ───────────────────────────────── */

static void test_wm_register_unregister(lua_State *L) {
    int r = run_lua(L,
        "_s = chaos_gl.surface_create(200, 150, false)\n"
        "_reg = aios.wm.register(_s, {title='TestWin', task_id=1}) and 1 or 0\n"
        "_is_reg = aios.wm.is_registered(_s) and 1 or 0\n"
        "aios.wm.unregister(_s)\n"
        "_is_unreg = aios.wm.is_registered(_s) and 1 or 0\n"
        "chaos_gl.surface_destroy(_s)\n"
    );
    check("WM register/unregister",
        r == 0 && get_lua_int(L, "_reg") == 1
              && get_lua_int(L, "_is_reg") == 1
              && get_lua_int(L, "_is_unreg") == 0);
}

static void test_wm_focus(lua_State *L) {
    int r = run_lua(L,
        "_s1 = chaos_gl.surface_create(100, 100, false)\n"
        "_s2 = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s1, {title='Win1'})\n"
        "aios.wm.register(_s2, {title='Win2'})\n"
        "_active1 = aios.wm.get_active()\n"
        "aios.wm.focus(_s1)\n"
        "_active2 = aios.wm.get_active()\n"
        "aios.wm.unregister(_s1)\n"
        "aios.wm.unregister(_s2)\n"
        "chaos_gl.surface_destroy(_s1)\n"
        "chaos_gl.surface_destroy(_s2)\n"
    );
    check("WM focus management",
        r == 0 && get_lua_int(L, "_active1") == get_lua_int(L, "_s2")
              && get_lua_int(L, "_active2") == get_lua_int(L, "_s1"));
}

static void test_wm_z_compaction(lua_State *L) {
    /* Register many windows to trigger z-compaction */
    int r = run_lua(L,
        "_surfaces = {}\n"
        "for i = 1, 10 do\n"
        "    _surfaces[i] = chaos_gl.surface_create(50, 50, false)\n"
        "    aios.wm.register(_surfaces[i], {title='W' .. i})\n"
        "end\n"
        "-- Focus each one multiple times to push z high\n"
        "for j = 1, 10 do\n"
        "    for i = 1, 10 do\n"
        "        aios.wm.focus(_surfaces[i])\n"
        "    end\n"
        "end\n"
        "_z_ok = aios.wm.get_active() == _surfaces[10] and 1 or 0\n"
        "for i = 1, 10 do\n"
        "    aios.wm.unregister(_surfaces[i])\n"
        "    chaos_gl.surface_destroy(_surfaces[i])\n"
        "end\n"
    );
    check("WM z-order compaction", r == 0 && get_lua_int(L, "_z_ok") == 1);
}

static void test_wm_minimize_restore(lua_State *L) {
    int r = run_lua(L,
        "_s = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s, {title='MinWin'})\n"
        "aios.wm.minimize(_s)\n"
        "_wins1 = aios.wm.get_windows()\n"
        "_min1 = _wins1[1] and _wins1[1].minimized and 1 or 0\n"
        "aios.wm.restore(_s)\n"
        "_wins2 = aios.wm.get_windows()\n"
        "_min2 = _wins2[1] and _wins2[1].minimized and 1 or 0\n"
        "aios.wm.unregister(_s)\n"
        "chaos_gl.surface_destroy(_s)\n"
    );
    check("WM minimize/restore",
        r == 0 && get_lua_int(L, "_min1") == 1
              && get_lua_int(L, "_min2") == 0);
}

static void test_wm_toggle(lua_State *L) {
    int r = run_lua(L,
        "_s = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s, {title='TogWin'})\n"
        "aios.wm.toggle(_s)\n"
        "_wins1 = aios.wm.get_windows()\n"
        "_tog1 = _wins1[1] and _wins1[1].minimized and 1 or 0\n"
        "aios.wm.toggle(_s)\n"
        "_wins2 = aios.wm.get_windows()\n"
        "_tog2 = _wins2[1] and _wins2[1].minimized and 1 or 0\n"
        "aios.wm.unregister(_s)\n"
        "chaos_gl.surface_destroy(_s)\n"
    );
    check("WM toggle",
        r == 0 && get_lua_int(L, "_tog1") == 1
              && get_lua_int(L, "_tog2") == 0);
}

/* ── Input Routing Tests ─────────────────────────────── */

static void test_wm_get_windows(lua_State *L) {
    int r = run_lua(L,
        "_s1 = chaos_gl.surface_create(100, 100, false)\n"
        "_s2 = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s1, {title='App1'})\n"
        "aios.wm.register(_s2, {title='App2'})\n"
        "_wins = aios.wm.get_windows()\n"
        "_wcount = #_wins\n"
        "_w1_title = _wins[1] and _wins[1].title or ''\n"
        "_w2_title = _wins[2] and _wins[2].title or ''\n"
        "aios.wm.unregister(_s1)\n"
        "aios.wm.unregister(_s2)\n"
        "chaos_gl.surface_destroy(_s1)\n"
        "chaos_gl.surface_destroy(_s2)\n"
    );
    lua_getglobal(L, "_w1_title");
    const char *t1 = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getglobal(L, "_w2_title");
    const char *t2 = lua_tostring(L, -1);
    lua_pop(L, 1);

    check("WM get_windows list",
        r == 0 && get_lua_int(L, "_wcount") == 2
              && t1 && strcmp(t1, "App1") == 0
              && t2 && strcmp(t2, "App2") == 0);
}

static void test_wm_active_indicator(lua_State *L) {
    int r = run_lua(L,
        "_s1 = chaos_gl.surface_create(100, 100, false)\n"
        "_s2 = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s1, {title='A1'})\n"
        "aios.wm.register(_s2, {title='A2'})\n"
        "_wins = aios.wm.get_windows()\n"
        "_a1_active = 0\n"
        "_a2_active = 0\n"
        "for _, w in ipairs(_wins) do\n"
        "    if w.title == 'A1' then _a1_active = w.active and 1 or 0 end\n"
        "    if w.title == 'A2' then _a2_active = w.active and 1 or 0 end\n"
        "end\n"
        "aios.wm.unregister(_s1)\n"
        "aios.wm.unregister(_s2)\n"
        "chaos_gl.surface_destroy(_s1)\n"
        "chaos_gl.surface_destroy(_s2)\n"
    );
    check("WM active indicator in get_windows",
        r == 0 && get_lua_int(L, "_a1_active") == 0
              && get_lua_int(L, "_a2_active") == 1);
}

/* ── Event Queue Tests ───────────────────────────────── */

static void test_wm_push_poll_event(lua_State *L) {
    int r = run_lua(L,
        "_s = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s, {title='EvWin'})\n"
        "aios.wm.push_event(_s, {type=5, key=42, mouse_x=10, mouse_y=20})\n"
        "_ev = aios.wm.poll_event(_s)\n"
        "_ev_type = _ev and _ev.type or -1\n"
        "_ev_key = _ev and _ev.key or -1\n"
        "_ev_mx = _ev and _ev.mouse_x or -1\n"
        "_ev_nil = aios.wm.poll_event(_s)\n"
        "_ev_nil_ok = (_ev_nil == nil) and 1 or 0\n"
        "aios.wm.unregister(_s)\n"
        "chaos_gl.surface_destroy(_s)\n"
    );
    check("WM push/poll event",
        r == 0 && get_lua_int(L, "_ev_type") == 5
              && get_lua_int(L, "_ev_key") == 42
              && get_lua_int(L, "_ev_mx") == 10
              && get_lua_int(L, "_ev_nil_ok") == 1);
}

static void test_wm_event_overflow(lua_State *L) {
    int r = run_lua(L,
        "_s = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s, {title='OvfWin'})\n"
        "for i = 1, 300 do\n"
        "    aios.wm.push_event(_s, {type=1, mouse_x=i, mouse_y=0})\n"
        "end\n"
        "_ovf_ok = 1\n"  /* Should not crash */
        "-- Drain queue\n"
        "local count = 0\n"
        "while aios.wm.poll_event(_s) do count = count + 1 end\n"
        "_drain_count = count\n"
        "aios.wm.unregister(_s)\n"
        "chaos_gl.surface_destroy(_s)\n"
    );
    check("WM event queue overflow protection",
        r == 0 && get_lua_int(L, "_ovf_ok") == 1
              && get_lua_int(L, "_drain_count") <= 256);
}

static void test_wm_empty_poll(lua_State *L) {
    int r = run_lua(L,
        "_s = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(_s, {title='EmptyWin'})\n"
        "_ep = aios.wm.poll_event(_s)\n"
        "_ep_ok = (_ep == nil) and 1 or 0\n"
        "aios.wm.unregister(_s)\n"
        "chaos_gl.surface_destroy(_s)\n"
    );
    check("WM empty poll returns nil",
        r == 0 && get_lua_int(L, "_ep_ok") == 1);
}

/* ── Integration Tests ───────────────────────────────── */

static void test_wm_full_lifecycle(lua_State *L) {
    int r = run_lua(L,
        "local s = chaos_gl.surface_create(200, 150, false)\n"
        "chaos_gl.surface_set_position(s, 100, 100)\n"
        "chaos_gl.surface_set_visible(s, true)\n"
        "aios.wm.register(s, {title='Lifecycle', task_id=99})\n"
        "_lc_reg = aios.wm.is_registered(s) and 1 or 0\n"
        "_lc_active = aios.wm.get_active() == s and 1 or 0\n"
        "aios.wm.push_event(s, {type=2, button=1, mouse_x=50, mouse_y=50})\n"
        "local ev = aios.wm.poll_event(s)\n"
        "_lc_ev_ok = (ev and ev.type == 2) and 1 or 0\n"
        "aios.wm.minimize(s)\n"
        "aios.wm.restore(s)\n"
        "aios.wm.unregister(s)\n"
        "_lc_unreg = aios.wm.is_registered(s) and 1 or 0\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("WM full lifecycle",
        r == 0 && get_lua_int(L, "_lc_reg") == 1
              && get_lua_int(L, "_lc_active") == 1
              && get_lua_int(L, "_lc_ev_ok") == 1
              && get_lua_int(L, "_lc_unreg") == 0);
}

static void test_wm_multi_window(lua_State *L) {
    int r = run_lua(L,
        "local s1 = chaos_gl.surface_create(100, 100, false)\n"
        "local s2 = chaos_gl.surface_create(100, 100, false)\n"
        "local s3 = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(s1, {title='MW1'})\n"
        "aios.wm.register(s2, {title='MW2'})\n"
        "aios.wm.register(s3, {title='MW3'})\n"
        "_mw_count = #aios.wm.get_windows()\n"
        "aios.wm.focus(s1)\n"
        "_mw_active = aios.wm.get_active() == s1 and 1 or 0\n"
        "aios.wm.unregister(s1)\n"
        "aios.wm.unregister(s2)\n"
        "aios.wm.unregister(s3)\n"
        "chaos_gl.surface_destroy(s1)\n"
        "chaos_gl.surface_destroy(s2)\n"
        "chaos_gl.surface_destroy(s3)\n"
    );
    check("WM multi-window management",
        r == 0 && get_lua_int(L, "_mw_count") == 3
              && get_lua_int(L, "_mw_active") == 1);
}

static void test_wm_maximize_restore(lua_State *L) {
    int r = run_lua(L,
        "local s = chaos_gl.surface_create(200, 150, false)\n"
        "chaos_gl.surface_set_position(s, 50, 50)\n"
        "aios.wm.register(s, {title='MaxWin'})\n"
        "aios.wm.maximize(s)\n"
        "local mx, my = chaos_gl.surface_get_position(s)\n"
        "local mw, mh = chaos_gl.surface_get_size(s)\n"
        "_max_x = mx\n"
        "_max_y = my\n"
        "_max_w = mw\n"
        "_max_h = mh\n"
        "aios.wm.restore_size(s)\n"
        "local rx, ry = chaos_gl.surface_get_position(s)\n"
        "local rw, rh = chaos_gl.surface_get_size(s)\n"
        "_rst_x = rx\n"
        "_rst_y = ry\n"
        "_rst_w = rw\n"
        "_rst_h = rh\n"
        "aios.wm.unregister(s)\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("WM maximize/restore_size",
        r == 0 && get_lua_int(L, "_max_x") == 0
              && get_lua_int(L, "_max_y") == 0
              && get_lua_int(L, "_max_w") == 1024
              && get_lua_int(L, "_max_h") == 736
              && get_lua_int(L, "_rst_x") == 50
              && get_lua_int(L, "_rst_y") == 50
              && get_lua_int(L, "_rst_w") == 200
              && get_lua_int(L, "_rst_h") == 150);
}

static void test_wm_clamp_position(lua_State *L) {
    int r = run_lua(L,
        "local s = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(s, {title='ClampWin'})\n"
        "local cx, cy = aios.wm.clamp_position(s, -500, -10)\n"
        "_clamp_x = cx\n"
        "_clamp_y = cy\n"
        "local cx2, cy2 = aios.wm.clamp_position(s, 2000, 2000)\n"
        "_clamp_x2 = cx2\n"
        "_clamp_y2 = cy2\n"
        "aios.wm.unregister(s)\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("WM clamp_position",
        r == 0 && get_lua_int(L, "_clamp_x") == -68
              && get_lua_int(L, "_clamp_y") == 0
              && get_lua_int(L, "_clamp_x2") == 992
              && get_lua_int(L, "_clamp_y2") == 704);
}

static void test_wm_click_to_focus(lua_State *L) {
    int r = run_lua(L,
        "local s1 = chaos_gl.surface_create(100, 100, false)\n"
        "local s2 = chaos_gl.surface_create(100, 100, false)\n"
        "chaos_gl.surface_set_position(s1, 0, 0)\n"
        "chaos_gl.surface_set_position(s2, 200, 200)\n"
        "aios.wm.register(s1, {title='CF1'})\n"
        "aios.wm.register(s2, {title='CF2'})\n"
        "-- s2 is active after registration\n"
        "_cf_before = aios.wm.get_active() == s2 and 1 or 0\n"
        "-- Now focus s1\n"
        "aios.wm.focus(s1)\n"
        "_cf_after = aios.wm.get_active() == s1 and 1 or 0\n"
        "aios.wm.unregister(s1)\n"
        "aios.wm.unregister(s2)\n"
        "chaos_gl.surface_destroy(s1)\n"
        "chaos_gl.surface_destroy(s2)\n"
    );
    check("WM click-to-focus",
        r == 0 && get_lua_int(L, "_cf_before") == 1
              && get_lua_int(L, "_cf_after") == 1);
}

static void test_wm_unregister_active_fallback(lua_State *L) {
    int r = run_lua(L,
        "local s1 = chaos_gl.surface_create(100, 100, false)\n"
        "local s2 = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(s1, {title='FB1'})\n"
        "aios.wm.register(s2, {title='FB2'})\n"
        "-- s2 is active\n"
        "aios.wm.unregister(s2)\n"
        "-- Should fall back to s1\n"
        "_fb = aios.wm.get_active() == s1 and 1 or 0\n"
        "aios.wm.unregister(s1)\n"
        "chaos_gl.surface_destroy(s1)\n"
        "chaos_gl.surface_destroy(s2)\n"
    );
    check("WM unregister active falls back to next",
        r == 0 && get_lua_int(L, "_fb") == 1);
}

static void test_wm_event_with_char(lua_State *L) {
    int r = run_lua(L,
        "local s = chaos_gl.surface_create(100, 100, false)\n"
        "aios.wm.register(s, {title='CharWin'})\n"
        "aios.wm.push_event(s, {type=7, char='A'})\n"
        "local ev = aios.wm.poll_event(s)\n"
        "_ch_type = ev and ev.type or -1\n"
        "_ch_char = ev and ev.char or ''\n"
        "aios.wm.unregister(s)\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    lua_getglobal(L, "_ch_char");
    const char *ch = lua_tostring(L, -1);
    lua_pop(L, 1);
    check("WM event with char field",
        r == 0 && get_lua_int(L, "_ch_type") == 7
              && ch && strcmp(ch, "A") == 0);
}

/* ── Runner ──────────────────────────────────────────── */

void phase9_acceptance_tests(void) {
    serial_print("\n[Phase 9] Window Manager acceptance tests\n");
    pass_count = 0;
    fail_count = 0;

    /* Boot splash tests (C-level) */
    test_boot_splash_init_destroy();
    test_boot_splash_module();

    /* WM registry + event tests (Lua) */
    lua_State *L = lua_state_create();
    if (!L) {
        serial_print("  FAIL: Could not create Lua state\n");
        serial_printf("Phase 9: %d/%d tests passed\n", pass_count, pass_count + fail_count + 15);
        return;
    }

    test_wm_register_unregister(L);
    test_wm_focus(L);
    test_wm_z_compaction(L);
    test_wm_minimize_restore(L);
    test_wm_toggle(L);
    test_wm_get_windows(L);
    test_wm_active_indicator(L);
    test_wm_push_poll_event(L);
    test_wm_event_overflow(L);
    test_wm_empty_poll(L);
    test_wm_full_lifecycle(L);
    test_wm_multi_window(L);
    test_wm_maximize_restore(L);
    test_wm_clamp_position(L);
    test_wm_click_to_focus(L);
    test_wm_unregister_active_fallback(L);
    test_wm_event_with_char(L);

    lua_state_destroy(L);

    int total = pass_count + fail_count;
    serial_printf("Phase 9: %d/%d tests passed\n", pass_count, total);
}
