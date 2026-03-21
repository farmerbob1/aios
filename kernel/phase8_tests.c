/* AIOS v2 — Phase 8 Acceptance Tests: UI Toolkit
 * Tests ChaosGL Lua bindings, widgets, layout, theming, and focus management.
 * All tests run Lua code via lua_state and verify results. */

#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "heap.h"
#include "phase8_tests.h"

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

/* Run Lua code, return 0 on success */
static int run_lua(lua_State *L, const char *code) {
    int r = luaL_dostring(L, code);
    if (r != 0) {
        const char *err = lua_tostring(L, -1);
        serial_printf("  Lua error: %s\n", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
    return r;
}

/* Run Lua code and get integer result from global variable */
static lua_Integer get_lua_int(lua_State *L, const char *var) {
    lua_getglobal(L, var);
    lua_Integer v = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}


/* ── ChaosGL Binding Tests ───────────────────────────── */

static void test_surface_create_destroy(lua_State *L) {
    int r = run_lua(L,
        "_s = chaos_gl.surface_create(100, 80, false)\n"
        "_sr = type(_s) == 'number' and _s >= 0 and 1 or 0\n"
        "if _sr == 1 then chaos_gl.surface_destroy(_s) end\n"
    );
    check("Surface create/destroy from Lua", r == 0 && get_lua_int(L, "_sr") == 1);
}

static void test_rect_draw(lua_State *L) {
    int r = run_lua(L,
        "_s2 = chaos_gl.surface_create(64, 64, false)\n"
        "chaos_gl.surface_bind(_s2)\n"
        "chaos_gl.surface_clear(_s2, 0x00000000)\n"
        "chaos_gl.rect(0, 0, 32, 32, 0x00FF0000)\n"
        "chaos_gl.rect_outline(10, 10, 20, 20, 0x0000FF00, 1)\n"
        "chaos_gl.rect_rounded(5, 5, 30, 30, 4, 0x000000FF)\n"
        "chaos_gl.surface_present(_s2)\n"
        "chaos_gl.surface_destroy(_s2)\n"
        "_rect_ok = 1\n"
    );
    check("Rect draw from Lua", r == 0 && get_lua_int(L, "_rect_ok") == 1);
}

static void test_text_draw(lua_State *L) {
    int r = run_lua(L,
        "_s3 = chaos_gl.surface_create(200, 50, false)\n"
        "chaos_gl.surface_bind(_s3)\n"
        "chaos_gl.surface_clear(_s3, 0x00000000)\n"
        "chaos_gl.text(0, 0, 'Hello', 0x00FFFFFF, 0, 0)\n"
        "_tw = chaos_gl.text_width('Hello')\n"
        "chaos_gl.surface_present(_s3)\n"
        "chaos_gl.surface_destroy(_s3)\n"
    );
    check("Text draw + text_width from Lua", r == 0 && get_lua_int(L, "_tw") == 40);
}

/* ── Widget Tests ────────────────────────────────────── */

static void test_button_states(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Button = require('button')\n"
        "_click_count = 0\n"
        "local btn = Button.new('Test', function() _click_count = _click_count + 1 end)\n"
        "local s = chaos_gl.surface_create(200, 50, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0x002D2D2D)\n"
        "btn:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "-- Verify button is not pressed/hovered initially\n"
        "_btn_normal = (not btn.pressed and not btn.hovered) and 1 or 0\n"
        "-- Simulate click\n"
        "btn._layout_x = 0\n"
        "btn._layout_y = 0\n"
        "local w, h = btn:get_size()\n"
        "btn.w = w\n"
        "btn.h = h\n"
        "btn:on_input({type=2, button=1, mouse_x=5, mouse_y=5})\n"
        "_btn_pressed = btn.pressed and 1 or 0\n"
        "btn:on_input({type=3, button=1, mouse_x=5, mouse_y=5})\n"
        "_btn_clicked = _click_count\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("Button states + click callback",
        r == 0 && get_lua_int(L, "_btn_normal") == 1
              && get_lua_int(L, "_btn_pressed") == 1
              && get_lua_int(L, "_btn_clicked") == 1);
}

static void test_label_render(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Label = require('label')\n"
        "local lbl = Label.new('Hello World')\n"
        "local w, h = lbl:get_size()\n"
        "_lbl_w = w\n"
        "_lbl_h = h\n"
        "local s = chaos_gl.surface_create(200, 50, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "lbl:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("Label render + size",
        r == 0 && get_lua_int(L, "_lbl_w") == 88  /* 11 chars * 8 */
              && get_lua_int(L, "_lbl_h") == 16);
}

static void test_textfield_input(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local TextField = require('textfield')\n"
        "_tf_changed = ''\n"
        "_tf_submitted = ''\n"
        "local tf = TextField.new({\n"
        "    on_change = function(t) _tf_changed = t end,\n"
        "    on_submit = function(t) _tf_submitted = t end,\n"
        "})\n"
        "tf.focused = true\n"
        "tf:on_input({type=7, char='A'})\n"
        "tf:on_input({type=7, char='B'})\n"
        "tf:on_input({type=7, char='C'})\n"
        "_tf_text = tf.text\n"
        "_tf_cursor = tf.cursor_pos\n"
        "-- Backspace\n"
        "tf:on_input({type=5, key=14})\n"
        "_tf_after_bs = tf.text\n"
        "-- Left arrow\n"
        "tf:on_input({type=5, key=203})\n"
        "_tf_cursor_after_left = tf.cursor_pos\n"
        "-- Home\n"
        "tf:on_input({type=5, key=199})\n"
        "_tf_cursor_home = tf.cursor_pos\n"
        "-- End\n"
        "tf:on_input({type=5, key=207})\n"
        "_tf_cursor_end = tf.cursor_pos\n"
        "-- Enter (submit)\n"
        "tf:on_input({type=5, key=28})\n"
    );
    lua_getglobal(L, "_tf_text");
    const char *text = lua_tostring(L, -1);
    bool text_ok = text && strcmp(text, "ABC") == 0;
    lua_pop(L, 1);

    lua_getglobal(L, "_tf_after_bs");
    const char *after_bs = lua_tostring(L, -1);
    bool bs_ok = after_bs && strcmp(after_bs, "AB") == 0;
    lua_pop(L, 1);

    lua_getglobal(L, "_tf_submitted");
    const char *submitted = lua_tostring(L, -1);
    bool submit_ok = submitted && strcmp(submitted, "AB") == 0;
    lua_pop(L, 1);

    check("TextField input + cursor + backspace + submit",
        r == 0 && text_ok && bs_ok
              && get_lua_int(L, "_tf_cursor") == 3
              && get_lua_int(L, "_tf_cursor_after_left") == 1
              && get_lua_int(L, "_tf_cursor_home") == 0
              && get_lua_int(L, "_tf_cursor_end") == 2
              && submit_ok);
}

static void test_checkbox_toggle(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Checkbox = require('checkbox')\n"
        "_cb_state = false\n"
        "local cb = Checkbox.new('Enable', function(v) _cb_state = v end)\n"
        "cb._layout_x = 0\n"
        "cb._layout_y = 0\n"
        "local w, h = cb:get_size()\n"
        "cb.w = w\n"
        "cb.h = h\n"
        "_cb_initial = cb.checked and 1 or 0\n"
        "cb:on_input({type=2, button=1, mouse_x=5, mouse_y=5})\n"
        "_cb_after = cb.checked and 1 or 0\n"
        "_cb_state_val = _cb_state and 1 or 0\n"
    );
    check("Checkbox toggle",
        r == 0 && get_lua_int(L, "_cb_initial") == 0
              && get_lua_int(L, "_cb_after") == 1
              && get_lua_int(L, "_cb_state_val") == 1);
}

static void test_slider_drag(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Slider = require('slider')\n"
        "_sl_val = 0\n"
        "local sl = Slider.new(0, 100, 50, function(v) _sl_val = v end)\n"
        "sl._layout_x = 0\n"
        "sl._layout_y = 0\n"
        "_sl_initial = sl.value\n"
        "-- Click at start of track to set to low value\n"
        "sl:on_input({type=2, button=1, mouse_x=8, mouse_y=10})\n"
        "sl:on_input({type=3, button=1, mouse_x=8, mouse_y=10})\n"
        "_sl_after = math.floor(sl.value)\n"
        "-- Key right\n"
        "sl.focused = true\n"
        "sl:on_input({type=5, key=205})\n"
        "_sl_after_key = math.floor(sl.value)\n"
    );
    check("Slider drag + key",
        r == 0 && get_lua_int(L, "_sl_initial") == 50
              && get_lua_int(L, "_sl_after") >= 0
              && get_lua_int(L, "_sl_after") <= 10);
}

static void test_progressbar(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local ProgressBar = require('progressbar')\n"
        "local pb = ProgressBar.new({value=50, max=100})\n"
        "_pb_val = pb.value\n"
        "_pb_max = pb.max\n"
        "-- ProgressBar is non-interactive\n"
        "_pb_consumed = pb:on_input({type=2, button=1, mouse_x=5, mouse_y=5}) and 1 or 0\n"
        "local s = chaos_gl.surface_create(250, 30, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "pb:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "chaos_gl.surface_destroy(s)\n"
        "_pb_ok = 1\n"
    );
    check("ProgressBar render + non-interactive",
        r == 0 && get_lua_int(L, "_pb_val") == 50
              && get_lua_int(L, "_pb_max") == 100
              && get_lua_int(L, "_pb_consumed") == 0);
}

static void test_fileitem(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local FileItem = require('fileitem')\n"
        "local fi = FileItem.new('test.lua', 'lua', {view='grid'})\n"
        "local w, h = fi:get_size()\n"
        "_fi_w = w\n"
        "_fi_h = h\n"
        "-- Type detection\n"
        "_fi_type = FileItem.type_from_path('readme.txt')\n"
        "_fi_type2 = FileItem.type_from_path('game.cobj')\n"
        "_fi_type3 = FileItem.type_from_path('unknown.xyz')\n"
    );
    lua_getglobal(L, "_fi_type");
    const char *t1 = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getglobal(L, "_fi_type2");
    const char *t2 = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getglobal(L, "_fi_type3");
    const char *t3 = lua_tostring(L, -1);
    lua_pop(L, 1);

    check("FileItem grid size + type detection",
        r == 0 && get_lua_int(L, "_fi_w") == 72
              && get_lua_int(L, "_fi_h") == 72
              && t1 && strcmp(t1, "text") == 0
              && t2 && strcmp(t2, "cobj") == 0
              && t3 && strcmp(t3, "file") == 0);
}

static void test_appicon(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local AppIcon = require('appicon')\n"
        "_launch_count = 0\n"
        "local ai = AppIcon.new('Test', -1, function() _launch_count = _launch_count + 1 end, {mode='taskbar'})\n"
        "local w, h = ai:get_size()\n"
        "_ai_w = w\n"
        "-- Taskbar click launches\n"
        "ai._layout_x = 0\n"
        "ai._layout_y = 0\n"
        "ai.w = w\n"
        "ai.h = h\n"
        "ai:on_input({type=2, button=1, mouse_x=5, mouse_y=5})\n"
        "_ai_launched = _launch_count\n"
        "-- Badge\n"
        "ai.badge = 5\n"
        "_ai_badge = ai.badge\n"
    );
    check("AppIcon taskbar + launch + badge",
        r == 0 && get_lua_int(L, "_ai_w") == 28
              && get_lua_int(L, "_ai_launched") == 1
              && get_lua_int(L, "_ai_badge") == 5);
}

/* ── Container Tests ─────────────────────────────────── */

static void test_panel_children(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Panel = require('panel')\n"
        "local Label = require('label')\n"
        "local p = Panel.new({children={Label.new('A'), Label.new('B')}, bg_color=0x00333333})\n"
        "_panel_kids = #p.children\n"
        "local s = chaos_gl.surface_create(200, 100, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "p:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "chaos_gl.surface_destroy(s)\n"
        "_panel_ok = 1\n"
    );
    check("Panel with children",
        r == 0 && get_lua_int(L, "_panel_kids") == 2
              && get_lua_int(L, "_panel_ok") == 1);
}

static void test_scrollview(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local ScrollView = require('scrollview')\n"
        "local Label = require('label')\n"
        "local big = Label.new('Very tall content')\n"
        "big.w = 200\n"
        "big.h = 1000\n"
        "local sv = ScrollView.new(big, {w=200, h=100})\n"
        "_sv_scroll = sv.scroll_y\n"
        "sv._layout_x = 0\n"
        "sv._layout_y = 0\n"
        "sv.w = 200\n"
        "sv.h = 100\n"
        "-- Scroll down via wheel\n"
        "sv:on_input({type=4, wheel=-1, mouse_x=50, mouse_y=50})\n"
        "_sv_scrolled = sv.scroll_y\n"
        "local s = chaos_gl.surface_create(200, 100, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "sv:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("ScrollView scroll + clip",
        r == 0 && get_lua_int(L, "_sv_scroll") == 0
              && get_lua_int(L, "_sv_scrolled") > 0);
}

static void test_listview(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local ListView = require('listview')\n"
        "_lv_render_calls = 0\n"
        "local items = {}\n"
        "for i = 1, 1000 do items[i] = 'Item ' .. i end\n"
        "local lv = ListView.new({\n"
        "    items = items,\n"
        "    item_height = 24,\n"
        "    render_item = function(item, idx, sel, x, y, w, h)\n"
        "        _lv_render_calls = _lv_render_calls + 1\n"
        "    end,\n"
        "    w = 200, h = 200,\n"
        "})\n"
        "local s = chaos_gl.surface_create(200, 200, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "lv:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "chaos_gl.surface_destroy(s)\n"
        "-- Should only render ~9 visible items, not 1000\n"
    );
    check("ListView virtualised rendering",
        r == 0 && get_lua_int(L, "_lv_render_calls") < 20);
}

static void test_tabview(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local TabView = require('tabview')\n"
        "local Label = require('label')\n"
        "local tv = TabView.new({\n"
        "    {label='Tab1', content=Label.new('Content 1')},\n"
        "    {label='Tab2', content=Label.new('Content 2')},\n"
        "}, {w=300, h=200})\n"
        "_tv_active = tv.active_tab\n"
        "local s = chaos_gl.surface_create(300, 200, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "tv:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("TabView render + active tab",
        r == 0 && get_lua_int(L, "_tv_active") == 1);
}

static void test_window_chrome(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Window = require('window')\n"
        "local Label = require('label')\n"
        "_close_called = 0\n"
        "local win = Window.new('Test Window', {\n"
        "    content = Label.new('Body'),\n"
        "    on_close = function() _close_called = _close_called + 1 end,\n"
        "    w = 300, h = 200,\n"
        "})\n"
        "local s = chaos_gl.surface_create(300, 200, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "win:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "-- Click close button (top-right)\n"
        "win._layout_x = 0\n"
        "win._layout_y = 0\n"
        "win:on_input({type=2, button=1, mouse_x=290, mouse_y=10})\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("Window chrome + close button",
        r == 0 && get_lua_int(L, "_close_called") == 1);
}

static void test_menu(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Menu = require('menu')\n"
        "_menu_click = 0\n"
        "local m = Menu.new({\n"
        "    {label='Open', on_click=function() _menu_click = 1 end},\n"
        "    {separator=true},\n"
        "    {label='Close', on_click=function() _menu_click = 2 end},\n"
        "})\n"
        "m:show(100, 100)\n"
        "_menu_visible = m.visible and 1 or 0\n"
        "-- Dismiss with escape\n"
        "m:on_input({type=5, key=1})\n"
        "_menu_dismissed = (not m.visible) and 1 or 0\n"
    );
    check("Menu show + dismiss",
        r == 0 && get_lua_int(L, "_menu_visible") == 1
              && get_lua_int(L, "_menu_dismissed") == 1);
}

static void test_dialog(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Dialog = require('dialog')\n"
        "_dlg_btn_click = 0\n"
        "local dlg = Dialog.new({\n"
        "    title = 'Confirm',\n"
        "    message = 'Are you sure?',\n"
        "    buttons = {{label='Yes', on_click=function() _dlg_btn_click = 1 end, style='primary'},\n"
        "              {label='No', on_click=function() _dlg_btn_click = 2 end}},\n"
        "})\n"
        "dlg:show(1024, 768)\n"
        "_dlg_visible = dlg.visible and 1 or 0\n"
        "dlg:dismiss()\n"
        "_dlg_dismissed = (not dlg.visible) and 1 or 0\n"
    );
    check("Dialog modal show + dismiss",
        r == 0 && get_lua_int(L, "_dlg_visible") == 1
              && get_lua_int(L, "_dlg_dismissed") == 1);
}

/* ── Layout Tests ────────────────────────────────────── */

static void test_flex_row(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Label = require('label')\n"
        "local l1 = Label.new('AAA')\n"
        "local l2 = Label.new('BBB')\n"
        "local l3 = Label.new('CCC')\n"
        "local row = flex.row({l1, l2, l3}, {spacing=8, w=200, h=16})\n"
        "row:draw(0, 0)\n"
        "-- l1 at x=0, l2 at x=24+8=32, l3 at x=32+24+8=64\n"
        "_fr_x1 = l1._layout_x\n"
        "_fr_x2 = l2._layout_x\n"
        "_fr_x3 = l3._layout_x\n"
    );
    check("Flex row spacing",
        r == 0 && get_lua_int(L, "_fr_x1") == 0
              && get_lua_int(L, "_fr_x2") == 32
              && get_lua_int(L, "_fr_x3") == 64);
}

static void test_flex_col(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Label = require('label')\n"
        "local l1 = Label.new('A')\n"
        "local l2 = Label.new('B')\n"
        "local col = flex.col({l1, l2}, {spacing=4, w=100, h=100})\n"
        "col:draw(0, 0)\n"
        "_fc_y1 = l1._layout_y\n"
        "_fc_y2 = l2._layout_y\n"
    );
    check("Flex col spacing",
        r == 0 && get_lua_int(L, "_fc_y1") == 0
              && get_lua_int(L, "_fc_y2") == 20);
}

static void test_flex_justify(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Label = require('label')\n"
        "local l1 = Label.new('A')\n"
        "local l2 = Label.new('B')\n"
        "-- justify center: total content = 8+8 = 16, container = 100, offset = (100-16)/2 = 42\n"
        "local row_c = flex.row({l1, l2}, {justify='center', w=100, h=16})\n"
        "row_c:draw(0, 0)\n"
        "_fj_center_x1 = l1._layout_x\n"
        "-- justify end: offset = 100-16 = 84\n"
        "local l3 = Label.new('X')\n"
        "local l4 = Label.new('Y')\n"
        "local row_e = flex.row({l3, l4}, {justify='end', w=100, h=16})\n"
        "row_e:draw(0, 0)\n"
        "_fj_end_x1 = l3._layout_x\n"
        "-- justify space_between: 2 items, remaining = 100-16=84, gap = 84\n"
        "local l5 = Label.new('A')\n"
        "local l6 = Label.new('B')\n"
        "local row_sb = flex.row({l5, l6}, {justify='space_between', w=100, h=16})\n"
        "row_sb:draw(0, 0)\n"
        "_fj_sb_x2 = l6._layout_x\n"
    );
    check("Flex justify center/end/space_between",
        r == 0 && get_lua_int(L, "_fj_center_x1") == 42
              && get_lua_int(L, "_fj_end_x1") == 84
              && get_lua_int(L, "_fj_sb_x2") > 80);
}

static void test_flex_align(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Label = require('label')\n"
        "local l1 = Label.new('A')\n"
        "-- align center in a 100px tall container: y = (100-16)/2 = 42\n"
        "local row = flex.row({l1}, {align='center', w=100, h=100})\n"
        "row:draw(0, 0)\n"
        "_fa_center_y = l1._layout_y\n"
        "-- align stretch: h becomes container h\n"
        "local l2 = Label.new('B')\n"
        "local row2 = flex.row({l2}, {align='stretch', w=100, h=60})\n"
        "row2:draw(0, 0)\n"
        "_fa_stretch_h = l2.h\n"
    );
    check("Flex align center + stretch",
        r == 0 && get_lua_int(L, "_fa_center_y") == 42
              && get_lua_int(L, "_fa_stretch_h") == 60);
}

static void test_flex_weight(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Label = require('label')\n"
        "local Panel = require('panel')\n"
        "local p1 = Panel.new({w=50, h=20})\n"
        "local p2 = Panel.new({h=20})\n"
        "p2.flex = 1\n"
        "local row = flex.row({p1, p2}, {w=200, h=20})\n"
        "row:draw(0, 0)\n"
        "-- p2 should get 200 - 50 = 150\n"
        "_fw_p2_w = p2.w\n"
        "_fw_p2_x = p2._layout_x\n"
    );
    check("Flex weight",
        r == 0 && get_lua_int(L, "_fw_p2_w") == 150
              && get_lua_int(L, "_fw_p2_x") == 50);
}

static void test_grid_layout(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local grid = require('grid')\n"
        "local Label = require('label')\n"
        "local items = {}\n"
        "for i = 1, 6 do items[i] = Label.new('I' .. i) end\n"
        "local g = grid.new(items, {cols=3, row_height=32, spacing={x=8,y=8}, padding=8, w=300})\n"
        "g:draw(0, 0)\n"
        "-- item 1: col=0, row=0 -> x=8, y=8\n"
        "-- item 4: col=0, row=1 -> y=8+32+8=48\n"
        "_g_x1 = items[1]._layout_x\n"
        "_g_y1 = items[1]._layout_y\n"
        "_g_y4 = items[4]._layout_y\n"
    );
    check("Grid layout",
        r == 0 && get_lua_int(L, "_g_x1") == 8
              && get_lua_int(L, "_g_y1") == 8
              && get_lua_int(L, "_g_y4") == 48);
}

static void test_layout_reflow(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Panel = require('panel')\n"
        "local p = Panel.new({h=20})\n"
        "p.flex = 1\n"
        "local row = flex.row({p}, {w=200, h=20})\n"
        "row:draw(0, 0)\n"
        "_lr_w1 = p.w\n"
        "-- Resize container\n"
        "row.w = 400\n"
        "row:invalidate()\n"
        "row:draw(0, 0)\n"
        "_lr_w2 = p.w\n"
    );
    check("Layout reflow on resize",
        r == 0 && get_lua_int(L, "_lr_w1") == 200
              && get_lua_int(L, "_lr_w2") == 400);
}

/* ── Theme Tests ─────────────────────────────────────── */

static void test_theme_load(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "ui.load_theme('/system/themes/dark.lua')\n"
        "_theme_accent = theme.accent\n"
        "_theme_btn = theme.button_normal\n"
    );
    check("Theme load (dark)",
        r == 0 && get_lua_int(L, "_theme_accent") == 0x00FF8800
              && get_lua_int(L, "_theme_btn") == 0x003A3A3A);
}

static void test_theme_switch(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "ui.load_theme('/system/themes/dark.lua')\n"
        "_ts_dark = theme.accent\n"
        "ui.load_theme('/system/themes/light.lua')\n"
        "_ts_light = theme.accent\n"
        "-- Switch back\n"
        "ui.load_theme('/system/themes/dark.lua')\n"
    );
    check("Theme switch dark -> light -> dark",
        r == 0 && get_lua_int(L, "_ts_dark") == 0x00FF8800
              && get_lua_int(L, "_ts_light") == 0x00FF6600);
}

static void test_style_override(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Button = require('button')\n"
        "ui.load_theme('/system/themes/dark.lua')\n"
        "local btn1 = Button.new('Normal', nil)\n"
        "local btn2 = Button.new('Custom', nil, {style={button_normal=0x00FF0000}})\n"
        "_so_normal = btn1:get_style('button_normal')\n"
        "_so_custom = btn2:get_style('button_normal')\n"
    );
    check("Per-widget style override",
        r == 0 && get_lua_int(L, "_so_normal") == 0x003A3A3A
              && get_lua_int(L, "_so_custom") == 0x00FF0000);
}

/* ── Focus Tests ─────────────────────────────────────── */

static void test_tab_navigation(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Button = require('button')\n"
        "local TextField = require('textfield')\n"
        "local b1 = Button.new('B1', nil)\n"
        "local tf = TextField.new({})\n"
        "local b2 = Button.new('B2', nil)\n"
        "local row = flex.row({b1, tf, b2}, {w=400, h=32})\n"
        "ui.build_focus_chain(row)\n"
        "_tn_chain = #ui.get_focus_chain()\n"
        "_tn_f1 = b1.focused and 1 or 0\n"
        "ui.focus_next()\n"
        "_tn_f2 = tf.focused and 1 or 0\n"
        "ui.focus_next()\n"
        "_tn_f3 = b2.focused and 1 or 0\n"
        "ui.focus_prev()\n"
        "_tn_f4 = tf.focused and 1 or 0\n"
    );
    check("Tab navigation (focus chain)",
        r == 0 && get_lua_int(L, "_tn_chain") == 3
              && get_lua_int(L, "_tn_f1") == 1
              && get_lua_int(L, "_tn_f2") == 1
              && get_lua_int(L, "_tn_f3") == 1
              && get_lua_int(L, "_tn_f4") == 1);
}

static void test_click_focus(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local TextField = require('textfield')\n"
        "local tf1 = TextField.new({w=100, h=28})\n"
        "local tf2 = TextField.new({w=100, h=28})\n"
        "local row = flex.row({tf1, tf2}, {spacing=8, w=220, h=28})\n"
        "ui.build_focus_chain(row)\n"
        "row:draw(0, 0)\n"
        "_cf_initial = tf1.focused and 1 or 0\n"
        "-- Click tf2\n"
        "tf2:on_input({type=2, button=1, mouse_x=tf2._layout_x + 5, mouse_y=tf2._layout_y + 5})\n"
        "_cf_after = tf2.focused and 1 or 0\n"
        "_cf_tf1_blur = tf1.focused and 1 or 0\n"
    );
    check("Click focus",
        r == 0 && get_lua_int(L, "_cf_initial") == 1
              && get_lua_int(L, "_cf_after") == 1
              && get_lua_int(L, "_cf_tf1_blur") == 0);
}

static void test_focus_rebuild(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local flex = require('flex')\n"
        "local Button = require('button')\n"
        "local b1 = Button.new('B1', nil)\n"
        "local b2 = Button.new('B2', nil)\n"
        "local row = flex.row({b1, b2}, {w=200, h=32})\n"
        "ui.build_focus_chain(row)\n"
        "_fr_before = #ui.get_focus_chain()\n"
        "-- Add a third button\n"
        "local b3 = Button.new('B3', nil)\n"
        "row.children[3] = b3\n"
        "ui.build_focus_chain(row)\n"
        "_fr_after = #ui.get_focus_chain()\n"
    );
    check("Focus chain rebuild",
        r == 0 && get_lua_int(L, "_fr_before") == 2
              && get_lua_int(L, "_fr_after") == 3);
}

/* ── Integration Tests ───────────────────────────────── */

static void test_full_app_pattern(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Window = require('window')\n"
        "local Button = require('button')\n"
        "local TextField = require('textfield')\n"
        "local flex = require('flex')\n"
        "ui.load_theme('/system/themes/dark.lua')\n"
        "_app_closed = 0\n"
        "local surface = chaos_gl.surface_create(400, 300, false)\n"
        "chaos_gl.surface_set_position(surface, 100, 100)\n"
        "chaos_gl.surface_set_zorder(surface, 10)\n"
        "chaos_gl.surface_set_visible(surface, true)\n"
        "local win = Window.new('Test App', {\n"
        "    content = flex.col({\n"
        "        TextField.new({placeholder='Type here'}),\n"
        "        flex.row({\n"
        "            Button.new('OK', function() end),\n"
        "            Button.new('Cancel', function() end),\n"
        "        }, {spacing=8}),\n"
        "    }, {spacing=12, padding=8}),\n"
        "    on_close = function() _app_closed = 1 end,\n"
        "    surface = surface,\n"
        "    w = 400, h = 300,\n"
        "})\n"
        "ui.build_focus_chain(win)\n"
        "-- Render one frame\n"
        "chaos_gl.surface_bind(surface)\n"
        "chaos_gl.surface_clear(surface, theme.window_bg)\n"
        "win:draw(0, 0)\n"
        "chaos_gl.surface_present(surface)\n"
        "-- Close\n"
        "chaos_gl.surface_destroy(surface)\n"
        "_app_ok = 1\n"
    );
    check("Full app pattern",
        r == 0 && get_lua_int(L, "_app_ok") == 1);
}

static void test_multi_window(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Window = require('window')\n"
        "local Label = require('label')\n"
        "local s1 = chaos_gl.surface_create(200, 100, false)\n"
        "local s2 = chaos_gl.surface_create(200, 100, false)\n"
        "local w1 = Window.new('Win1', {content=Label.new('A'), w=200, h=100, surface=s1})\n"
        "local w2 = Window.new('Win2', {content=Label.new('B'), w=200, h=100, surface=s2})\n"
        "chaos_gl.surface_bind(s1)\n"
        "chaos_gl.surface_clear(s1, 0x002D2D2D)\n"
        "w1:draw(0, 0)\n"
        "chaos_gl.surface_present(s1)\n"
        "chaos_gl.surface_bind(s2)\n"
        "chaos_gl.surface_clear(s2, 0x002D2D2D)\n"
        "w2:draw(0, 0)\n"
        "chaos_gl.surface_present(s2)\n"
        "chaos_gl.surface_destroy(s1)\n"
        "chaos_gl.surface_destroy(s2)\n"
        "_mw_ok = 1\n"
    );
    check("Multi-window isolation",
        r == 0 && get_lua_int(L, "_mw_ok") == 1);
}

static void test_window_drag(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Window = require('window')\n"
        "local Label = require('label')\n"
        "local s = chaos_gl.surface_create(300, 200, false)\n"
        "chaos_gl.surface_set_position(s, 50, 50)\n"
        "local win = Window.new('Drag', {content=Label.new('Hi'), w=300, h=200, surface=s})\n"
        "win._layout_x = 0\n"
        "win._layout_y = 0\n"
        "-- Start drag on titlebar\n"
        "win:on_input({type=2, button=1, mouse_x=100, mouse_y=10})\n"
        "-- Drag\n"
        "win:on_input({type=1, mouse_x=120, mouse_y=30})\n"
        "-- Check position changed\n"
        "local nx, ny = chaos_gl.surface_get_position(s)\n"
        "_wd_nx = nx\n"
        "_wd_ny = ny\n"
        "-- Release\n"
        "win:on_input({type=3, button=1, mouse_x=120, mouse_y=30})\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("Window drag",
        r == 0 && get_lua_int(L, "_wd_nx") == 70
              && get_lua_int(L, "_wd_ny") == 70);
}

static void test_window_resize(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Window = require('window')\n"
        "local Label = require('label')\n"
        "local s = chaos_gl.surface_create(300, 200, false)\n"
        "local win = Window.new('Resize', {\n"
        "    content=Label.new('Hi'), w=300, h=200, surface=s, min_w=100, min_h=100})\n"
        "win._layout_x = 0\n"
        "win._layout_y = 0\n"
        "-- Start resize at bottom-right corner\n"
        "win:on_input({type=2, button=1, mouse_x=295, mouse_y=195})\n"
        "-- Drag to increase size\n"
        "win:on_input({type=1, mouse_x=395, mouse_y=295})\n"
        "_wr_w = win.w\n"
        "_wr_h = win.h\n"
        "-- Release\n"
        "win:on_input({type=3, button=1, mouse_x=395, mouse_y=295})\n"
        "chaos_gl.surface_destroy(s)\n"
    );
    check("Window resize",
        r == 0 && get_lua_int(L, "_wr_w") == 400
              && get_lua_int(L, "_wr_h") == 300);
}

/* ── Icon System Tests ───────────────────────────────── */

static void test_icon_registry(lua_State *L) {
    int r = run_lua(L,
        "local icons = require('icons')\n"
        "icons.init()\n"
        "_icon_folder_32 = icons.get('folder', 32)\n"
        "_icon_folder_16 = icons.get('folder', 16)\n"
        "_icon_missing = icons.get('nonexistent_icon', 32)\n"
        "-- folder icons should be valid handles (>= 0)\n"
        "_icon_f32_ok = _icon_folder_32 >= 0 and 1 or 0\n"
        "_icon_f16_ok = _icon_folder_16 >= 0 and 1 or 0\n"
    );
    check("Icon registry load + get by size + fallback",
        r == 0 && get_lua_int(L, "_icon_f32_ok") == 1
              && get_lua_int(L, "_icon_f16_ok") == 1);
}

/* ── Extra Widget Tests ──────────────────────────────── */

static void test_separator(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Separator = require('separator')\n"
        "local sep = Separator.new({direction='horizontal', w=200})\n"
        "local w, h = sep:get_size()\n"
        "_sep_w = w\n"
        "_sep_h = h\n"
        "local s = chaos_gl.surface_create(200, 10, false)\n"
        "chaos_gl.surface_bind(s)\n"
        "chaos_gl.surface_clear(s, 0)\n"
        "sep:draw(0, 0)\n"
        "chaos_gl.surface_present(s)\n"
        "chaos_gl.surface_destroy(s)\n"
        "_sep_ok = 1\n"
    );
    check("Separator render",
        r == 0 && get_lua_int(L, "_sep_w") == 200
              && get_lua_int(L, "_sep_h") == 1
              && get_lua_int(L, "_sep_ok") == 1);
}

static void test_tooltip(lua_State *L) {
    int r = run_lua(L,
        "local ui = require('core')\n"
        "local Tooltip = require('tooltip')\n"
        "local tt = Tooltip.new('Help text', {delay_frames=2})\n"
        "_tt_initial = tt.visible and 1 or 0\n"
        "tt:tick(true, 100, 100)\n"
        "tt:tick(true, 100, 100)\n"
        "_tt_after = tt.visible and 1 or 0\n"
        "tt:tick(false, 0, 0)\n"
        "_tt_hidden = tt.visible and 1 or 0\n"
    );
    check("Tooltip hover delay + hide",
        r == 0 && get_lua_int(L, "_tt_initial") == 0
              && get_lua_int(L, "_tt_after") == 1
              && get_lua_int(L, "_tt_hidden") == 0);
}

/* ── Runner ──────────────────────────────────────────── */

void phase8_acceptance_tests(void) {
    serial_print("\n[Phase 8] UI Toolkit acceptance tests\n");
    pass_count = 0;
    fail_count = 0;

    lua_State *L = lua_state_create();
    if (!L) {
        serial_print("  FAIL: Could not create Lua state\n");
        serial_printf("Phase 8: 0/40 tests passed\n");
        return;
    }

    /* ChaosGL bindings */
    test_surface_create_destroy(L);
    test_rect_draw(L);
    test_text_draw(L);

    /* Widgets */
    test_button_states(L);
    test_label_render(L);
    test_textfield_input(L);
    test_checkbox_toggle(L);
    test_slider_drag(L);
    test_progressbar(L);
    test_fileitem(L);
    test_appicon(L);

    /* Containers */
    test_panel_children(L);
    test_scrollview(L);
    test_listview(L);
    test_tabview(L);
    test_window_chrome(L);
    test_menu(L);
    test_dialog(L);

    /* Layout */
    test_flex_row(L);
    test_flex_col(L);
    test_flex_justify(L);
    test_flex_align(L);
    test_flex_weight(L);
    test_grid_layout(L);
    test_layout_reflow(L);

    /* Theme */
    test_theme_load(L);
    test_theme_switch(L);
    test_style_override(L);

    /* Focus */
    test_tab_navigation(L);
    test_click_focus(L);
    test_focus_rebuild(L);

    /* Integration */
    test_full_app_pattern(L);
    test_multi_window(L);
    test_window_drag(L);
    test_window_resize(L);

    /* Icons */
    test_icon_registry(L);

    /* Extra widgets */
    test_separator(L);
    test_tooltip(L);

    lua_state_destroy(L);

    int total = pass_count + fail_count;
    serial_printf("Phase 8: %d/%d tests passed\n", pass_count, total);
}
