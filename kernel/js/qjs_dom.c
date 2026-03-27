/* AIOS QuickJS DOM Bridge
 * Wraps Lexbor DOM nodes as JavaScript objects so <script> tags
 * can use document.getElementById(), element.textContent, etc. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../../drivers/serial.h"

#include "quickjs.h"

/* Lexbor headers */
#include "lexbor/html/parser.h"
#include "lexbor/html/html.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/text.h"
#include "lexbor/dom/interfaces/node.h"
#include "lexbor/dom/collection.h"

/* ── JS Class IDs ────────────────────────────────────── */

static JSClassID js_element_class_id;
static JSClassID js_document_class_id;

/* ── Forward declarations ────────────────────────────── */

static JSValue js_wrap_element(JSContext *ctx, lxb_dom_element_t *el);
static JSValue js_wrap_node(JSContext *ctx, lxb_dom_node_t *node);

/* ── Element class ───────────────────────────────────── */

static void js_element_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    (void)val;
    /* We don't own the Lexbor node — it's owned by the document */
}

static lxb_dom_element_t *js_get_element(JSContext *ctx, JSValueConst this_val) {
    return (lxb_dom_element_t *)JS_GetOpaque2(ctx, this_val, js_element_class_id);
}

/* element.tagName */
static JSValue js_element_get_tagName(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_UNDEFINED;
    size_t len;
    const lxb_char_t *name = lxb_dom_element_tag_name(el, &len);
    if (!name) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, (const char *)name, len);
}

/* element.id */
static JSValue js_element_get_id(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NewString(ctx, "");
    size_t len;
    const lxb_char_t *id = lxb_dom_element_id(el, &len);
    if (!id || len == 0) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, (const char *)id, len);
}

/* element.className (get/set) */
static JSValue js_element_get_className(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NewString(ctx, "");
    size_t len;
    const lxb_char_t *val = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)"class", 5, &len);
    if (!val) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, (const char *)val, len);
}

static JSValue js_element_set_className(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_UNDEFINED;
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    lxb_dom_element_set_attribute(el, (const lxb_char_t *)"class", 5,
                                  (const lxb_char_t *)str, strlen(str));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* element.textContent (get/set) */
static JSValue js_element_get_textContent(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NewString(ctx, "");
    size_t len;
    lxb_char_t *text = lxb_dom_node_text_content(lxb_dom_interface_node(el), &len);
    if (!text) return JS_NewString(ctx, "");
    JSValue ret = JS_NewStringLen(ctx, (const char *)text, len);
    /* text is allocated by lexbor — we need to free it */
    lxb_dom_document_t *doc = lxb_dom_interface_node(el)->owner_document;
    if (doc) {
        lexbor_mraw_free(doc->mraw, text);
    }
    return ret;
}

static JSValue js_element_set_textContent(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_UNDEFINED;
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    lxb_dom_node_text_content_set(lxb_dom_interface_node(el),
                                   (const lxb_char_t *)str, strlen(str));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* element.innerHTML (get/set — uses Lexbor fragment parsing) */
static JSValue js_element_get_innerHTML(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NewString(ctx, "");
    /* Serialize child nodes to HTML string */
    /* For now, return text content as approximation */
    size_t len;
    lxb_char_t *text = lxb_dom_node_text_content(lxb_dom_interface_node(el), &len);
    if (!text) return JS_NewString(ctx, "");
    JSValue ret = JS_NewStringLen(ctx, (const char *)text, len);
    lxb_dom_document_t *doc = lxb_dom_interface_node(el)->owner_document;
    if (doc) lexbor_mraw_free(doc->mraw, text);
    return ret;
}

static JSValue js_element_set_innerHTML(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_UNDEFINED;
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;

    /* Remove all existing children */
    lxb_dom_node_t *child = lxb_dom_node_first_child(lxb_dom_interface_node(el));
    while (child) {
        lxb_dom_node_t *next = lxb_dom_node_next(child);
        lxb_dom_node_remove(child);
        child = next;
    }

    /* Parse the HTML fragment and insert under this element.
     * lxb_html_element_inner_html_set parses HTML and replaces children. */
    size_t len = strlen(str);
    lxb_html_element_t *html_el = lxb_html_interface_element(el);
    if (html_el && len > 0) {
        lxb_html_element_t *result = lxb_html_element_inner_html_set(html_el,
            (const lxb_char_t *)str, len);
        if (!result) {
            /* Fallback: set as text content */
            lxb_dom_node_text_content_set(lxb_dom_interface_node(el),
                                           (const lxb_char_t *)str, len);
        }
    }

    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* element.getAttribute(name) */
static JSValue js_element_getAttribute(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    size_t val_len;
    const lxb_char_t *val = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)name, strlen(name), &val_len);
    JS_FreeCString(ctx, name);
    if (!val) return JS_NULL;
    return JS_NewStringLen(ctx, (const char *)val, val_len);
}

/* element.setAttribute(name, value) */
static JSValue js_element_setAttribute(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (name && val) {
        lxb_dom_element_set_attribute(el,
            (const lxb_char_t *)name, strlen(name),
            (const lxb_char_t *)val, strlen(val));
    }
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

/* element.removeAttribute(name) */
static JSValue js_element_removeAttribute(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    lxb_dom_element_remove_attribute(el,
        (const lxb_char_t *)name, strlen(name));
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* element.parentNode */
static JSValue js_element_get_parentNode(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NULL;
    lxb_dom_node_t *parent = lxb_dom_node_parent(lxb_dom_interface_node(el));
    if (!parent || parent->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_NULL;
    return js_wrap_element(ctx, lxb_dom_interface_element(parent));
}

/* element.firstChild / lastChild / nextSibling / previousSibling */
static JSValue js_element_get_firstChild(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NULL;
    lxb_dom_node_t *child = lxb_dom_node_first_child(lxb_dom_interface_node(el));
    return child ? js_wrap_node(ctx, child) : JS_NULL;
}

static JSValue js_element_get_nextSibling(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NULL;
    lxb_dom_node_t *sib = lxb_dom_node_next(lxb_dom_interface_node(el));
    return sib ? js_wrap_node(ctx, sib) : JS_NULL;
}

/* element.children — returns array of child elements */
static JSValue js_element_get_children(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NewArray(ctx);
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    lxb_dom_node_t *child = lxb_dom_node_first_child(lxb_dom_interface_node(el));
    while (child) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            JS_SetPropertyUint32(ctx, arr, idx++,
                js_wrap_element(ctx, lxb_dom_interface_element(child)));
        }
        child = lxb_dom_node_next(child);
    }
    return arr;
}

/* element.appendChild(child) */
static JSValue js_element_appendChild(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el || argc < 1) return JS_UNDEFINED;
    lxb_dom_element_t *child = (lxb_dom_element_t *)JS_GetOpaque2(ctx, argv[0], js_element_class_id);
    if (!child) return JS_UNDEFINED;
    lxb_dom_node_insert_child(lxb_dom_interface_node(el),
                               lxb_dom_interface_node(child));
    return JS_DupValue(ctx, argv[0]);
}

/* element.removeChild(child) */
static JSValue js_element_removeChild(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    lxb_dom_element_t *child = (lxb_dom_element_t *)JS_GetOpaque2(ctx, argv[0], js_element_class_id);
    if (!child) return JS_UNDEFINED;
    lxb_dom_node_remove(lxb_dom_interface_node(child));
    return JS_DupValue(ctx, argv[0]);
}

/* ── CSSStyleDeclaration (exotic class) ──────────────── */
/* Proxies reads/writes to the element's inline "style" attribute.
 * element.style.display = "none" → updates style="...;display:none" on the DOM. */

static JSClassID js_style_class_id;

/* Convert JS camelCase to CSS kebab-case: backgroundColor → background-color */
static int css_prop_name(const char *js_name, char *out, int max) {
    int j = 0;
    for (int i = 0; js_name[i] && j < max - 2; i++) {
        char c = js_name[i];
        if (c >= 'A' && c <= 'Z') {
            out[j++] = '-';
            out[j++] = c + 32;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return j;
}

/* Parse a CSS property value from an inline style string */
static const char *style_get_prop(const char *style_str, const char *prop,
                                   int *out_len) {
    if (!style_str || !prop) return NULL;
    int prop_len = (int)strlen(prop);
    const char *p = style_str;
    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        /* Check if this property matches */
        if (strncmp(p, prop, prop_len) == 0) {
            const char *after = p + prop_len;
            /* Skip whitespace after property name */
            while (*after == ' ') after++;
            if (*after == ':') {
                after++;
                while (*after == ' ') after++;
                const char *val_start = after;
                /* Find end of value */
                while (*after && *after != ';') after++;
                /* Trim trailing whitespace */
                const char *val_end = after;
                while (val_end > val_start && val_end[-1] == ' ') val_end--;
                *out_len = (int)(val_end - val_start);
                return val_start;
            }
        }
        /* Skip to next semicolon */
        while (*p && *p != ';') p++;
    }
    return NULL;
}

/* Build a new style string with a property set/replaced */
static void style_set_prop(const char *old_style, const char *prop,
                            const char *value, char *out, int max) {
    int prop_len = (int)strlen(prop);
    int value_len = (int)strlen(value);
    int pos = 0;
    int replaced = 0;

    if (old_style) {
        const char *p = old_style;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == ';') { if (*p) p++; continue; }

            /* Find the end of this declaration */
            const char *decl_start = p;
            while (*p && *p != ';') p++;
            int decl_len = (int)(p - decl_start);
            if (*p == ';') p++;

            /* Check if this is the property we're setting */
            const char *d = decl_start;
            while (d < decl_start + decl_len && *d == ' ') d++;
            if (strncmp(d, prop, prop_len) == 0) {
                const char *after = d + prop_len;
                while (*after == ' ') after++;
                if (*after == ':') {
                    /* Replace this declaration */
                    if (value_len > 0) {
                        int n = snprintf(out + pos, max - pos, "%s: %s; ",
                                         prop, value);
                        if (n > 0) pos += n;
                    }
                    /* Skip old value (already consumed) */
                    replaced = 1;
                    continue;
                }
            }
            /* Copy unchanged declaration */
            if (pos + decl_len + 2 < max) {
                memcpy(out + pos, decl_start, decl_len);
                pos += decl_len;
                out[pos++] = ';';
                out[pos++] = ' ';
            }
        }
    }

    /* Append new property if not replaced */
    if (!replaced && value_len > 0) {
        int n = snprintf(out + pos, max - pos, "%s: %s", prop, value);
        if (n > 0) pos += n;
    }

    /* Trim trailing "; " */
    while (pos > 0 && (out[pos - 1] == ' ' || out[pos - 1] == ';')) pos--;
    out[pos] = '\0';
}

static JSValue js_style_get_property(JSContext *ctx, JSValueConst obj,
                                      JSAtom atom, JSValueConst receiver) {
    (void)receiver;
    lxb_dom_element_t *el = (lxb_dom_element_t *)JS_GetOpaque(obj, js_style_class_id);
    if (!el) return JS_UNDEFINED;

    const char *js_name = JS_AtomToCString(ctx, atom);
    if (!js_name) return JS_UNDEFINED;

    /* Convert camelCase to kebab-case */
    char css_name[128];
    css_prop_name(js_name, css_name, sizeof(css_name));
    JS_FreeCString(ctx, js_name);

    /* Read the style attribute */
    size_t style_len;
    const lxb_char_t *style_str = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)"style", 5, &style_len);
    if (!style_str) return JS_NewString(ctx, "");

    int val_len;
    const char *val = style_get_prop((const char *)style_str, css_name, &val_len);
    if (!val) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, val, val_len);
}

static int js_style_set_property(JSContext *ctx, JSValueConst obj,
                                  JSAtom atom, JSValueConst value,
                                  JSValueConst receiver, int flags) {
    (void)receiver;
    (void)flags;
    lxb_dom_element_t *el = (lxb_dom_element_t *)JS_GetOpaque(obj, js_style_class_id);
    if (!el) return 0;

    const char *js_name = JS_AtomToCString(ctx, atom);
    if (!js_name) return 0;

    char css_name[128];
    css_prop_name(js_name, css_name, sizeof(css_name));
    JS_FreeCString(ctx, js_name);

    const char *val_str = JS_ToCString(ctx, value);
    if (!val_str) return 0;

    /* Read old style */
    size_t old_len;
    const lxb_char_t *old_style = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)"style", 5, &old_len);

    /* Build new style */
    char new_style[1024];
    style_set_prop(old_style ? (const char *)old_style : "",
                   css_name, val_str, new_style, sizeof(new_style));
    JS_FreeCString(ctx, val_str);

    /* Write back to DOM */
    lxb_dom_element_set_attribute(el,
        (const lxb_char_t *)"style", 5,
        (const lxb_char_t *)new_style, strlen(new_style));
    return 1;
}

static JSClassExoticMethods js_style_exotic = {
    .get_property = js_style_get_property,
    .set_property = js_style_set_property,
};

static JSClassDef js_style_class = {
    "CSSStyleDeclaration",
    .exotic = &js_style_exotic,
};

static JSValue js_element_get_style(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = js_get_element(ctx, this_val);
    if (!el) return JS_NewObject(ctx);
    JSValue obj = JS_NewObjectClass(ctx, js_style_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, el);
    return obj;
}

/* ── Element function/property lists ─────────────────── */

static const JSCFunctionListEntry js_element_proto[] = {
    JS_CGETSET_DEF("tagName", js_element_get_tagName, NULL),
    JS_CGETSET_DEF("id", js_element_get_id, NULL),
    JS_CGETSET_DEF("className", js_element_get_className, js_element_set_className),
    JS_CGETSET_DEF("textContent", js_element_get_textContent, js_element_set_textContent),
    JS_CGETSET_DEF("innerHTML", js_element_get_innerHTML, js_element_set_innerHTML),
    JS_CGETSET_DEF("parentNode", js_element_get_parentNode, NULL),
    JS_CGETSET_DEF("firstChild", js_element_get_firstChild, NULL),
    JS_CGETSET_DEF("nextSibling", js_element_get_nextSibling, NULL),
    JS_CGETSET_DEF("children", js_element_get_children, NULL),
    JS_CGETSET_DEF("style", js_element_get_style, NULL),
    JS_CFUNC_DEF("getAttribute", 1, js_element_getAttribute),
    JS_CFUNC_DEF("setAttribute", 2, js_element_setAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, js_element_removeAttribute),
    JS_CFUNC_DEF("appendChild", 1, js_element_appendChild),
    JS_CFUNC_DEF("removeChild", 1, js_element_removeChild),
};

static JSClassDef js_element_class = {
    "Element",
    .finalizer = js_element_finalizer,
};

/* ── Wrap a Lexbor element as a JS Element object ────── */

static JSValue js_wrap_element(JSContext *ctx, lxb_dom_element_t *el) {
    if (!el) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, js_element_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, el);
    return obj;
}

static JSValue js_wrap_node(JSContext *ctx, lxb_dom_node_t *node) {
    if (!node) return JS_NULL;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        return js_wrap_element(ctx, lxb_dom_interface_element(node));
    }
    /* For text nodes and others, return a minimal object with textContent */
    JSValue obj = JS_NewObject(ctx);
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t len;
        lxb_char_t *text = lxb_dom_node_text_content(node, &len);
        if (text) {
            JS_SetPropertyStr(ctx, obj, "textContent",
                JS_NewStringLen(ctx, (const char *)text, len));
            lxb_dom_document_t *doc = node->owner_document;
            if (doc) lexbor_mraw_free(doc->mraw, text);
        }
        JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, 3));
    }
    return obj;
}

/* ── Document class ──────────────────────────────────── */

typedef struct {
    lxb_html_document_t *doc;
} JSDocData;

static void js_document_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    (void)val;
    /* Document is owned by html_doc_t — we don't free it */
}

static lxb_html_document_t *js_get_doc(JSContext *ctx, JSValueConst this_val) {
    JSDocData *data = (JSDocData *)JS_GetOpaque2(ctx, this_val, js_document_class_id);
    return data ? data->doc : NULL;
}

/* document.getElementById(id) */
static JSValue js_doc_getElementById(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    lxb_html_document_t *doc = js_get_doc(ctx, this_val);
    if (!doc || argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;

    lxb_dom_element_t *body = lxb_dom_interface_element(
        lxb_html_document_body_element(doc));
    if (!body) body = lxb_dom_interface_element(doc->dom_document.element);

    lxb_dom_element_t *found = lxb_dom_element_by_id(body,
        (const lxb_char_t *)id, strlen(id));
    JS_FreeCString(ctx, id);
    if (!found) return JS_NULL;
    return js_wrap_element(ctx, found);
}

/* document.getElementsByTagName(tag) */
static JSValue js_doc_getElementsByTagName(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    lxb_html_document_t *doc = js_get_doc(ctx, this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_EXCEPTION;

    lxb_dom_collection_t *col = lxb_dom_collection_make(&doc->dom_document, 16);
    if (!col) { JS_FreeCString(ctx, tag); return JS_NewArray(ctx); }

    lxb_dom_element_t *root = lxb_dom_interface_element(doc->dom_document.element);
    lxb_dom_elements_by_tag_name(root, col,
        (const lxb_char_t *)tag, strlen(tag));
    JS_FreeCString(ctx, tag);

    JSValue arr = JS_NewArray(ctx);
    size_t len = lxb_dom_collection_length(col);
    for (size_t i = 0; i < len; i++) {
        lxb_dom_element_t *el = lxb_dom_collection_element(col, i);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, js_wrap_element(ctx, el));
    }
    lxb_dom_collection_destroy(col, true);
    return arr;
}

/* document.getElementsByClassName(cls) */
static JSValue js_doc_getElementsByClassName(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
    lxb_html_document_t *doc = js_get_doc(ctx, this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    const char *cls = JS_ToCString(ctx, argv[0]);
    if (!cls) return JS_EXCEPTION;

    lxb_dom_collection_t *col = lxb_dom_collection_make(&doc->dom_document, 16);
    if (!col) { JS_FreeCString(ctx, cls); return JS_NewArray(ctx); }

    lxb_dom_element_t *root = lxb_dom_interface_element(doc->dom_document.element);
    lxb_dom_elements_by_class_name(root, col,
        (const lxb_char_t *)cls, strlen(cls));
    JS_FreeCString(ctx, cls);

    JSValue arr = JS_NewArray(ctx);
    size_t len = lxb_dom_collection_length(col);
    for (size_t i = 0; i < len; i++) {
        lxb_dom_element_t *el = lxb_dom_collection_element(col, i);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, js_wrap_element(ctx, el));
    }
    lxb_dom_collection_destroy(col, true);
    return arr;
}

/* document.body */
static JSValue js_doc_get_body(JSContext *ctx, JSValueConst this_val) {
    lxb_html_document_t *doc = js_get_doc(ctx, this_val);
    if (!doc) return JS_NULL;
    lxb_html_body_element_t *body = lxb_html_document_body_element(doc);
    if (!body) return JS_NULL;
    return js_wrap_element(ctx, lxb_dom_interface_element(body));
}

/* document.title */
static JSValue js_doc_get_title(JSContext *ctx, JSValueConst this_val) {
    lxb_html_document_t *doc = js_get_doc(ctx, this_val);
    if (!doc) return JS_NewString(ctx, "");
    const lxb_char_t *title = lxb_html_document_title(doc, NULL);
    if (!title) return JS_NewString(ctx, "");
    return JS_NewString(ctx, (const char *)title);
}

/* document.documentElement */
static JSValue js_doc_get_documentElement(JSContext *ctx, JSValueConst this_val) {
    lxb_html_document_t *doc = js_get_doc(ctx, this_val);
    if (!doc) return JS_NULL;
    lxb_dom_element_t *root = lxb_dom_interface_element(doc->dom_document.element);
    return root ? js_wrap_element(ctx, root) : JS_NULL;
}

static const JSCFunctionListEntry js_doc_proto[] = {
    JS_CFUNC_DEF("getElementById", 1, js_doc_getElementById),
    JS_CFUNC_DEF("getElementsByTagName", 1, js_doc_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName", 1, js_doc_getElementsByClassName),
    JS_CGETSET_DEF("body", js_doc_get_body, NULL),
    JS_CGETSET_DEF("title", js_doc_get_title, NULL),
    JS_CGETSET_DEF("documentElement", js_doc_get_documentElement, NULL),
};

static JSClassDef js_document_class = {
    "Document",
    .finalizer = js_document_finalizer,
};

/* ── console.log ─────────────────────────────────────── */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            serial_printf("%s%s", i ? " " : "", str);
            JS_FreeCString(ctx, str);
        }
    }
    serial_printf("\n");
    return JS_UNDEFINED;
}

/* ── Public API ──────────────────────────────────────── */

void qjs_dom_init_classes(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);

    /* Register Element class */
    JS_NewClassID(&js_element_class_id);
    JS_NewClass(rt, js_element_class_id, &js_element_class);
    JSValue el_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, el_proto, js_element_proto,
                                sizeof(js_element_proto) / sizeof(js_element_proto[0]));
    JS_SetClassProto(ctx, js_element_class_id, el_proto);

    /* Register CSSStyleDeclaration class (exotic — intercepts property get/set) */
    JS_NewClassID(&js_style_class_id);
    JS_NewClass(rt, js_style_class_id, &js_style_class);

    /* Register Document class */
    JS_NewClassID(&js_document_class_id);
    JS_NewClass(rt, js_document_class_id, &js_document_class);
    JSValue doc_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, doc_proto, js_doc_proto,
                                sizeof(js_doc_proto) / sizeof(js_doc_proto[0]));
    JS_SetClassProto(ctx, js_document_class_id, doc_proto);
}

JSValue qjs_dom_wrap_document(JSContext *ctx, lxb_html_document_t *doc) {
    JSDocData *data = (JSDocData *)js_malloc(ctx, sizeof(JSDocData));
    if (!data) return JS_NULL;
    data->doc = doc;

    JSValue obj = JS_NewObjectClass(ctx, js_document_class_id);
    if (JS_IsException(obj)) {
        js_free(ctx, data);
        return JS_NULL;
    }
    JS_SetOpaque(obj, data);
    return obj;
}

void qjs_dom_install_console(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, js_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_log, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}
