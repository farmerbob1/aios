/* AIOS QuickJS Bridge
 * Two-phase script execution:
 *   Phase 1: get_scripts(doc) → Lua table of {type, code/url} entries
 *   Phase 2: run_scripts(doc, scripts_array) → execute resolved scripts
 *
 * This allows Lua to fetch external <script src="..."> via HTTP between phases. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"

#include "quickjs.h"

/* ── Execution timeout interrupt handler ─────────────── */
/* Kills scripts after 5 seconds to prevent infinite loops */

#define QJS_TIMEOUT_TICKS  (5 * 250)  /* 5 seconds at 250 Hz */

typedef struct {
    uint32_t start_ticks;
    uint32_t timeout_ticks;
} qjs_timeout_t;

static int qjs_interrupt_handler(JSRuntime *rt, void *opaque) {
    (void)rt;
    qjs_timeout_t *to = (qjs_timeout_t *)opaque;
    uint32_t elapsed = timer_get_ticks() - to->start_ticks;
    if (elapsed > to->timeout_ticks) {
        serial_printf("[qjs] script timeout (%u ticks)\n", elapsed);
        return 1;  /* interrupt execution */
    }
    return 0;
}

/* Lua headers */
#include "lua.h"
#include "lauxlib.h"

/* Lexbor headers */
#include "lexbor/html/parser.h"
#include "lexbor/html/html.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/text.h"
#include "lexbor/dom/interfaces/node.h"
#include "lexbor/tag/tag.h"

/* DOM bridge (qjs_dom.c) */
extern void qjs_dom_init_classes(JSContext *ctx);
extern JSValue qjs_dom_wrap_document(JSContext *ctx, lxb_html_document_t *doc);
extern void qjs_dom_install_console(JSContext *ctx);

/* Custom allocator from qjs_init.c */
extern const JSMallocFunctions qjs_aios_mf;

/* html_layout.c accessor */
extern lxb_html_document_t *html_doc_get_lxb(void *doc_handle);

/* ── Script discovery ────────────────────────────────── */

#define MAX_SCRIPTS     128
#define MAX_SCRIPT_SIZE (128 * 1024)  /* 128KB per script */

typedef struct {
    int is_external;    /* 0 = inline code, 1 = external URL */
    char *data;         /* inline: code text, external: URL string */
    uint32_t len;
} script_info_t;

/* Walk DOM tree, collect script entries (inline code + external URLs) in order */
static void collect_scripts(lxb_dom_node_t *node, script_info_t *scripts,
                            int *count, int max) {
    while (node) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            lxb_dom_node_tag_id(node) == LXB_TAG_SCRIPT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);

            /* Check for type attribute — skip non-JS types */
            size_t type_len;
            const lxb_char_t *type_attr = lxb_dom_element_get_attribute(el,
                (const lxb_char_t *)"type", 4, &type_len);
            if (type_attr && type_len > 0) {
                /* Skip JSON, module, and non-JS types */
                const char *t = (const char *)type_attr;
                if (strstr(t, "json") || strstr(t, "module") ||
                    strstr(t, "template") || strstr(t, "importmap")) {
                    node = lxb_dom_node_next(node);
                    continue;
                }
            }

            if (*count >= max) { node = lxb_dom_node_next(node); continue; }

            /* Check for src attribute */
            size_t src_len;
            const lxb_char_t *src = lxb_dom_element_get_attribute(el,
                (const lxb_char_t *)"src", 3, &src_len);

            if (src && src_len > 0) {
                /* External script */
                scripts[*count].is_external = 1;
                scripts[*count].data = (char *)kmalloc(src_len + 1);
                if (scripts[*count].data) {
                    memcpy(scripts[*count].data, src, src_len);
                    scripts[*count].data[src_len] = '\0';
                    scripts[*count].len = (uint32_t)src_len;
                    (*count)++;
                }
            } else {
                /* Inline script */
                size_t text_len;
                lxb_char_t *text = lxb_dom_node_text_content(node, &text_len);
                if (text && text_len > 0 && text_len < MAX_SCRIPT_SIZE) {
                    scripts[*count].is_external = 0;
                    scripts[*count].data = (char *)kmalloc(text_len + 1);
                    if (scripts[*count].data) {
                        memcpy(scripts[*count].data, text, text_len);
                        scripts[*count].data[text_len] = '\0';
                        scripts[*count].len = (uint32_t)text_len;
                        (*count)++;
                    }
                }
                if (text) {
                    lxb_dom_document_t *doc = node->owner_document;
                    if (doc) lexbor_mraw_free(doc->mraw, text);
                }
            }
            node = lxb_dom_node_next(node);
            continue;
        }

        /* Recurse into children (but NOT into <script> children) */
        if (lxb_dom_node_first_child(node)) {
            collect_scripts(lxb_dom_node_first_child(node), scripts, count, max);
        }
        node = lxb_dom_node_next(node);
    }
}

/* ── Lua API: aios.html.get_scripts(doc) ─────────────── */
/* Returns: { {type="inline", code="..."}, {type="external", url="..."}, ... } */

static int l_html_get_scripts(lua_State *L) {
    void *doc_handle = lua_touserdata(L, 1);
    if (!doc_handle) { lua_newtable(L); return 1; }

    lxb_html_document_t *lxb_doc = html_doc_get_lxb(doc_handle);
    if (!lxb_doc) { lua_newtable(L); return 1; }

    script_info_t scripts[MAX_SCRIPTS];
    int count = 0;
    memset(scripts, 0, sizeof(scripts));

    lxb_dom_node_t *root = lxb_dom_interface_node(lxb_doc->dom_document.element);
    if (root) {
        collect_scripts(lxb_dom_node_first_child(root), scripts, &count, MAX_SCRIPTS);
    }

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_createtable(L, 0, 2);
        if (scripts[i].is_external) {
            lua_pushstring(L, "external");
            lua_setfield(L, -2, "type");
            lua_pushstring(L, scripts[i].data);
            lua_setfield(L, -2, "url");
        } else {
            lua_pushstring(L, "inline");
            lua_setfield(L, -2, "type");
            lua_pushlstring(L, scripts[i].data, scripts[i].len);
            lua_setfield(L, -2, "code");
        }
        lua_rawseti(L, -2, i + 1);
        kfree(scripts[i].data);
    }

    return 1;
}

/* ── Lua API: aios.html.exec_scripts(doc, scripts_array) ── */
/* scripts_array: Lua table of strings (resolved JS code, in order) */

static int l_html_exec_scripts(lua_State *L) {
    void *doc_handle = lua_touserdata(L, 1);
    if (!doc_handle || !lua_istable(L, 2)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lxb_html_document_t *lxb_doc = html_doc_get_lxb(doc_handle);
    if (!lxb_doc) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Create QuickJS runtime + context */
    JSRuntime *rt = JS_NewRuntime2(&qjs_aios_mf, NULL);
    if (!rt) { lua_pushboolean(L, 0); return 1; }
    JS_SetMemoryLimit(rt, 8 * 1024 * 1024);   /* 8MB for page scripts */
    JS_SetMaxStackSize(rt, 512 * 1024);

    /* Install timeout interrupt — kill scripts after 5 seconds */
    qjs_timeout_t timeout = { .start_ticks = timer_get_ticks(),
                               .timeout_ticks = QJS_TIMEOUT_TICKS };
    JS_SetInterruptHandler(rt, qjs_interrupt_handler, &timeout);

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Install DOM bridge */
    qjs_dom_init_classes(ctx);

    /* Set globals */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue js_doc = qjs_dom_wrap_document(ctx, lxb_doc);
    JS_SetPropertyStr(ctx, global, "document", js_doc);
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    qjs_dom_install_console(ctx);

    /* Stubs — comprehensive browser environment for real-world pages */
    const char *stubs =
        /* Navigator / Location */
        "var navigator = { userAgent: 'Mozilla/5.0 AIOS/2.0', platform: 'AIOS',\n"
        "  appName: 'Netscape', language: 'en', languages: ['en'],\n"
        "  cookieEnabled: false, onLine: true };\n"
        "var location = { href: '', hostname: '', pathname: '/', protocol: 'https:',\n"
        "  search: '', hash: '', origin: '', host: '' };\n"
        "var history = { pushState: function(){}, replaceState: function(){}, back: function(){} };\n"
        "var screen = { width: 1024, height: 768, availWidth: 1024, availHeight: 768,\n"
        "  colorDepth: 32, pixelDepth: 32 };\n"

        /* Timers (no-op) */
        "var _dcl_handlers = [];\n"
        "var _tid = 1;\n"
        "function setTimeout(fn, ms) { return _tid++; }\n"
        "function clearTimeout(id) {}\n"
        "function setInterval(fn, ms) { return _tid++; }\n"
        "function clearInterval(id) {}\n"
        "function requestAnimationFrame(fn) { return _tid++; }\n"
        "function cancelAnimationFrame(id) {}\n"
        "function queueMicrotask(fn) { try { fn(); } catch(e) {} }\n"

        /* Style / Layout */
        "function getComputedStyle(el) {\n"
        "  return new Proxy({}, { get: function(t,p) {\n"
        "    if (el && el.style) return el.style[p] || '';\n"
        "    return '';\n"
        "  }});\n"
        "}\n"
        "window.getComputedStyle = getComputedStyle;\n"
        "window.matchMedia = function(q) { return { matches: false, media: q,\n"
        "  addEventListener: function(){}, removeEventListener: function(){} }; };\n"
        "window.innerWidth = 1024; window.innerHeight = 768;\n"
        "window.outerWidth = 1024; window.outerHeight = 768;\n"
        "window.scrollX = 0; window.scrollY = 0;\n"
        "window.devicePixelRatio = 1;\n"
        "window.scroll = function(){}; window.scrollTo = function(){};\n"

        /* Document properties */
        "document.cookie = '';\n"
        "document.readyState = 'loading';\n"
        "document.charset = 'UTF-8';\n"
        "document.characterSet = 'UTF-8';\n"
        "document.compatMode = 'CSS1Compat';\n"
        "document.visibilityState = 'visible';\n"
        "document.hidden = false;\n"
        "document.dir = 'ltr';\n"
        "document.domain = '';\n"
        "document.referrer = '';\n"
        "document.URL = '';\n"
        "document.location = location;\n"
        "document.defaultView = window;\n"
        "if (document.documentElement) {\n"
        "  document.documentElement.dir = 'ltr';\n"
        "  document.documentElement.lang = 'en';\n"
        "}\n"
        "document.head = document.getElementsByTagName('head')[0] || null;\n"

        /* Event handling */
        "document.addEventListener = function(ev, fn, opts) {\n"
        "  if (ev === 'DOMContentLoaded') _dcl_handlers.push(fn);\n"
        "};\n"
        "document.removeEventListener = function(){};\n"
        "window.addEventListener = function(ev, fn, opts) {\n"
        "  if (ev === 'DOMContentLoaded' || ev === 'load') _dcl_handlers.push(fn);\n"
        "};\n"
        "window.removeEventListener = function(){};\n"
        "window.dispatchEvent = function(){ return true; };\n"

        /* DOM query stubs — use real getElementById when possible */
        "document.querySelectorAll = function(sel) {\n"
        "  if (sel.charAt(0) === '#') {\n"
        "    var el = document.getElementById(sel.substring(1));\n"
        "    return el ? [el] : [];\n"
        "  }\n"
        "  if (sel.charAt(0) === '.') {\n"
        "    return document.getElementsByClassName(sel.substring(1));\n"
        "  }\n"
        "  return document.getElementsByTagName(sel);\n"
        "};\n"
        "document.querySelector = function(sel) {\n"
        "  var r = document.querySelectorAll(sel);\n"
        "  return r.length > 0 ? r[0] : null;\n"
        "};\n"

        /* Element creation */
        "document.createElement = function(tag) {\n"
        "  var el = { tagName: tag.toUpperCase(), nodeType: 1, style: {},\n"
        "    attributes: {}, childNodes: [], children: [],\n"
        "    className: '', id: '', textContent: '', innerHTML: '',\n"
        "    setAttribute: function(n,v) { this.attributes[n] = v;\n"
        "      if (n === 'class') this.className = v;\n"
        "      if (n === 'id') this.id = v; },\n"
        "    getAttribute: function(n) { return this.attributes[n] || null; },\n"
        "    removeAttribute: function(n) { delete this.attributes[n]; },\n"
        "    hasAttribute: function(n) { return n in this.attributes; },\n"
        "    appendChild: function(c) { this.childNodes.push(c);\n"
        "      if (c.nodeType === 1) this.children.push(c);\n"
        "      c.parentNode = this; return c; },\n"
        "    removeChild: function(c) { return c; },\n"
        "    insertBefore: function(n, r) { this.childNodes.push(n); return n; },\n"
        "    addEventListener: function(){},\n"
        "    removeEventListener: function(){},\n"
        "    classList: { add: function(){}, remove: function(){},\n"
        "      contains: function(){ return false; }, toggle: function(){} },\n"
        "    dataset: {},\n"
        "    parentNode: null, firstChild: null, lastChild: null,\n"
        "    nextSibling: null, previousSibling: null,\n"
        "    getBoundingClientRect: function() {\n"
        "      return {top:0,left:0,bottom:0,right:0,width:0,height:0}; },\n"
        "    cloneNode: function(deep) { return document.createElement(tag); },\n"
        "    contains: function(n) { return false; }\n"
        "  };\n"
        "  return el;\n"
        "};\n"
        "document.createTextNode = function(t) {\n"
        "  return { textContent: t, nodeType: 3, parentNode: null };\n"
        "};\n"
        "document.createDocumentFragment = function() {\n"
        "  return document.createElement('fragment');\n"
        "};\n"
        "document.createComment = function(t) { return { nodeType: 8 }; };\n"
        "document.createElementNS = function(ns, tag) { return document.createElement(tag); };\n"

        /* Performance API stub */
        "var performance = { now: function() { return 0; },\n"
        "  mark: function(){}, measure: function(){},\n"
        "  getEntriesByName: function(){ return []; } };\n"
        "window.performance = performance;\n"

        /* Storage stubs */
        "var _storage = {};\n"
        "var localStorage = { getItem: function(k) { return _storage[k] || null; },\n"
        "  setItem: function(k,v) { _storage[k] = String(v); },\n"
        "  removeItem: function(k) { delete _storage[k]; },\n"
        "  clear: function() { _storage = {}; }, length: 0 };\n"
        "var sessionStorage = localStorage;\n"
        "window.localStorage = localStorage;\n"
        "window.sessionStorage = sessionStorage;\n"

        /* Misc */
        "var XMLHttpRequest = function() {\n"
        "  this.open = function(){}; this.send = function(){};\n"
        "  this.setRequestHeader = function(){};\n"
        "  this.addEventListener = function(){};\n"
        "  this.readyState = 0; this.status = 0; this.responseText = '';\n"
        "};\n"
        "window.XMLHttpRequest = XMLHttpRequest;\n"
        "window.fetch = function() { return Promise.reject('no fetch'); };\n"
        "window.MutationObserver = function(cb) {\n"
        "  this.observe = function(){}; this.disconnect = function(){};\n"
        "};\n"
        "window.IntersectionObserver = function(cb) {\n"
        "  this.observe = function(){}; this.disconnect = function(){};\n"
        "};\n"
        "window.ResizeObserver = function(cb) {\n"
        "  this.observe = function(){}; this.disconnect = function(){};\n"
        "};\n"
        "var CustomEvent = function(type, opts) { this.type = type; };\n"
        "var Event = function(type) { this.type = type; };\n";
    JSValue sr = JS_Eval(ctx, stubs, strlen(stubs), "<stubs>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, sr);
    JS_FreeValue(ctx, global);

    /* Execute each resolved script */
    int script_count = (int)lua_rawlen(L, 2);
    int errors = 0;
    for (int i = 1; i <= script_count; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isstring(L, -1)) {
            size_t code_len;
            const char *code = lua_tolstring(L, -1, &code_len);
            if (code && code_len > 0) {
                JSValue result = JS_Eval(ctx, code, code_len,
                                          "<script>", JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(result)) {
                    JSValue exc = JS_GetException(ctx);
                    const char *msg = JS_ToCString(ctx, exc);
                    if (msg) {
                        /* Only log first 3 errors to avoid flooding serial */
                        if (errors < 3)
                            serial_printf("[qjs] script %d: %s\n", i, msg);
                        JS_FreeCString(ctx, msg);
                    }
                    JS_FreeValue(ctx, exc);
                    errors++;
                }
                JS_FreeValue(ctx, result);
            }
        }
        lua_pop(L, 1);
    }

    /* Fire DOMContentLoaded handlers */
    const char *dcl_fire =
        "for (var i = 0; i < _dcl_handlers.length; i++) {\n"
        "  try { _dcl_handlers[i]({ type: 'DOMContentLoaded' }); }\n"
        "  catch(e) { console.log('DOMContentLoaded handler error:', e); }\n"
        "}\n";
    JSValue dcl_result = JS_Eval(ctx, dcl_fire, strlen(dcl_fire),
                                  "<dcl>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, dcl_result);

    /* Clean up */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    lua_pushboolean(L, 1);
    return 1;
}

/* ── Legacy single-call API ──────────────────────────── */
/* aios.html.run_scripts(doc) — runs inline scripts only (no fetch) */

int qjs_execute_scripts(void *doc_handle) {
    /* This is the simple path: just inline scripts, no external fetch.
     * Used when the browser doesn't need external script support. */
    lxb_html_document_t *lxb_doc = html_doc_get_lxb(doc_handle);
    if (!lxb_doc) return 0;

    script_info_t scripts[MAX_SCRIPTS];
    int count = 0;
    memset(scripts, 0, sizeof(scripts));

    lxb_dom_node_t *root = lxb_dom_interface_node(lxb_doc->dom_document.element);
    if (!root) return 0;
    collect_scripts(lxb_dom_node_first_child(root), scripts, &count, MAX_SCRIPTS);

    /* Filter to inline only */
    int inline_count = 0;
    for (int i = 0; i < count; i++) {
        if (scripts[i].is_external) {
            kfree(scripts[i].data);
            scripts[i].data = NULL;
        } else {
            inline_count++;
        }
    }
    if (inline_count == 0) {
        for (int i = 0; i < count; i++) if (scripts[i].data) kfree(scripts[i].data);
        return 1;
    }

    /* Create runtime, execute inline scripts */
    JSRuntime *rt = JS_NewRuntime2(&qjs_aios_mf, NULL);
    if (!rt) {
        for (int i = 0; i < count; i++) if (scripts[i].data) kfree(scripts[i].data);
        return 0;
    }
    JS_SetMemoryLimit(rt, 16 * 1024 * 1024);
    qjs_timeout_t timeout = { .start_ticks = timer_get_ticks(),
                               .timeout_ticks = QJS_TIMEOUT_TICKS };
    JS_SetInterruptHandler(rt, qjs_interrupt_handler, &timeout);
    JS_SetMaxStackSize(rt, 512 * 1024);

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        for (int i = 0; i < count; i++) if (scripts[i].data) kfree(scripts[i].data);
        return 0;
    }

    qjs_dom_init_classes(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "document",
                       qjs_dom_wrap_document(ctx, lxb_doc));
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    qjs_dom_install_console(ctx);

    const char *stubs =
        "var navigator = { userAgent: 'AIOS/2.0' };\n"
        "function setTimeout(fn, ms) { return 0; }\n"
        "function clearTimeout(id) {}\n"
        "function setInterval(fn, ms) { return 0; }\n"
        "function clearInterval(id) {}\n"
        "document.addEventListener = function(){};\n"
        "window.addEventListener = function(){};\n"
        "document.querySelectorAll = function(sel) { return []; };\n"
        "document.querySelector = function(sel) { return null; };\n"
        "document.createElement = function(tag) {\n"
        "  return { tagName: tag, style: {}, setAttribute: function(){},\n"
        "    appendChild: function(c){ return c; }, children: [] };\n"
        "};\n";
    JSValue sr = JS_Eval(ctx, stubs, strlen(stubs), "<stubs>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, sr);
    JS_FreeValue(ctx, global);

    for (int i = 0; i < count; i++) {
        if (!scripts[i].data || scripts[i].is_external) continue;
        JSValue result = JS_Eval(ctx, scripts[i].data, scripts[i].len,
                                  "<script>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, result);
        kfree(scripts[i].data);
        scripts[i].data = NULL;
    }

    /* Cleanup remaining */
    for (int i = 0; i < count; i++) if (scripts[i].data) kfree(scripts[i].data);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 1;
}

/* ── Registration (called from lua_html.c) ───────────── */

void qjs_bridge_register(lua_State *L) {
    /* Add to existing aios.html table */
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "html");
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }

    lua_pushcfunction(L, l_html_get_scripts);
    lua_setfield(L, -2, "get_scripts");

    lua_pushcfunction(L, l_html_exec_scripts);
    lua_setfield(L, -2, "exec_scripts");

    lua_pop(L, 2);
}
