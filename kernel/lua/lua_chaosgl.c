/* AIOS v2 — ChaosGL Lua Bindings (Phase 8)
 * Wraps all ChaosGL C functions as Lua-callable functions under global `chaos_gl` table. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../renderer/chaos_gl.h"
#include "../../renderer/compositor.h"

#include "lua.h"
#include "lauxlib.h"

/* ── Surface management ──────────────────────────────── */

static int l_surface_create(lua_State *L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    bool has_depth = lua_toboolean(L, 3);
    int handle = chaos_gl_surface_create(w, h, has_depth);
    lua_pushinteger(L, handle);
    return 1;
}

static int l_surface_destroy(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    chaos_gl_surface_destroy(handle);
    return 0;
}

static int l_surface_bind(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    chaos_gl_surface_bind(handle);
    return 0;
}

static int l_surface_clear(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 2);
    chaos_gl_surface_clear(handle, color);
    return 0;
}

static int l_surface_present(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    chaos_gl_surface_present(handle);
    return 0;
}

static int l_surface_set_position(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    chaos_gl_surface_set_position(handle, x, y);
    return 0;
}

static int l_surface_get_position(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int x, y;
    chaos_gl_surface_get_position(handle, &x, &y);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    return 2;
}

static int l_surface_set_zorder(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int z = (int)luaL_checkinteger(L, 2);
    chaos_gl_surface_set_zorder(handle, z);
    return 0;
}

static int l_surface_get_zorder(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int z = chaos_gl_surface_get_zorder(handle);
    lua_pushinteger(L, z);
    return 1;
}

static int l_surface_set_visible(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    bool visible = lua_toboolean(L, 2);
    chaos_gl_surface_set_visible(handle, visible);
    return 0;
}

static int l_surface_set_alpha(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    uint8_t alpha = (uint8_t)luaL_checkinteger(L, 2);
    chaos_gl_surface_set_alpha(handle, alpha);
    return 0;
}

static int l_surface_resize(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    int result = chaos_gl_surface_resize(handle, w, h);
    lua_pushinteger(L, result);
    return 1;
}

static int l_surface_get_size(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int w, h;
    chaos_gl_surface_get_size(handle, &w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

static int l_surface_set_color_key(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    bool enabled = lua_toboolean(L, 2);
    uint32_t key = (uint32_t)luaL_optinteger(L, 3, 0x00FF00FF);
    chaos_gl_surface_set_color_key(handle, enabled, key);
    return 0;
}

/* ── 2D Primitives ───────────────────────────────────── */

static int l_rect(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 5);
    chaos_gl_rect(x, y, w, h, color);
    return 0;
}

static int l_rect_outline(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 5);
    int thickness = (int)luaL_optinteger(L, 6, 1);
    chaos_gl_rect_outline(x, y, w, h, color, thickness);
    return 0;
}

static int l_rect_rounded(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    int radius = (int)luaL_checkinteger(L, 5);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 6);
    chaos_gl_rect_rounded(x, y, w, h, radius, color);
    return 0;
}

static int l_rect_rounded_outline(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    int radius = (int)luaL_checkinteger(L, 5);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 6);
    int thickness = (int)luaL_optinteger(L, 7, 1);
    chaos_gl_rect_rounded_outline(x, y, w, h, radius, color, thickness);
    return 0;
}

static int l_circle(lua_State *L) {
    int cx = (int)luaL_checkinteger(L, 1);
    int cy = (int)luaL_checkinteger(L, 2);
    int radius = (int)luaL_checkinteger(L, 3);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 4);
    chaos_gl_circle(cx, cy, radius, color);
    return 0;
}

static int l_circle_outline(lua_State *L) {
    int cx = (int)luaL_checkinteger(L, 1);
    int cy = (int)luaL_checkinteger(L, 2);
    int radius = (int)luaL_checkinteger(L, 3);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 4);
    int thickness = (int)luaL_optinteger(L, 5, 1);
    chaos_gl_circle_outline(cx, cy, radius, color, thickness);
    return 0;
}

static int l_line(lua_State *L) {
    int x0 = (int)luaL_checkinteger(L, 1);
    int y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3);
    int y1 = (int)luaL_checkinteger(L, 4);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 5);
    chaos_gl_line(x0, y0, x1, y1, color);
    return 0;
}

static int l_pixel(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 3);
    chaos_gl_pixel(x, y, color);
    return 0;
}

/* ── Clip stack ──────────────────────────────────────── */

static int l_push_clip(lua_State *L) {
    rect_t r;
    r.x = (int)luaL_checkinteger(L, 1);
    r.y = (int)luaL_checkinteger(L, 2);
    r.w = (int)luaL_checkinteger(L, 3);
    r.h = (int)luaL_checkinteger(L, 4);
    chaos_gl_push_clip(r);
    return 0;
}

static int l_pop_clip(lua_State *L) {
    (void)L;
    chaos_gl_pop_clip();
    return 0;
}

static int l_reset_clip(lua_State *L) {
    (void)L;
    chaos_gl_reset_clip();
    return 0;
}

/* ── Text ────────────────────────────────────────────── */

static int l_text(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    uint32_t fg = (uint32_t)luaL_checkinteger(L, 4);
    uint32_t bg = (uint32_t)luaL_optinteger(L, 5, 0);
    uint32_t flags = (uint32_t)luaL_optinteger(L, 6, 0);
    int result = chaos_gl_text(x, y, str, fg, bg, flags);
    lua_pushinteger(L, result);
    return 1;
}

static int l_text_wrapped(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int max_w = (int)luaL_checkinteger(L, 3);
    const char *str = luaL_checkstring(L, 4);
    uint32_t fg = (uint32_t)luaL_checkinteger(L, 5);
    uint32_t bg = (uint32_t)luaL_optinteger(L, 6, 0);
    uint32_t flags = (uint32_t)luaL_optinteger(L, 7, 0);
    int result = chaos_gl_text_wrapped(x, y, max_w, str, fg, bg, flags);
    lua_pushinteger(L, result);
    return 1;
}

static int l_text_width(lua_State *L) {
    const char *str = luaL_checkstring(L, 1);
    int w = chaos_gl_text_width(str);
    lua_pushinteger(L, w);
    return 1;
}

static int l_text_height_wrapped(lua_State *L) {
    int max_w = (int)luaL_checkinteger(L, 1);
    const char *str = luaL_checkstring(L, 2);
    int h = chaos_gl_text_height_wrapped(max_w, str);
    lua_pushinteger(L, h);
    return 1;
}

static int l_char(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    char c = s[0];
    uint32_t fg = (uint32_t)luaL_checkinteger(L, 4);
    uint32_t bg = (uint32_t)luaL_optinteger(L, 5, 0);
    uint32_t flags = (uint32_t)luaL_optinteger(L, 6, 0);
    int result = chaos_gl_char(x, y, c, fg, bg, flags);
    lua_pushinteger(L, result);
    return 1;
}

/* ── TTF Fonts ───────────────────────────────────────── */

static int l_font_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    float size = (float)luaL_checknumber(L, 2);
    int handle = chaos_gl_font_load(path, size);
    lua_pushinteger(L, handle);
    return 1;
}

static int l_font_free(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    chaos_gl_font_free(handle);
    return 0;
}

static int l_set_font(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    chaos_gl_set_font(handle);
    return 0;
}

static int l_get_font(lua_State *L) {
    lua_pushinteger(L, chaos_gl_get_font());
    return 1;
}

static int l_font_text(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    const char *str = luaL_checkstring(L, 4);
    uint32_t fg = (uint32_t)luaL_checkinteger(L, 5);
    int result = chaos_gl_font_text(handle, x, y, str, fg);
    lua_pushinteger(L, result);
    return 1;
}

static int l_font_text_width(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    const char *str = luaL_checkstring(L, 2);
    lua_pushinteger(L, chaos_gl_font_text_width(handle, str));
    return 1;
}

static int l_font_text_wrapped(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int max_w = (int)luaL_checkinteger(L, 4);
    const char *str = luaL_checkstring(L, 5);
    uint32_t fg = (uint32_t)luaL_checkinteger(L, 6);
    int result = chaos_gl_font_text_wrapped(handle, x, y, max_w, str, fg);
    lua_pushinteger(L, result);
    return 1;
}

static int l_font_text_height_wrapped(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int max_w = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    lua_pushinteger(L, chaos_gl_font_text_height_wrapped(handle, max_w, str));
    return 1;
}

static int l_font_height(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, chaos_gl_font_height(handle));
    return 1;
}

static int l_font_ascent(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, chaos_gl_font_ascent(handle));
    return 1;
}

static int l_font_descent(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, chaos_gl_font_descent(handle));
    return 1;
}

static int l_font_char_width(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    const char *s = luaL_checkstring(L, 2);
    lua_pushinteger(L, chaos_gl_font_char_width(handle, s[0]));
    return 1;
}

/* ── Textures ────────────────────────────────────────── */

static int l_texture_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int handle = chaos_gl_texture_load(path);
    lua_pushinteger(L, handle);
    return 1;
}

static int l_texture_free(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    chaos_gl_texture_free(handle);
    return 0;
}

static int l_texture_get_size(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    int w, h;
    chaos_gl_texture_get_size(handle, &w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

/* ── Blit (texture handle based) ─────────────────────── */

static int l_blit(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    int tex_handle = (int)luaL_checkinteger(L, 5);
    const chaos_gl_texture_t *tex = chaos_gl_texture_get(tex_handle);
    if (!tex || !tex->data) return 0;
    chaos_gl_blit(x, y, w, h, tex->data, tex->pitch);
    return 0;
}

static int l_blit_keyed(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    int tex_handle = (int)luaL_checkinteger(L, 5);
    uint32_t key = (uint32_t)luaL_checkinteger(L, 6);
    const chaos_gl_texture_t *tex = chaos_gl_texture_get(tex_handle);
    if (!tex || !tex->data) return 0;
    chaos_gl_blit_keyed(x, y, w, h, tex->data, tex->pitch, key);
    return 0;
}

static int l_blit_alpha(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    int tex_handle = (int)luaL_checkinteger(L, 5);
    const chaos_gl_texture_t *tex = chaos_gl_texture_get(tex_handle);
    if (!tex || !tex->data) return 0;
    chaos_gl_blit_alpha(x, y, w, h, tex->data, tex->pitch);
    return 0;
}

static int l_texture_has_alpha(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, chaos_gl_texture_has_alpha(handle));
    return 1;
}

/* ── 3D Camera / Projection / Transform ──────────────── */

static int l_set_camera(lua_State *L) {
    vec3_t eye    = { (float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3) };
    vec3_t center = { (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5), (float)luaL_checknumber(L, 6) };
    vec3_t up     = { (float)luaL_checknumber(L, 7), (float)luaL_checknumber(L, 8), (float)luaL_checknumber(L, 9) };
    chaos_gl_set_camera(eye, center, up);
    return 0;
}

static int l_set_perspective(lua_State *L) {
    float fovy = (float)luaL_checknumber(L, 1);
    float aspect = (float)luaL_checknumber(L, 2);
    float z_near = (float)luaL_checknumber(L, 3);
    float z_far  = (float)luaL_checknumber(L, 4);
    chaos_gl_set_perspective(fovy, aspect, z_near, z_far);
    return 0;
}

static int l_set_transform(lua_State *L) {
    float tx = (float)luaL_checknumber(L, 1);
    float ty = (float)luaL_checknumber(L, 2);
    float tz = (float)luaL_checknumber(L, 3);
    float rx = (float)luaL_checknumber(L, 4);
    float ry = (float)luaL_checknumber(L, 5);
    float rz = (float)luaL_checknumber(L, 6);
    float sx = (float)luaL_checknumber(L, 7);
    float sy = (float)luaL_checknumber(L, 8);
    float sz = (float)luaL_checknumber(L, 9);
    chaos_gl_set_transform(tx, ty, tz, rx, ry, rz, sx, sy, sz);
    return 0;
}

/* ── 3D Model ────────────────────────────────────────── */

static int l_load_model(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    chaos_gl_model_t *model = chaos_gl_model_load(path);
    if (!model) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, model);
    return 1;
}

static int l_draw_model(lua_State *L) {
    chaos_gl_model_t *model = (chaos_gl_model_t *)lua_touserdata(L, 1);
    if (!model) return 0;
    const char *shader = luaL_checkstring(L, 2);

    /* Parse optional uniforms table */
    if (strcmp(shader, "flat") == 0) {
        flat_uniforms_t u = { .color = 0x00FFFFFF };
        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "color");
            if (!lua_isnil(L, -1)) u.color = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        chaos_gl_shader_set_by_name("flat", &u);
    } else if (strcmp(shader, "gouraud") == 0) {
        gouraud_uniforms_t u = { .light_dir = {0, -1, 0}, .ambient = 0.1f, .color = 0x00FFFFFF };
        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "light_dir_x");
            if (!lua_isnil(L, -1)) u.light_dir.x = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "light_dir_y");
            if (!lua_isnil(L, -1)) u.light_dir.y = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "light_dir_z");
            if (!lua_isnil(L, -1)) u.light_dir.z = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "ambient");
            if (!lua_isnil(L, -1)) u.ambient = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "color");
            if (!lua_isnil(L, -1)) u.color = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        chaos_gl_shader_set_by_name("gouraud", &u);
    } else if (strcmp(shader, "diffuse") == 0) {
        diffuse_uniforms_t u = { .tex_handle = -1, .light_dir = {0, -1, 0}, .ambient = 0.1f };
        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "texture");
            if (!lua_isnil(L, -1)) u.tex_handle = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "light_dir_x");
            if (!lua_isnil(L, -1)) u.light_dir.x = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "light_dir_y");
            if (!lua_isnil(L, -1)) u.light_dir.y = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "light_dir_z");
            if (!lua_isnil(L, -1)) u.light_dir.z = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "ambient");
            if (!lua_isnil(L, -1)) u.ambient = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        chaos_gl_shader_set_by_name("diffuse", &u);
    } else {
        /* Unknown shader — try by name with no uniforms */
        chaos_gl_shader_set_by_name(shader, NULL);
    }

    chaos_gl_draw_model(model);
    return 0;
}

static int l_draw_wireframe(lua_State *L) {
    chaos_gl_model_t *model = (chaos_gl_model_t *)lua_touserdata(L, 1);
    if (!model) return 0;
    uint32_t color = (uint32_t)luaL_checkinteger(L, 2);
    chaos_gl_draw_model_wire(model, color);
    return 0;
}

static int l_free_model(lua_State *L) {
    chaos_gl_model_t *model = (chaos_gl_model_t *)lua_touserdata(L, 1);
    if (model) chaos_gl_model_free(model);
    return 0;
}

/* ── Compositor ──────────────────────────────────────── */

static int l_compose(lua_State *L) {
    uint32_t bg = (uint32_t)luaL_optinteger(L, 1, 0);
    chaos_gl_compose(bg);
    return 0;
}

/* ── Boot splash ─────────────────────────────────────── */

extern void boot_splash_destroy(void);

static int l_boot_splash_destroy(lua_State *L) {
    (void)L;
    boot_splash_destroy();
    return 0;
}

/* ── Stats ───────────────────────────────────────────── */

static void push_stats_table(lua_State *L, chaos_gl_stats_t *s) {
    lua_newtable(L);
    lua_pushinteger(L, s->triangles_submitted);  lua_setfield(L, -2, "triangles_submitted");
    lua_pushinteger(L, s->triangles_culled);      lua_setfield(L, -2, "triangles_culled");
    lua_pushinteger(L, s->triangles_clipped);     lua_setfield(L, -2, "triangles_clipped");
    lua_pushinteger(L, s->triangles_drawn);       lua_setfield(L, -2, "triangles_drawn");
    lua_pushinteger(L, s->pixels_written);        lua_setfield(L, -2, "pixels_written");
    lua_pushinteger(L, s->pixels_zfailed);        lua_setfield(L, -2, "pixels_zfailed");
    lua_pushinteger(L, s->pixels_discarded);      lua_setfield(L, -2, "pixels_discarded");
    lua_pushinteger(L, s->draw_calls_2d);         lua_setfield(L, -2, "draw_calls_2d");
    lua_pushinteger(L, s->frame_time_us);         lua_setfield(L, -2, "frame_time_us");
    lua_pushinteger(L, s->frame_3d_us);           lua_setfield(L, -2, "frame_3d_us");
    lua_pushinteger(L, s->frame_2d_us);           lua_setfield(L, -2, "frame_2d_us");
    lua_pushinteger(L, s->compose_time_us);       lua_setfield(L, -2, "compose_time_us");
    lua_pushinteger(L, s->compose_blit_us);       lua_setfield(L, -2, "compose_blit_us");
    lua_pushinteger(L, s->surfaces_composited);   lua_setfield(L, -2, "surfaces_composited");
}

static int l_get_stats(lua_State *L) {
    chaos_gl_stats_t s = chaos_gl_get_stats();
    push_stats_table(L, &s);
    return 1;
}

static int l_get_compose_stats(lua_State *L) {
    chaos_gl_stats_t s = chaos_gl_get_compose_stats();
    push_stats_table(L, &s);
    return 1;
}

/* ── Registration ────────────────────────────────────── */

static const struct luaL_Reg chaosgl_funcs[] = {
    /* Surface */
    {"surface_create",       l_surface_create},
    {"surface_destroy",      l_surface_destroy},
    {"surface_bind",         l_surface_bind},
    {"surface_clear",        l_surface_clear},
    {"surface_present",      l_surface_present},
    {"surface_set_position", l_surface_set_position},
    {"surface_get_position", l_surface_get_position},
    {"surface_set_zorder",   l_surface_set_zorder},
    {"surface_get_zorder",   l_surface_get_zorder},
    {"surface_set_visible",  l_surface_set_visible},
    {"surface_set_alpha",    l_surface_set_alpha},
    {"surface_resize",       l_surface_resize},
    {"surface_get_size",     l_surface_get_size},
    {"surface_set_color_key",l_surface_set_color_key},
    /* 2D primitives */
    {"rect",                 l_rect},
    {"rect_outline",         l_rect_outline},
    {"rect_rounded",         l_rect_rounded},
    {"rect_rounded_outline", l_rect_rounded_outline},
    {"circle",               l_circle},
    {"circle_outline",       l_circle_outline},
    {"line",                 l_line},
    {"pixel",                l_pixel},
    /* Clip */
    {"push_clip",            l_push_clip},
    {"pop_clip",             l_pop_clip},
    {"reset_clip",           l_reset_clip},
    /* Text */
    {"text",                 l_text},
    {"text_wrapped",         l_text_wrapped},
    {"text_width",           l_text_width},
    {"text_height_wrapped",  l_text_height_wrapped},
    {"char",                 l_char},
    /* TTF Fonts */
    {"font_load",            l_font_load},
    {"font_free",            l_font_free},
    {"set_font",             l_set_font},
    {"get_font",             l_get_font},
    {"font_text",            l_font_text},
    {"font_text_width",      l_font_text_width},
    {"font_text_wrapped",    l_font_text_wrapped},
    {"font_text_height_wrapped", l_font_text_height_wrapped},
    {"font_height",          l_font_height},
    {"font_ascent",          l_font_ascent},
    {"font_descent",         l_font_descent},
    {"font_char_width",      l_font_char_width},
    /* Textures */
    {"load_texture",         l_texture_load},
    {"texture_load",         l_texture_load},
    {"free_texture",         l_texture_free},
    {"texture_free",         l_texture_free},
    {"texture_get_size",     l_texture_get_size},
    {"texture_has_alpha",    l_texture_has_alpha},
    /* Blit */
    {"blit",                 l_blit},
    {"blit_keyed",           l_blit_keyed},
    {"blit_alpha",           l_blit_alpha},
    /* 3D */
    {"set_camera",           l_set_camera},
    {"set_perspective",      l_set_perspective},
    {"set_transform",        l_set_transform},
    {"load_model",           l_load_model},
    {"draw_model",           l_draw_model},
    {"draw_wireframe",       l_draw_wireframe},
    {"free_model",           l_free_model},
    /* Stats */
    {"get_stats",            l_get_stats},
    {"get_compose_stats",    l_get_compose_stats},
    /* Compositor */
    {"compose",              l_compose},
    /* Boot splash */
    {"boot_splash_destroy",  l_boot_splash_destroy},
    {NULL, NULL}
};

void aios_register_chaosgl(lua_State *L) {
    lua_newtable(L);
    luaL_setfuncs(L, chaosgl_funcs, 0);
    /* Add constants */
    lua_pushinteger(L, CHAOS_GL_TEXT_BG_TRANSPARENT);
    lua_setfield(L, -2, "TEXT_BG_TRANSPARENT");
    lua_pushinteger(L, CHAOS_GL_TEXT_BG_FILL);
    lua_setfield(L, -2, "TEXT_BG_FILL");
    lua_setglobal(L, "chaos_gl");
}
