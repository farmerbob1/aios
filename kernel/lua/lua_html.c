/* AIOS Lua HTML Bindings
 * Exposes Lexbor HTML parsing + layout engine to Lua as aios.html.* */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../html/html_layout.h"
#include "../../drivers/serial.h"

/* Lexbor headers for DOM traversal in get_stylesheets */
#include "lexbor/html/parser.h"
#include "lexbor/html/html.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/node.h"
#include "lexbor/tag/tag.h"

#include "lua.h"
#include "lauxlib.h"

/* aios.html.parse(html_string) → doc userdata */
static int l_html_parse(lua_State *L) {
    size_t len;
    const char *src = luaL_checklstring(L, 1, &len);

    html_doc_t *doc = html_doc_parse(src, (uint32_t)len);
    if (!doc) {
        lua_pushnil(L);
        return 1;
    }

    /* Store as light userdata */
    lua_pushlightuserdata(L, doc);
    return 1;
}

/* aios.html.title(doc) → string */
static int l_html_title(lua_State *L) {
    html_doc_t *doc = (html_doc_t *)lua_touserdata(L, 1);
    if (!doc) { lua_pushstring(L, ""); return 1; }
    lua_pushstring(L, html_doc_title(doc));
    return 1;
}

/* aios.html.layout(doc, width) → boxes_table, total_height */
static int l_html_layout(lua_State *L) {
    html_doc_t *doc = (html_doc_t *)lua_touserdata(L, 1);
    int width = (int)luaL_checkinteger(L, 2);

    if (!doc) { lua_pushnil(L); lua_pushinteger(L, 0); return 2; }

    html_layout_t *ly = html_layout_compute(doc, width);
    if (!ly) { lua_pushnil(L); lua_pushinteger(L, 0); return 2; }

    /* Build Lua table of boxes */
    lua_createtable(L, (int)ly->count, 0);

    for (uint32_t i = 0; i < ly->count; i++) {
        html_box_t *b = &ly->boxes[i];
        lua_createtable(L, 0, 10);

        lua_pushinteger(L, b->x);          lua_setfield(L, -2, "x");
        lua_pushinteger(L, b->y);          lua_setfield(L, -2, "y");
        lua_pushinteger(L, b->w);          lua_setfield(L, -2, "w");
        lua_pushinteger(L, b->h);          lua_setfield(L, -2, "h");
        lua_pushinteger(L, b->type);       lua_setfield(L, -2, "type");
        lua_pushinteger(L, b->fg_color);   lua_setfield(L, -2, "fg");

        if (b->bg_color)
            { lua_pushinteger(L, b->bg_color); lua_setfield(L, -2, "bg"); }
        if (b->text[0])
            { lua_pushstring(L, b->text); lua_setfield(L, -2, "text"); }
        if (b->url[0])
            { lua_pushstring(L, b->url); lua_setfield(L, -2, "url"); }
        if (b->style_flags & HTML_BOLD)
            { lua_pushboolean(L, 1); lua_setfield(L, -2, "bold"); }
        if (b->style_flags & HTML_UNDERLINE)
            { lua_pushboolean(L, 1); lua_setfield(L, -2, "underline"); }
        if (b->style_flags & HTML_ITALIC)
            { lua_pushboolean(L, 1); lua_setfield(L, -2, "italic"); }
        if (b->style_flags & HTML_MONOSPACE)
            { lua_pushboolean(L, 1); lua_setfield(L, -2, "mono"); }

        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }

    lua_pushinteger(L, ly->total_height);
    html_layout_free(ly);
    return 2;
}

/* aios.html.free(doc) */
static int l_html_free(lua_State *L) {
    html_doc_t *doc = (html_doc_t *)lua_touserdata(L, 1);
    if (doc) html_doc_free(doc);
    return 0;
}

/* aios.html.run_scripts(doc) → true/false */
extern int qjs_execute_scripts(void *doc_handle);

static int l_html_run_scripts(lua_State *L) {
    html_doc_t *doc = (html_doc_t *)lua_touserdata(L, 1);
    if (!doc) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int ok = qjs_execute_scripts(doc);
    lua_pushboolean(L, ok);
    return 1;
}

/* aios.html.get_stylesheets(doc) → table of {url="..."} */
static int l_html_get_stylesheets(lua_State *L) {
    html_doc_t *doc = (html_doc_t *)lua_touserdata(L, 1);
    if (!doc) { lua_newtable(L); return 1; }

    lxb_html_document_t *lxb_doc = html_doc_get_lxb(doc);
    if (!lxb_doc) { lua_newtable(L); return 1; }

    lua_newtable(L);
    int idx = 0;

    /* Walk DOM for <link rel="stylesheet" href="..."> */
    lxb_dom_node_t *root = lxb_dom_interface_node(lxb_doc->dom_document.element);
    lxb_dom_node_t *node = root;
    while (node) {
        if (lxb_dom_node_type(node) == LXB_DOM_NODE_TYPE_ELEMENT &&
            lxb_dom_node_tag_id(node) == LXB_TAG_LINK) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);
            size_t rel_len;
            const lxb_char_t *rel = lxb_dom_element_get_attribute(el,
                (const lxb_char_t *)"rel", 3, &rel_len);
            if (rel && rel_len >= 10) {
                /* Check if rel contains "stylesheet" */
                const char *r = (const char *)rel;
                int is_ss = 0;
                for (size_t i = 0; i + 9 < rel_len; i++) {
                    if (r[i] == 's' && memcmp(r + i, "stylesheet", 10) == 0) { is_ss = 1; break; }
                }
                if (is_ss) {
                    size_t href_len;
                    const lxb_char_t *href = lxb_dom_element_get_attribute(el,
                        (const lxb_char_t *)"href", 4, &href_len);
                    if (href && href_len > 0) {
                        lua_createtable(L, 0, 1);
                        lua_pushlstring(L, (const char *)href, href_len);
                        lua_setfield(L, -2, "url");
                        lua_rawseti(L, -2, ++idx);
                    }
                }
            }
        }
        /* Depth-first traversal */
        if (lxb_dom_node_first_child(node)) {
            node = lxb_dom_node_first_child(node);
        } else {
            while (node && !lxb_dom_node_next(node) && node != root) {
                node = lxb_dom_node_parent(node);
            }
            if (node == root) break;
            if (node) node = lxb_dom_node_next(node);
        }
    }

    return 1;
}

/* aios.html.attach_css(doc, css_text) → true/false */
static int l_html_attach_css(lua_State *L) {
    html_doc_t *doc = (html_doc_t *)lua_touserdata(L, 1);
    size_t len;
    const char *css = luaL_checklstring(L, 2, &len);
    if (!doc || !css || len == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int ok = html_doc_attach_css(doc, css, (uint32_t)len);
    lua_pushboolean(L, ok == 0);
    return 1;
}

/* ── Registration ────────────────────────────────────── */

static const luaL_Reg html_funcs[] = {
    {"parse",           l_html_parse},
    {"title",           l_html_title},
    {"layout",          l_html_layout},
    {"free",            l_html_free},
    {"run_scripts",     l_html_run_scripts},
    {"get_stylesheets", l_html_get_stylesheets},
    {"attach_css",      l_html_attach_css},
    {NULL, NULL}
};

extern void qjs_bridge_register(lua_State *L);

void aios_register_html(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, html_funcs, 0);
    lua_setfield(L, -2, "html");
    lua_pop(L, 1);

    /* Register QuickJS bridge functions (get_scripts, exec_scripts) */
    qjs_bridge_register(L);
}
