/* AIOS HTML Layout Engine
 *
 * Walks a Lexbor DOM tree and computes box positions for rendering.
 * Supports: inline CSS styles, block/inline layout, text wrapping,
 * tables, images (placeholders), form elements, background colors.
 * Uses ChaosGL text measurement for word wrapping. */

#include "html_layout.h"
#include "../heap.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

/* Lexbor headers */
#include "lexbor/html/parser.h"
#include "lexbor/html/html.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/text.h"
#include "lexbor/css/css.h"
#include "lexbor/css/stylesheet.h"
#include "lexbor/css/parser.h"
#include "lexbor/style/style.h"

/* ChaosGL text measurement */
extern int chaos_gl_text_width(const char *str);
extern int chaos_gl_font_height(int handle);

/* ═══ Document wrapper ══════════════════════════════════ */

struct html_doc {
    lxb_html_document_t *document;
    char                 title[256];
    bool                 title_extracted;
    bool                 css_initialized;
    bool                 styles_processed;
};

/* Recursively find first element with a given tag ID */
static lxb_dom_node_t *find_tag(lxb_dom_node_t *node, lxb_tag_id_t tag) {
    while (node) {
        if (lxb_dom_node_type(node) == LXB_DOM_NODE_TYPE_ELEMENT &&
            lxb_dom_node_tag_id(node) == tag) return node;
        lxb_dom_node_t *found = find_tag(lxb_dom_node_first_child(node), tag);
        if (found) return found;
        node = lxb_dom_node_next(node);
    }
    return NULL;
}

html_doc_t *html_doc_parse(const char *html, uint32_t len) {
    if (!html || len == 0) return NULL;
    html_doc_t *doc = (html_doc_t *)kmalloc(sizeof(html_doc_t));
    if (!doc) return NULL;
    memset(doc, 0, sizeof(html_doc_t));
    doc->document = lxb_html_document_create();
    if (!doc->document) { kfree(doc); return NULL; }
    if (lxb_html_document_parse(doc->document, (const lxb_char_t *)html, (size_t)len) != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc->document);
        kfree(doc); return NULL;
    }

    /* Initialize Lexbor CSS system immediately after parsing */
    if (lxb_style_init(doc->document) == LXB_STATUS_OK) {
        doc->css_initialized = true;
    }

    return doc;
}

const char *html_doc_title(html_doc_t *doc) {
    if (!doc || !doc->document) return "";
    if (doc->title_extracted) return doc->title;
    doc->title_extracted = true;
    doc->title[0] = 0;
    lxb_dom_node_t *root = lxb_dom_interface_node(doc->document);
    lxb_dom_node_t *tn = find_tag(lxb_dom_node_first_child(root), LXB_TAG_TITLE);
    if (!tn) return doc->title;
    lxb_dom_node_t *text = lxb_dom_node_first_child(tn);
    if (text && lxb_dom_node_type(text) == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_text_t *t = lxb_dom_interface_text(text);
        if (t && t->char_data.data.data) {
            size_t tl = t->char_data.data.length;
            if (tl > 255) tl = 255;
            memcpy(doc->title, t->char_data.data.data, tl);
            doc->title[tl] = 0;
        }
    }
    return doc->title;
}

void html_doc_free(html_doc_t *doc) {
    if (!doc) return;
    if (doc->document) lxb_html_document_destroy(doc->document);
    kfree(doc);
}

lxb_html_document_t *html_doc_get_lxb(html_doc_t *doc) {
    return doc ? doc->document : NULL;
}

/* ═══ Lexbor CSS Integration ═══════════════════════════ */

/* Process <style> blocks and inline styles (called before layout) */
static void css_init_document(html_doc_t *doc) {
    if (!doc || !doc->document) return;

    /* Ensure CSS system is initialized */
    if (!doc->css_initialized) {
        lxb_status_t status = lxb_style_init(doc->document);
        if (status != LXB_STATUS_OK) {
            serial_printf("[css] style init failed: %d\n", (int)status);
            return;
        }
        doc->css_initialized = true;
    }

    /* Process <style> blocks and inline styles only once */
    if (doc->styles_processed) return;
    doc->styles_processed = true;

    /* Find and process all <style> elements */
    lxb_dom_node_t *root = lxb_dom_interface_node(doc->document->dom_document.element);
    if (!root) return;

    /* Walk DOM looking for <style> tags */
    lxb_dom_node_t *node = root;
    while (node) {
        if (lxb_dom_node_type(node) == LXB_DOM_NODE_TYPE_ELEMENT &&
            lxb_dom_node_tag_id(node) == LXB_TAG_STYLE) {
            /* Extract text content */
            size_t text_len;
            lxb_char_t *text = lxb_dom_node_text_content(node, &text_len);
            if (text && text_len > 0) {
                html_doc_attach_css(doc, (const char *)text, (uint32_t)text_len);
                lexbor_mraw_free(doc->document->dom_document.mraw, text);
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

    /* Also parse inline style="" attributes for all elements */
    node = root;
    while (node) {
        if (lxb_dom_node_type(node) == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);
            size_t style_len;
            const lxb_char_t *style_attr = lxb_dom_element_get_attribute(
                el, (const lxb_char_t *)"style", 5, &style_len);
            if (style_attr && style_len > 0) {
                lxb_html_element_style_parse(lxb_html_interface_element(el),
                                              style_attr, style_len);
            }
        }
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
}

/* Attach a CSS stylesheet string to the document */
int html_doc_attach_css(html_doc_t *doc, const char *css, uint32_t len) {
    if (!doc || !doc->document || !css || len == 0) return -1;

    if (!doc->css_initialized) {
        lxb_status_t s = lxb_style_init(doc->document);
        if (s != LXB_STATUS_OK) return -1;
        doc->css_initialized = true;
    }

    lxb_css_parser_t *parser = lxb_css_parser_create();
    if (!parser) return -1;
    if (lxb_css_parser_init(parser, NULL) != LXB_STATUS_OK) {
        lxb_css_parser_destroy(parser, true);
        return -1;
    }

    lxb_css_stylesheet_t *sst = lxb_css_stylesheet_create(NULL);
    if (!sst) {
        lxb_css_parser_destroy(parser, true);
        return -1;
    }

    lxb_status_t status = lxb_css_stylesheet_parse(sst, parser,
        (const lxb_char_t *)css, (size_t)len);
    if (status != LXB_STATUS_OK) {
        lxb_css_stylesheet_destroy(sst, true);
        lxb_css_parser_destroy(parser, true);
        return -1;
    }

    /* Limit stylesheet size to prevent OOM on large CSS */
    if (len > 512 * 1024) {
        serial_printf("[css] stylesheet too large (%d bytes), skipping\n", (int)len);
        lxb_css_stylesheet_destroy(sst, true);
        lxb_css_parser_destroy(parser, true);
        return -1;
    }

    status = lxb_html_document_stylesheet_attach(doc->document, sst);
    serial_printf("[css] attach stylesheet: %d bytes, status=%d\n", (int)len, (int)status);
    lxb_css_parser_destroy(parser, true);

    if (status != LXB_STATUS_OK) {
        serial_printf("[css] attach FAILED, destroying stylesheet\n");
        lxb_css_stylesheet_destroy(sst, true);
        return -1;
    }

    return 0;
}

/* ═══ CSS Style Reading (Lexbor computed styles) ═══════ */

#include "lexbor/css/property/const.h"
#include "lexbor/css/value/const.h"
#include "lexbor/css/unit/const.h"

typedef struct {
    uint32_t color;
    uint32_t bg_color;
    int      font_size;   /* 0 = inherit */
    int      margin_top, margin_bottom, margin_left, margin_right;
    bool     margin_left_auto, margin_right_auto;
    int      padding_top, padding_right, padding_bottom, padding_left;
    int      width, height;  /* 0 = auto */
    int      max_width;      /* 0 = none */
    int      min_width;      /* 0 = none */
    int      display;        /* -1=inherit, 0=none, 1=block, 2=inline, 3=flex, 4=inline-block */
    int      text_align;     /* 0=left, 1=center, 2=right */
    int      border;         /* border width, 0=none */
    uint32_t border_color;
    int      visibility;     /* 0=visible, 1=hidden */
    int      css_float;      /* 0=none, 1=left, 2=right */
    int      css_clear;      /* 0=none, 1=left, 2=right, 3=both */
    int      overflow;       /* 0=visible, 1=hidden */
    int      position;       /* 0=static, 1=relative, 2=absolute, 3=fixed */
    int      white_space;    /* 0=normal, 1=nowrap, 2=pre */
} css_style_t;

/* Convert Lexbor color value to our 0x00RRGGBB format */
static uint32_t lexbor_color_to_rgb(const lxb_css_value_color_t *color) {
    if (!color) return 0;
    if (color->type == LXB_CSS_VALUE_COLOR) {
        /* Named color or function — check hex first */
        const lxb_css_value_color_hex_t *hex = &color->u.hex;
        return ((uint32_t)hex->rgba.r << 16) |
               ((uint32_t)hex->rgba.g << 8) |
               (uint32_t)hex->rgba.b;
    }
    return 0;
}

/* Convert Lexbor length/percentage to pixels.
 * Returns -1 for "auto" to distinguish from 0px. */
static int lexbor_length_to_px(const lxb_css_value_length_percentage_t *lp,
                                int container_w) {
    if (!lp) return 0;
    if (lp->type == LXB_CSS_VALUE__LENGTH) {
        double num = lp->u.length.num;
        /* Handle em units: treat 1em = 16px base */
        if ((int)lp->u.length.unit == (int)LXB_CSS_UNIT_EM) return (int)(num * 16.0);
        if ((int)lp->u.length.unit == (int)LXB_CSS_UNIT_REM) return (int)(num * 16.0);
        if ((int)lp->u.length.unit == (int)LXB_CSS_UNIT_PT) return (int)(num * 1.333);
        return (int)num;
    }
    if (lp->type == LXB_CSS_VALUE__PERCENTAGE) {
        return (int)(lp->u.percentage.num * container_w / 100.0);
    }
    if (lp->type == LXB_CSS_VALUE_AUTO) return -1;
    return 0;
}

/* Same as above but return 0 for auto (for properties where auto = 0) */
static int lexbor_length_to_px_noauto(const lxb_css_value_length_percentage_t *lp,
                                       int container_w) {
    int v = lexbor_length_to_px(lp, container_w);
    return (v < 0) ? 0 : v;
}

/* lexbor_is_auto removed — auto detection done inline via return value -1 */

/* Read computed styles from Lexbor's CSS engine for an element */
static void lexbor_read_style(lxb_dom_element_t *el, css_style_t *out, int viewport_w) {
    const lxb_css_rule_declaration_t *d;

    /* display — CSS display is two-component: outer (a) + inner (b)
     * display: flex → a=BLOCK, b=FLEX
     * display: block → a=BLOCK, b=FLOW
     * display: inline → a=INLINE
     * display: none → a=NONE */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"display", 7);
    if (d && d->u.display) {
        lxb_css_display_type_t outer = d->u.display->a;
        lxb_css_display_type_t inner = d->u.display->b;

        if (outer == LXB_CSS_DISPLAY_NONE)          out->display = 0;
        else if (inner == LXB_CSS_DISPLAY_FLEX)      out->display = 3;  /* flex! */
        else if (inner == LXB_CSS_DISPLAY_GRID)      out->display = 3;  /* grid → flex */
        else if (outer == LXB_CSS_DISPLAY_BLOCK)     out->display = 1;
        else if (outer == LXB_CSS_DISPLAY_INLINE)    out->display = 2;
        else if (outer == LXB_CSS_DISPLAY_TABLE)     out->display = 1;
        else if (outer == LXB_CSS_DISPLAY_LIST_ITEM) out->display = 1;

        /* inline-block: outer=inline, inner=flow-root */
        if (outer == LXB_CSS_DISPLAY_INLINE && inner == LXB_CSS_DISPLAY_FLOW_ROOT)
            out->display = 4;
        /* inline-flex */
        if (outer == LXB_CSS_DISPLAY_INLINE && inner == LXB_CSS_DISPLAY_FLEX)
            out->display = 3;
    }

    /* visibility */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"visibility", 10);
    if (d && d->u.visibility) {
        if (d->u.visibility->type == LXB_CSS_VALUE_HIDDEN)
            out->visibility = 1;
    }

    /* color */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"color", 5);
    if (d && d->u.color) {
        uint32_t c = lexbor_color_to_rgb(d->u.color);
        if (c) out->color = c;
    }

    /* background-color — also check shorthand "background" */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"background-color", 16);
    if (d && d->u.background_color) {
        uint32_t c = lexbor_color_to_rgb(d->u.background_color);
        if (c) out->bg_color = c;
    }
    if (!out->bg_color) {
        d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"background", 10);
        if (d && d->u.undef) {
            /* Background shorthand — try to extract color from it */
            const lxb_css_value_color_t *bc = (const lxb_css_value_color_t *)d->u.undef;
            uint32_t c = lexbor_color_to_rgb(bc);
            if (c) out->bg_color = c;
        }
    }

    /* font-size — also check shorthand "font" */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"font-size", 9);
    if (d && d->u.undef) {
        const lxb_css_value_length_percentage_t *fs =
            (const lxb_css_value_length_percentage_t *)d->u.undef;
        if (fs->type == LXB_CSS_VALUE__LENGTH) {
            out->font_size = (int)fs->u.length.num;
        } else if (fs->type == LXB_CSS_VALUE__PERCENTAGE) {
            out->font_size = (int)(fs->u.percentage.num * 16.0 / 100.0);
        }
    }

    /* margin — check shorthand first, then longhand overrides */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"margin", 6);
    if (d && d->u.margin) {
        out->margin_top = lexbor_length_to_px_noauto(&d->u.margin->top, viewport_w);
        out->margin_bottom = lexbor_length_to_px_noauto(&d->u.margin->bottom, viewport_w);
        int ml = lexbor_length_to_px(&d->u.margin->left, viewport_w);
        int mr = lexbor_length_to_px(&d->u.margin->right, viewport_w);
        out->margin_left_auto = (ml == -1);
        out->margin_right_auto = (mr == -1);
        out->margin_left = (ml > 0) ? ml : 0;
        out->margin_right = (mr > 0) ? mr : 0;
    }
    /* Longhand overrides shorthand */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"margin-top", 10);
    if (d && d->u.margin_top) out->margin_top = lexbor_length_to_px_noauto(d->u.margin_top, viewport_w);
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"margin-bottom", 13);
    if (d && d->u.margin_bottom) out->margin_bottom = lexbor_length_to_px_noauto(d->u.margin_bottom, viewport_w);
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"margin-left", 11);
    if (d && d->u.margin_left) {
        int v = lexbor_length_to_px(d->u.margin_left, viewport_w);
        out->margin_left_auto = (v == -1);
        out->margin_left = (v > 0) ? v : 0;
    }
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"margin-right", 12);
    if (d && d->u.margin_right) {
        int v = lexbor_length_to_px(d->u.margin_right, viewport_w);
        out->margin_right_auto = (v == -1);
        out->margin_right = (v > 0) ? v : 0;
    }

    /* padding — check shorthand first, then longhand overrides */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"padding", 7);
    if (d && d->u.padding) {
        out->padding_top = lexbor_length_to_px_noauto(&d->u.padding->top, viewport_w);
        out->padding_right = lexbor_length_to_px_noauto(&d->u.padding->right, viewport_w);
        out->padding_bottom = lexbor_length_to_px_noauto(&d->u.padding->bottom, viewport_w);
        out->padding_left = lexbor_length_to_px_noauto(&d->u.padding->left, viewport_w);
    }
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"padding-top", 11);
    if (d && d->u.padding_top) out->padding_top = lexbor_length_to_px_noauto(d->u.padding_top, viewport_w);
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"padding-right", 13);
    if (d && d->u.padding_right) out->padding_right = lexbor_length_to_px_noauto(d->u.padding_right, viewport_w);
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"padding-bottom", 14);
    if (d && d->u.padding_bottom) out->padding_bottom = lexbor_length_to_px_noauto(d->u.padding_bottom, viewport_w);
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"padding-left", 12);
    if (d && d->u.padding_left) out->padding_left = lexbor_length_to_px_noauto(d->u.padding_left, viewport_w);

    /* width / height */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"width", 5);
    if (d && d->u.width) {
        out->width = lexbor_length_to_px_noauto(d->u.width, viewport_w);
    }
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"height", 6);
    if (d && d->u.height) {
        out->height = lexbor_length_to_px_noauto(d->u.height, viewport_w);
    }

    /* text-align */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"text-align", 10);
    if (d && d->u.text_align) {
        /* style hit */
        lxb_css_value_type_t ta = d->u.text_align->type;
        if (ta == LXB_CSS_VALUE_CENTER) out->text_align = 1;
        else if (ta == LXB_CSS_VALUE_RIGHT || ta == LXB_CSS_VALUE_END) out->text_align = 2;
    }

    /* max-width */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"max-width", 9);
    if (d && d->u.undef) {
        const lxb_css_value_length_percentage_t *mw =
            (const lxb_css_value_length_percentage_t *)d->u.undef;
        int v = lexbor_length_to_px(mw, viewport_w);
        out->max_width = (v > 0) ? v : 0;
    }

    /* min-width */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"min-width", 9);
    if (d && d->u.undef) {
        const lxb_css_value_length_percentage_t *mw =
            (const lxb_css_value_length_percentage_t *)d->u.undef;
        int v = lexbor_length_to_px(mw, viewport_w);
        out->min_width = (v > 0) ? v : 0;
    }

    /* border — check shorthand */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"border", 6);
    if (d && d->u.border) {
        /* Try to read border width from the shorthand struct */
        /* border shorthand has top/right/bottom/left sub-properties */
        /* For now, just mark that border exists */
        out->border = 1;
        out->border_color = 0x00CCCCCC;
    }
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"border-top", 10);
    if (d) { out->border = 1; out->border_color = 0x00CCCCCC; }

    /* float — union field is "floatp" (not "float" which is a C keyword) */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"float", 5);
    if (d && d->u.floatp) {
        if (d->u.floatp->type == LXB_CSS_FLOAT_LEFT) out->css_float = 1;
        else if (d->u.floatp->type == LXB_CSS_FLOAT_RIGHT) out->css_float = 2;
        /* style hit */
    }

    /* clear */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"clear", 5);
    if (d && d->u.clear) {
        if (d->u.clear->type == LXB_CSS_VALUE_LEFT) out->css_clear = 1;
        else if (d->u.clear->type == LXB_CSS_VALUE_RIGHT) out->css_clear = 2;
        else if (d->u.clear->type == LXB_CSS_VALUE_BOTH) out->css_clear = 3;
    }

    /* white-space */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"white-space", 11);
    if (d && d->u.undef) {
        lxb_css_value_type_t ws = *(lxb_css_value_type_t *)d->u.undef;
        if (ws == LXB_CSS_VALUE_NOWRAP) out->white_space = 1;
        else if (ws == LXB_CSS_VALUE_PRE) out->white_space = 2;
    }

    /* overflow-x (overflow shorthand may set overflow-x) */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"overflow-x", 10);
    if (d && d->u.overflow_x) {
        if (d->u.overflow_x->type == LXB_CSS_VALUE_HIDDEN) out->overflow = 1;
    }

    /* position */
    d = lxb_dom_element_style_by_name(el, (const lxb_char_t *)"position", 8);
    if (d && d->u.position) {
        if (d->u.position->type == LXB_CSS_VALUE_RELATIVE) out->position = 1;
        else if (d->u.position->type == LXB_CSS_VALUE_ABSOLUTE) out->position = 2;
        else if (d->u.position->type == LXB_CSS_VALUE_FIXED) out->position = 3;
    }

}

/* Keep parse_color for bgcolor attribute and tag defaults */
static int ci_match(const char *s, const char *m) {
    while (*m) {
        char a = *s, b = *m;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return 0;
        s++; m++;
    }
    return 1;
}

static uint32_t parse_color(const char *s, int len) {
    if (len <= 0 || !s) return 0;
    /* Skip whitespace */
    while (len > 0 && (*s == ' ' || *s == '\t')) { s++; len--; }
    if (len <= 0) return 0;

    if (s[0] == '#') {
        uint32_t v = 0;
        int digits = 0;
        for (int i = 1; i < len; i++) {
            char c = s[i];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | d;
            digits++;
        }
        if (digits == 3) {
            uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
            return ((r | (r << 4)) << 16) | ((g | (g << 4)) << 8) | (b | (b << 4));
        }
        return v & 0x00FFFFFF;
    }
    /* rgb(r,g,b) */
    if (len >= 10 && s[0] == 'r' && s[1] == 'g' && s[2] == 'b' && s[3] == '(') {
        int r = 0, g = 0, b = 0;
        const char *p = s + 4;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; }
        while (*p == ' ' || *p == ',') p++;
        while (*p >= '0' && *p <= '9') { g = g * 10 + (*p - '0'); p++; }
        while (*p == ' ' || *p == ',') p++;
        while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; }
        return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    }
    /* Named colors */
    if (ci_match(s, "white")) return 0x00FFFFFF;
    if (ci_match(s, "black")) return 0x00000001; /* use 1 to distinguish from "not set" */
    if (ci_match(s, "red")) return 0x00FF0000;
    if (ci_match(s, "green")) return 0x00008000;
    if (ci_match(s, "blue")) return 0x000000FF;
    if (ci_match(s, "yellow")) return 0x00FFFF00;
    if (ci_match(s, "orange")) return 0x00FF8800;
    if (ci_match(s, "gray") || ci_match(s, "grey")) return 0x00808080;
    if (ci_match(s, "lightgray") || ci_match(s, "lightgrey")) return 0x00D3D3D3;
    if (ci_match(s, "darkgray") || ci_match(s, "darkgrey")) return 0x00A9A9A9;
    if (ci_match(s, "navy")) return 0x00000080;
    if (ci_match(s, "teal")) return 0x00008080;
    if (ci_match(s, "maroon")) return 0x00800000;
    if (ci_match(s, "purple")) return 0x00800080;
    if (ci_match(s, "silver")) return 0x00C0C0C0;
    if (ci_match(s, "aqua") || ci_match(s, "cyan")) return 0x0000FFFF;
    if (ci_match(s, "lime")) return 0x0000FF00;
    if (ci_match(s, "transparent")) return 0;
    return 0;
}

static int parse_px(const char *s, int len) {
    if (len <= 0 || !s) return 0;
    while (len > 0 && *s == ' ') { s++; len--; }
    int val = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; len--; }
    while (len > 0 && *s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; len--; }
    return neg ? -val : val;
}

/* Old CSS parser removed — all styles now come from Lexbor's CSS engine */

/* ═══ Layout Context ════════════════════════════════════ */

#define MAX_BOXES 4096
#define DEFAULT_FG 0x00222222
#define LINK_COLOR 0x000066CC

/* Float state: tracks left/right floated elements */
#define MAX_FLOATS 32
typedef struct {
    int x, y, w, h;   /* position and size of float */
    int side;          /* 1=left, 2=right */
} float_box_t;

typedef struct {
    html_layout_t *layout;
    int viewport_w;      /* available width for content */
    int container_w;     /* width of containing block */
    int cx, cy;
    int line_h;
    int indent;
    uint32_t color;
    uint8_t  style_flags;
    const char *href;
    int href_len;
    int text_align;      /* 0=left, 1=center, 2=right — inherited */
    int white_space;     /* 0=normal, 1=nowrap, 2=pre — inherited */

    /* Float tracking */
    float_box_t floats[MAX_FLOATS];
    int float_count;
    int float_left_w;    /* width consumed by left floats at current Y */
    int float_right_w;   /* width consumed by right floats at current Y */
} layout_ctx_t;

/* Update float margins for current Y position */
static void update_float_clearance(layout_ctx_t *ctx) {
    ctx->float_left_w = 0;
    ctx->float_right_w = 0;
    for (int i = 0; i < ctx->float_count; i++) {
        float_box_t *f = &ctx->floats[i];
        /* Is this float still active at current Y? */
        if (ctx->cy >= f->y && ctx->cy < f->y + f->h) {
            if (f->side == 1) {
                int right_edge = f->x + f->w;
                if (right_edge > ctx->float_left_w)
                    ctx->float_left_w = right_edge;
            } else {
                if (f->w > ctx->float_right_w)
                    ctx->float_right_w = f->w;
            }
        }
    }
}

static void emit(layout_ctx_t *ctx, int type, int x, int y, int w, int h,
                 const char *text, uint32_t fg, uint32_t bg,
                 const char *url, int url_len, uint8_t flags) {
    html_layout_t *ly = ctx->layout;
    if (ly->count >= ly->capacity) return;
    html_box_t *b = &ly->boxes[ly->count++];
    memset(b, 0, sizeof(html_box_t));
    b->type = type; b->x = x; b->y = y; b->w = w; b->h = h;
    b->fg_color = fg; b->bg_color = bg; b->style_flags = flags;
    if (text) { uint32_t tl = strlen(text); if (tl > 511) tl = 511; memcpy(b->text, text, tl); b->text[tl] = 0; }
    if (url && url_len > 0) { int ul = url_len > 255 ? 255 : url_len; memcpy(b->url, url, ul); b->url[ul] = 0; }
}

/* Word-wrap text and emit text boxes */
static void layout_text(layout_ctx_t *ctx, const char *text, uint32_t len) {
    if (!text || len == 0) return;
    int fh = chaos_gl_font_height(-1);
    if (fh < 8) fh = 14;
    if (ctx->line_h < fh) ctx->line_h = fh;

    uint32_t i = 0;
    while (i < len) {
        if (ctx->cx == ctx->indent) while (i < len && text[i] == ' ') i++;
        if (i >= len) break;
        uint32_t ws = i;
        while (i < len && text[i] != ' ' && text[i] != '\n') i++;
        uint32_t wlen = i - ws;
        if (wlen == 0) { if (i < len) i++; continue; }
        char word[512];
        if (wlen > 511) wlen = 511;
        memcpy(word, text + ws, wlen); word[wlen] = 0;
        int ww = chaos_gl_text_width(word);
        if (ctx->cx + ww > ctx->viewport_w && ctx->cx > ctx->indent) {
            ctx->cx = ctx->indent; ctx->cy += ctx->line_h; ctx->line_h = fh;
        }
        int tx = ctx->cx;
        /* Text alignment: on first word of line, offset by available space */
        if (ctx->text_align && ctx->cx == ctx->indent) {
            /* Quick estimate: measure remaining text width on this line */
            int line_w = ww;
            uint32_t look = i;
            while (look < len && text[look] != '\n') {
                while (look < len && text[look] == ' ') { line_w += 6; look++; }
                uint32_t lws = look;
                while (look < len && text[look] != ' ' && text[look] != '\n') look++;
                if (look > lws) line_w += (int)(look - lws) * 7; /* ~7px per char estimate */
            }
            int avail = ctx->viewport_w - ctx->indent;
            if (ctx->text_align == 1) tx = ctx->indent + (avail - line_w) / 2;
            else if (ctx->text_align == 2) tx = ctx->indent + avail - line_w;
            if (tx < ctx->indent) tx = ctx->indent;
        }
        emit(ctx, HTML_BOX_TEXT, tx, ctx->cy, ww, fh, word,
             ctx->color, 0, ctx->href, ctx->href_len, ctx->style_flags);
        ctx->cx += ww;
        if (i < len && text[i] == ' ') { ctx->cx += chaos_gl_text_width(" "); i++; }
        if (i < len && text[i] == '\n') { ctx->cx = ctx->indent; ctx->cy += ctx->line_h; ctx->line_h = fh; i++; }
    }
}

/* ═══ Tag classification ════════════════════════════════ */

static bool is_block_tag(lxb_tag_id_t tag) {
    switch (tag) {
    case LXB_TAG_DIV: case LXB_TAG_P: case LXB_TAG_H1: case LXB_TAG_H2:
    case LXB_TAG_H3: case LXB_TAG_H4: case LXB_TAG_H5: case LXB_TAG_H6:
    case LXB_TAG_UL: case LXB_TAG_OL: case LXB_TAG_LI:
    case LXB_TAG_BLOCKQUOTE: case LXB_TAG_PRE:
    case LXB_TAG_HR: case LXB_TAG_BR:
    case LXB_TAG_TABLE: case LXB_TAG_TR: case LXB_TAG_TD: case LXB_TAG_TH:
    case LXB_TAG_HEADER: case LXB_TAG_FOOTER: case LXB_TAG_NAV:
    case LXB_TAG_SECTION: case LXB_TAG_ARTICLE: case LXB_TAG_MAIN:
    case LXB_TAG_FORM: case LXB_TAG_DL: case LXB_TAG_DT: case LXB_TAG_DD:
    case LXB_TAG_FIGURE: case LXB_TAG_FIGCAPTION:
        return true;
    default: return false;
    }
}

static bool is_skip_tag(lxb_tag_id_t tag) {
    return tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE ||
           tag == LXB_TAG_HEAD || tag == LXB_TAG_META ||
           tag == LXB_TAG_LINK || tag == LXB_TAG_TITLE ||
           tag == LXB_TAG_NOSCRIPT;
}

/* Tag CSS defaults */
static void tag_defaults(lxb_tag_id_t tag, css_style_t *s) {
    memset(s, 0, sizeof(css_style_t));
    s->display = -1;

    switch (tag) {
    case LXB_TAG_H1: s->font_size = 28; s->margin_top = 16; s->margin_bottom = 12; break;
    case LXB_TAG_H2: s->font_size = 22; s->margin_top = 14; s->margin_bottom = 10; break;
    case LXB_TAG_H3: s->font_size = 18; s->margin_top = 12; s->margin_bottom = 8; break;
    case LXB_TAG_H4: s->font_size = 16; s->margin_top = 10; s->margin_bottom = 6; break;
    case LXB_TAG_H5: s->font_size = 14; s->margin_top = 8; s->margin_bottom = 4; break;
    case LXB_TAG_H6: s->font_size = 12; s->margin_top = 6; s->margin_bottom = 4; break;
    case LXB_TAG_P: s->margin_top = 8; s->margin_bottom = 8; break;
    case LXB_TAG_A: s->color = LINK_COLOR; break;
    case LXB_TAG_CODE: s->bg_color = 0x00E8E8E8; break;
    case LXB_TAG_PRE: s->bg_color = 0x00F0F0F0; s->padding_top = s->padding_right = s->padding_bottom = s->padding_left = 8; s->margin_top = 8; s->margin_bottom = 8; s->white_space = 2; break;
    case LXB_TAG_UL: case LXB_TAG_OL: s->margin_top = 4; s->margin_bottom = 4; s->margin_left = 8; break;
    case LXB_TAG_LI: s->margin_top = 2; s->margin_bottom = 2; s->margin_left = 20; break;
    case LXB_TAG_BLOCKQUOTE: s->margin_top = 8; s->margin_bottom = 8; s->margin_left = 20;
                             s->padding_top = s->padding_right = s->padding_bottom = s->padding_left = 8;
                             s->bg_color = 0x00F8F8F8; s->border = 3; s->border_color = 0x00CCCCCC; break;
    case LXB_TAG_HR: s->margin_top = 8; s->margin_bottom = 8; break;
    case LXB_TAG_TD: case LXB_TAG_TH: s->padding_top = s->padding_right = s->padding_bottom = s->padding_left = 4; s->border = 1; s->border_color = 0x00DDDDDD; break;
    case LXB_TAG_TABLE: s->margin_top = 8; s->margin_bottom = 8; break;
    case LXB_TAG_DD: s->margin_left = 30; break;
    case LXB_TAG_DT: s->margin_top = 4; break;
    default: break;
    }
}

/* Style flags from tag */
static uint8_t tag_style_flags(lxb_tag_id_t tag) {
    switch (tag) {
    case LXB_TAG_H1: case LXB_TAG_H2: case LXB_TAG_H3:
    case LXB_TAG_H4: case LXB_TAG_H5: case LXB_TAG_H6:
    case LXB_TAG_B: case LXB_TAG_STRONG: case LXB_TAG_TH: return HTML_BOLD;
    case LXB_TAG_I: case LXB_TAG_EM: return HTML_ITALIC;
    case LXB_TAG_U: case LXB_TAG_A: return HTML_UNDERLINE;
    case LXB_TAG_CODE: case LXB_TAG_PRE: return HTML_MONOSPACE;
    default: return 0;
    }
}

/* ═══ DOM traversal + layout ════════════════════════════ */

static void layout_node(layout_ctx_t *ctx, lxb_dom_node_t *node);

static void layout_children(layout_ctx_t *ctx, lxb_dom_node_t *parent) {
    lxb_dom_node_t *child = lxb_dom_node_first_child(parent);
    while (child) { layout_node(ctx, child); child = lxb_dom_node_next(child); }
}

/* ═══ Flex Layout — arrange children horizontally ══════ */

static void layout_flex_row(layout_ctx_t *ctx, lxb_dom_node_t *node, int container_w) {
    int fh = chaos_gl_font_height(-1);
    if (fh < 8) fh = 14;

    /* Phase 1: count children and their widths */
    int child_count = 0;
    int fixed_width = 0;  /* total width of children with explicit width */
    int flex_count = 0;   /* children needing flexible width */

    lxb_dom_node_t *ch = lxb_dom_node_first_child(node);
    while (ch) {
        if (lxb_dom_node_type(ch) == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_tag_id_t ctag = lxb_dom_node_tag_id(ch);
            if (ctag != LXB_TAG_SCRIPT && ctag != LXB_TAG_STYLE) {
                child_count++;
                /* Read child's width */
                lxb_dom_element_t *cel = lxb_dom_interface_element(ch);
                css_style_t cs;
                tag_defaults(ctag, &cs);
                lexbor_read_style(cel, &cs, container_w);
                if (cs.display == 0) { ch = lxb_dom_node_next(ch); continue; }
                int cw = cs.width;
                if (cs.max_width > 0 && cw > cs.max_width) cw = cs.max_width;
                if (cw > 0) {
                    fixed_width += cw + cs.margin_left + cs.margin_right;
                } else {
                    flex_count++;
                }
            }
        }
        ch = lxb_dom_node_next(ch);
    }

    if (child_count == 0) return;

    /* Phase 2: compute flex child width */
    int remaining = container_w - fixed_width;
    if (remaining < 0) remaining = 0;
    int flex_w = (flex_count > 0) ? remaining / flex_count : 0;
    if (flex_w < 20) flex_w = 20;

    /* Phase 3: layout each child in its column */
    int start_y = ctx->cy;
    int max_h = 0;
    int saved_indent = ctx->indent;
    int saved_vw = ctx->viewport_w;
    int saved_cw = ctx->container_w;

    ch = lxb_dom_node_first_child(node);
    while (ch) {
        if (lxb_dom_node_type(ch) == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_tag_id_t ctag = lxb_dom_node_tag_id(ch);
            if (ctag != LXB_TAG_SCRIPT && ctag != LXB_TAG_STYLE) {
                lxb_dom_element_t *cel = lxb_dom_interface_element(ch);
                css_style_t cs;
                tag_defaults(ctag, &cs);
                lexbor_read_style(cel, &cs, container_w);
                if (cs.display == 0) { ch = lxb_dom_node_next(ch); continue; }

                int cw = cs.width;
                if (cs.max_width > 0 && (cw == 0 || cw > cs.max_width)) cw = cs.max_width;
                if (cw <= 0) cw = flex_w;
                cw -= cs.margin_left + cs.margin_right;
                if (cw < 10) cw = 10;

                /* Set up child's viewport */
                int child_x = ctx->cx + cs.margin_left;
                ctx->cx = child_x + cs.padding_left;
                ctx->cy = start_y + cs.padding_top + cs.margin_top;
                ctx->indent = ctx->cx;
                ctx->viewport_w = child_x + cw - cs.padding_right;
                ctx->container_w = cw;
                ctx->line_h = fh;

                /* Background */
                if (cs.bg_color) {
                    emit(ctx, HTML_BOX_RECT, child_x, start_y + cs.margin_top,
                         cw, 0, NULL, 0, cs.bg_color, NULL, 0, 0);
                }

                layout_children(ctx, ch);
                ctx->cy += ctx->line_h + cs.padding_bottom + cs.margin_bottom;

                int child_h = ctx->cy - start_y;
                if (child_h > max_h) max_h = child_h;

                /* Advance X for next flex child */
                ctx->cx = child_x + cw + cs.margin_right;
            }
        }
        ch = lxb_dom_node_next(ch);
    }

    /* Restore and advance Y past tallest child */
    ctx->indent = saved_indent;
    ctx->viewport_w = saved_vw;
    ctx->container_w = saved_cw;
    ctx->cx = saved_indent;
    ctx->cy = start_y + max_h;
    ctx->line_h = fh;
}

/* Count direct child elements with a specific tag */
static int count_children_tag(lxb_dom_node_t *parent, lxb_tag_id_t tag) {
    int count = 0;
    lxb_dom_node_t *ch = lxb_dom_node_first_child(parent);
    while (ch) {
        if (lxb_dom_node_type(ch) == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_node_tag_id(ch) == tag)
            count++;
        ch = lxb_dom_node_next(ch);
    }
    return count;
}

/* Layout a table */
static void layout_table(layout_ctx_t *ctx, lxb_dom_node_t *table_node, css_style_t *ts) {
    int fh = chaos_gl_font_height(-1);
    if (fh < 8) fh = 14;
    int table_w = ctx->viewport_w - ctx->indent * 2;
    if (ts->width > 0) table_w = ts->width;

    /* Count columns (from first TR) */
    int max_cols = 0;
    lxb_dom_node_t *child = lxb_dom_node_first_child(table_node);
    while (child) {
        lxb_tag_id_t ctag = lxb_dom_node_tag_id(child);
        if (ctag == LXB_TAG_TR || ctag == LXB_TAG_THEAD || ctag == LXB_TAG_TBODY) {
            lxb_dom_node_t *row = (ctag == LXB_TAG_TR) ? child : lxb_dom_node_first_child(child);
            while (row) {
                if (lxb_dom_node_tag_id(row) == LXB_TAG_TR) {
                    int cols = count_children_tag(row, LXB_TAG_TD) + count_children_tag(row, LXB_TAG_TH);
                    if (cols > max_cols) max_cols = cols;
                }
                row = (ctag == LXB_TAG_TR) ? NULL : lxb_dom_node_next(row);
            }
        }
        child = lxb_dom_node_next(child);
    }
    if (max_cols == 0) max_cols = 1;
    int col_w = table_w / max_cols;

    /* Table border */
    if (ts->border > 0) {
        emit(ctx, HTML_BOX_RECT, ctx->indent, ctx->cy, table_w, 1,
             NULL, 0, ts->border_color, NULL, 0, 0);
    }

    /* Layout rows */
    child = lxb_dom_node_first_child(table_node);
    while (child) {
        lxb_tag_id_t ctag = lxb_dom_node_tag_id(child);
        lxb_dom_node_t *row_start = child;
        if (ctag == LXB_TAG_THEAD || ctag == LXB_TAG_TBODY || ctag == LXB_TAG_TFOOT)
            row_start = lxb_dom_node_first_child(child);

        lxb_dom_node_t *row = row_start;
        while (row) {
            if (lxb_dom_node_tag_id(row) == LXB_TAG_TR) {
                int col = 0;
                int row_start_y = ctx->cy;
                int row_max_h = fh + 8;

                lxb_dom_node_t *cell = lxb_dom_node_first_child(row);
                while (cell) {
                    lxb_tag_id_t cell_tag = lxb_dom_node_tag_id(cell);
                    if (cell_tag == LXB_TAG_TD || cell_tag == LXB_TAG_TH) {
                        /* Cell border */
                        emit(ctx, HTML_BOX_RECT, ctx->indent + col * col_w, row_start_y,
                             col_w, 0, NULL, 0, 0x00F8F8F8, NULL, 0, 0);

                        /* Layout cell content */
                        int saved_cx = ctx->cx, saved_cy = ctx->cy;
                        int saved_indent = ctx->indent, saved_vw = ctx->viewport_w;

                        ctx->cx = ctx->indent + col * col_w + 4;
                        ctx->cy = row_start_y + 4;
                        ctx->indent = ctx->cx;
                        ctx->viewport_w = ctx->indent + col_w - 8;
                        ctx->line_h = fh;

                        if (cell_tag == LXB_TAG_TH) ctx->style_flags |= HTML_BOLD;
                        layout_children(ctx, cell);
                        if (cell_tag == LXB_TAG_TH) ctx->style_flags &= ~HTML_BOLD;

                        int cell_h = ctx->cy + ctx->line_h - row_start_y + 4;
                        if (cell_h > row_max_h) row_max_h = cell_h;

                        ctx->cx = saved_cx; ctx->cy = saved_cy;
                        ctx->indent = saved_indent; ctx->viewport_w = saved_vw;
                        col++;
                    }
                    cell = lxb_dom_node_next(cell);
                }

                /* Cell borders (draw after to get correct height) */
                for (int c = 0; c < col; c++) {
                    emit(ctx, HTML_BOX_RECT, ctx->indent + c * col_w, row_start_y,
                         col_w, row_max_h, NULL, 0, 0, NULL, 0, 0);
                    /* Cell border lines */
                    emit(ctx, HTML_BOX_HR, ctx->indent + c * col_w, row_start_y + row_max_h - 1,
                         col_w, 1, NULL, 0x00DDDDDD, 0, NULL, 0, 0);
                    if (c > 0) {
                        emit(ctx, HTML_BOX_HR, ctx->indent + c * col_w, row_start_y,
                             1, row_max_h, NULL, 0x00DDDDDD, 0, NULL, 0, 0);
                    }
                }
                ctx->cy = row_start_y + row_max_h;
            }
            row = (ctag == LXB_TAG_TR) ? NULL : lxb_dom_node_next(row);
        }
        child = lxb_dom_node_next(child);
    }
}

static void layout_node(layout_ctx_t *ctx, lxb_dom_node_t *node) {
    if (!node) return;
    int fh = chaos_gl_font_height(-1);
    if (fh < 8) fh = 14;

    /* Text node */
    if (lxb_dom_node_type(node) == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_text_t *text = lxb_dom_interface_text(node);
        if (text && text->char_data.data.data)
            layout_text(ctx, (const char *)text->char_data.data.data, (uint32_t)text->char_data.data.length);
        return;
    }
    if (lxb_dom_node_type(node) != LXB_DOM_NODE_TYPE_ELEMENT) return;

    lxb_tag_id_t tag = lxb_dom_node_tag_id(node);
    if (is_skip_tag(tag)) return;

    /* Save state */
    uint32_t saved_color = ctx->color;
    uint8_t saved_flags = ctx->style_flags;
    const char *saved_href = ctx->href;
    int saved_href_len = ctx->href_len;
    int saved_indent = ctx->indent;
    int saved_text_align = ctx->text_align;
    int saved_white_space = ctx->white_space;

    /* Tag defaults */
    css_style_t style;
    tag_defaults(tag, &style);
    uint8_t extra_flags = tag_style_flags(tag);

    /* Read computed styles from Lexbor CSS engine (handles cascade, specificity,
     * <style> blocks, inline style="" attributes, and external stylesheets) */
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    lexbor_read_style(el, &style, ctx->viewport_w);

    /* Also check bgcolor attribute (old-style HTML) */
    size_t bgcolor_len;
    const lxb_char_t *bgcolor_attr = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"bgcolor", 7, &bgcolor_len);
    if (bgcolor_attr && !style.bg_color) style.bg_color = parse_color((const char *)bgcolor_attr, (int)bgcolor_len);

    /* Display none or visibility hidden */
    if (style.display == 0) goto restore;
    if (style.visibility == 1) goto restore;

    /* Layout properties logged during development — now silent */

    /* Apply styles */
    if (style.color) ctx->color = style.color;
    ctx->style_flags |= extra_flags;
    ctx->indent += style.margin_left;
    if (style.text_align) ctx->text_align = style.text_align;
    if (style.white_space) ctx->white_space = style.white_space;

    /* Links */
    if (tag == LXB_TAG_A) {
        size_t href_len;
        const lxb_char_t *href = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"href", 4, &href_len);
        if (href) { ctx->href = (const char *)href; ctx->href_len = (int)href_len; }
    }

    /* Image */
    if (tag == LXB_TAG_IMG) {
        size_t src_len, alt_len;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &src_len);
        const lxb_char_t *alt = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"alt", 3, &alt_len);
        int img_w = style.width > 0 ? style.width : 200;
        int img_h = style.height > 0 ? style.height : 40;
        /* Check width/height attributes */
        size_t aw_len, ah_len;
        const lxb_char_t *aw = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"width", 5, &aw_len);
        const lxb_char_t *ah = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"height", 6, &ah_len);
        if (aw) img_w = parse_px((const char *)aw, (int)aw_len);
        if (ah) img_h = parse_px((const char *)ah, (int)ah_len);
        if (img_w > ctx->viewport_w - ctx->indent) img_w = ctx->viewport_w - ctx->indent;
        if (ctx->cx + img_w > ctx->viewport_w && ctx->cx > ctx->indent) {
            ctx->cx = ctx->indent; ctx->cy += ctx->line_h; ctx->line_h = fh;
        }
        const char *label = (alt && alt_len > 0) ? (const char *)alt : "[IMG]";
        emit(ctx, HTML_BOX_IMAGE, ctx->cx, ctx->cy, img_w, img_h, label, 0x00888888, 0x00EEEEEE,
             src ? (const char *)src : NULL, src ? (int)src_len : 0, 0);
        ctx->cx += img_w + 4;
        if (ctx->line_h < img_h) ctx->line_h = img_h;
        goto restore;
    }

    /* Form input */
    if (tag == LXB_TAG_INPUT) {
        size_t type_len, val_len, ph_len;
        const lxb_char_t *type = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &type_len);
        const lxb_char_t *val = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"value", 5, &val_len);
        const lxb_char_t *ph = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"placeholder", 11, &ph_len);
        bool is_submit = type && type_len == 6 && ci_match((const char *)type, "submit");
        bool is_hidden = type && type_len == 6 && ci_match((const char *)type, "hidden");
        if (is_hidden) goto restore;
        /* Copy label to null-terminated buffer for text measurement */
        char label_buf[256];
        label_buf[0] = 0;
        if (val && val_len > 0) { int cl = val_len > 255 ? 255 : (int)val_len; memcpy(label_buf, val, cl); label_buf[cl] = 0; }
        else if (ph && ph_len > 0) { int cl = ph_len > 255 ? 255 : (int)ph_len; memcpy(label_buf, ph, cl); label_buf[cl] = 0; }
        int iw, ih = 26;
        if (is_submit) {
            iw = label_buf[0] ? chaos_gl_text_width(label_buf) + 20 : 80;
        } else {
            /* Check size attribute */
            size_t sz_len;
            const lxb_char_t *sz = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"size", 4, &sz_len);
            iw = sz ? parse_px((const char *)sz, (int)sz_len) * 8 : 160;
            if (style.width > 0) iw = style.width;
        }
        if (iw < 20) iw = 20;
        uint32_t ibg = is_submit ? 0x00E0E0E0 : 0x00FFFFFF;
        uint32_t ifg = is_submit ? 0x00222222 : 0x00999999;
        /* Wrap to next line if needed */
        if (ctx->cx + iw > ctx->viewport_w && ctx->cx > ctx->indent) {
            ctx->cx = ctx->indent; ctx->cy += ctx->line_h; ctx->line_h = fh;
        }
        emit(ctx, HTML_BOX_RECT, ctx->cx, ctx->cy, iw, ih, NULL, 0x00CCCCCC, ibg, NULL, 0, 0);
        if (label_buf[0]) emit(ctx, HTML_BOX_TEXT, ctx->cx + 6, ctx->cy + 6, iw - 12, fh, label_buf, ifg, 0, NULL, 0, 0);
        ctx->cx += iw + 6;
        if (ctx->line_h < ih) ctx->line_h = ih;
        goto restore;
    }

    /* Button */
    if (tag == LXB_TAG_BUTTON) {
        const char *btn_text = "Button";
        /* Get text content */
        lxb_dom_node_t *tc = lxb_dom_node_first_child(node);
        if (tc && lxb_dom_node_type(tc) == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_text_t *t = lxb_dom_interface_text(tc);
            if (t && t->char_data.data.data) btn_text = (const char *)t->char_data.data.data;
        }
        int bw = chaos_gl_text_width(btn_text) + 16, bh = 26;
        emit(ctx, HTML_BOX_RECT, ctx->cx, ctx->cy, bw, bh, NULL, 0x00CCCCCC, 0x00E8E8E8, NULL, 0, 0);
        emit(ctx, HTML_BOX_TEXT, ctx->cx + 8, ctx->cy + 6, bw - 16, fh, btn_text, 0x00222222, 0, NULL, 0, 0);
        ctx->cx += bw + 4;
        if (ctx->line_h < bh) ctx->line_h = bh;
        goto restore;
    }

    /* Table */
    if (tag == LXB_TAG_TABLE) {
        if (ctx->cx > ctx->indent) { ctx->cy += ctx->line_h; ctx->line_h = fh; }
        ctx->cx = ctx->indent;
        ctx->cy += style.margin_top;
        layout_table(ctx, node, &style);
        ctx->cy += style.margin_bottom;
        ctx->cx = saved_indent;
        goto restore;
    }

    /* Skip position:absolute/fixed — they need their own coordinate space */
    if (style.position == 2 || style.position == 3) goto restore;

    /* Flex container — children laid out horizontally */
    if (style.display == 3) {
        if (ctx->cx > ctx->indent) { ctx->cy += ctx->line_h; ctx->line_h = fh; }
        ctx->cy += style.margin_top;
        int avail = ctx->viewport_w - ctx->indent - style.margin_right;
        int flex_w = avail;
        if (style.width > 0) flex_w = style.width;
        if (style.max_width > 0 && flex_w > style.max_width) flex_w = style.max_width;

        /* Auto margin centering */
        int block_x = ctx->indent + style.margin_left;
        if (style.margin_left_auto && style.margin_right_auto && flex_w < avail) {
            block_x = ctx->indent + (avail - flex_w) / 2;
        }

        int saved_vw = ctx->viewport_w;
        int saved_cw = ctx->container_w;
        ctx->cx = block_x + style.padding_left;
        ctx->indent = block_x + style.padding_left;
        ctx->viewport_w = block_x + flex_w - style.padding_right;
        ctx->container_w = flex_w;

        ctx->cy += style.padding_top;

        if (style.bg_color) {
            emit(ctx, HTML_BOX_RECT, block_x, ctx->cy, flex_w, 0,
                 NULL, 0, style.bg_color, NULL, 0, 0);
        }

        layout_flex_row(ctx, node, flex_w - style.padding_left - style.padding_right);

        ctx->cy += style.padding_bottom;
        ctx->viewport_w = saved_vw;
        ctx->container_w = saved_cw;
        ctx->cx = saved_indent;
        ctx->cy += style.margin_bottom;
        goto restore;
    }

    if (is_block_tag(tag) || style.display == 1) {
        if (ctx->cx > ctx->indent) { ctx->cy += ctx->line_h; ctx->line_h = fh; }

        /* Clear: drop below active floats */
        if (style.css_clear) {
            for (int fi = 0; fi < ctx->float_count; fi++) {
                float_box_t *fb = &ctx->floats[fi];
                bool should_clear = (style.css_clear == 3) ||
                    (style.css_clear == 1 && fb->side == 1) ||
                    (style.css_clear == 2 && fb->side == 2);
                if (should_clear) {
                    int bot = fb->y + fb->h;
                    if (bot > ctx->cy) ctx->cy = bot;
                }
            }
            update_float_clearance(ctx);
        }

        ctx->cy += style.margin_top;

        if (tag == LXB_TAG_HR) {
            int avail = ctx->viewport_w - ctx->indent * 2;
            emit(ctx, HTML_BOX_HR, ctx->indent, ctx->cy, avail, 1,
                 NULL, 0x00CCCCCC, 0, NULL, 0, 0);
            ctx->cy += 2 + style.margin_bottom; goto restore;
        }
        if (tag == LXB_TAG_BR) { ctx->cy += fh; goto restore; }
        if (tag == LXB_TAG_LI) {
            emit(ctx, HTML_BOX_BULLET, ctx->indent - 12, ctx->cy, 6, fh,
                 NULL, ctx->color ? ctx->color : DEFAULT_FG, 0, NULL, 0, 0);
        }

        /* Compute available and effective width */
        int saved_vw = ctx->viewport_w;
        int saved_container = ctx->container_w;
        int avail_w = ctx->viewport_w - ctx->indent - style.margin_right;

        /* Effective content width */
        int content_w = avail_w;
        if (style.width > 0) content_w = style.width;
        if (style.max_width > 0 && content_w > style.max_width) content_w = style.max_width;
        if (style.min_width > 0 && content_w < style.min_width) content_w = style.min_width;
        if (content_w > avail_w) content_w = avail_w;
        if (content_w < 0) content_w = 0;

        /* Float handling */
        if (style.css_float && ctx->float_count < MAX_FLOATS) {
            int float_x, float_w = content_w;
            if (float_w <= 0) float_w = avail_w / 4; /* default float width */

            if (style.css_float == 1) {
                /* Float left */
                float_x = ctx->indent + ctx->float_left_w;
            } else {
                /* Float right */
                float_x = ctx->viewport_w - float_w - ctx->float_right_w;
            }

            /* Record float */
            float_box_t *fb = &ctx->floats[ctx->float_count++];
            fb->x = float_x;
            fb->y = ctx->cy;
            fb->w = float_w;
            fb->side = style.css_float;

            /* Layout float content in its own region */
            int saved_cx = ctx->cx, saved_cy2 = ctx->cy;
            ctx->cx = float_x + style.padding_left;
            ctx->indent = float_x + style.padding_left;
            ctx->viewport_w = float_x + float_w - style.padding_right;
            ctx->container_w = float_w;

            if (style.bg_color) {
                emit(ctx, HTML_BOX_RECT, float_x, ctx->cy, float_w, 0,
                     NULL, 0, style.bg_color, NULL, 0, 0);
            }

            layout_children(ctx, node);
            ctx->cy += ctx->line_h;

            /* Set float height now that we know it */
            int float_h = ctx->cy - saved_cy2;
            if (float_h < fh) float_h = fh;
            fb->h = float_h;

            /* Restore — DON'T advance cy (float doesn't push content down) */
            ctx->cx = saved_cx;
            ctx->cy = saved_cy2;
            ctx->indent = saved_indent;
            ctx->viewport_w = saved_vw;
            ctx->container_w = saved_container;

            /* Update float clearance */
            update_float_clearance(ctx);

            goto restore;
        }

        /* Non-floated block: compute position with auto-margin centering */
        update_float_clearance(ctx);
        int block_x = ctx->indent + ctx->float_left_w + style.margin_left;
        int block_w = content_w - ctx->float_left_w - ctx->float_right_w;

        /* Auto margin centering: if both left and right margins are auto,
         * center the block within available space */
        if (style.margin_left_auto && style.margin_right_auto && content_w < (avail_w - ctx->float_left_w - ctx->float_right_w)) {
            int remaining = avail_w - ctx->float_left_w - ctx->float_right_w - content_w;
            block_x = ctx->indent + ctx->float_left_w + remaining / 2;
            block_w = content_w;
        }

        if (block_w < 50 && (ctx->float_left_w || ctx->float_right_w)) {
            /* Not enough room beside float — clear floats */
            for (int fi = 0; fi < ctx->float_count; fi++) {
                int fb = ctx->floats[fi].y + ctx->floats[fi].h;
                if (fb > ctx->cy) ctx->cy = fb;
            }
            update_float_clearance(ctx);
            block_x = ctx->indent + style.margin_left;
            block_w = content_w;
        }

        ctx->cx = block_x + style.padding_left;
        ctx->cy += style.padding_top;
        ctx->indent = block_x + style.padding_left;
        ctx->viewport_w = block_x + block_w - style.padding_right;
        ctx->container_w = block_w;

        int block_start_y = ctx->cy;

        /* Blockquote left border */
        if (tag == LXB_TAG_BLOCKQUOTE && style.border > 0) {
            emit(ctx, HTML_BOX_RECT, block_x - style.border, ctx->cy,
                 style.border, 0, NULL, 0, style.border_color, NULL, 0, 0);
        }

        layout_children(ctx, node);

        ctx->cy += ctx->line_h + style.padding_bottom;
        ctx->line_h = fh;
        ctx->viewport_w = saved_vw;
        ctx->container_w = saved_container;

        /* Background rect */
        if (style.bg_color) {
            int bh = ctx->cy - block_start_y;
            if (bh > 0) emit(ctx, HTML_BOX_RECT, block_x, block_start_y - 2,
                             block_w, bh + 4,
                             NULL, 0, style.bg_color, NULL, 0, 0);
        }
        /* Border */
        if (style.border > 0 && tag != LXB_TAG_BLOCKQUOTE) {
            int bh = ctx->cy - block_start_y;
            emit(ctx, HTML_BOX_RECT, block_x, block_start_y, block_w, bh,
                 NULL, style.border_color, 0, NULL, 0, 0);
        }

        ctx->cx = saved_indent;
        ctx->cy += style.margin_bottom;
    } else {
        /* Inline: bg for code */
        if (style.bg_color && tag == LXB_TAG_CODE) {
            int sx = ctx->cx, sy = ctx->cy;
            layout_children(ctx, node);
            int cw = ctx->cx - sx;
            if (cw > 0) emit(ctx, HTML_BOX_RECT, sx - 2, sy, cw + 4, fh, NULL, 0, style.bg_color, NULL, 0, 0);
        } else {
            layout_children(ctx, node);
        }
    }

restore:
    ctx->color = saved_color;
    ctx->style_flags = saved_flags;
    ctx->href = saved_href;
    ctx->href_len = saved_href_len;
    ctx->indent = saved_indent;
    ctx->text_align = saved_text_align;
    ctx->white_space = saved_white_space;
}

/* ═══ Public API ════════════════════════════════════════ */

html_layout_t *html_layout_compute(html_doc_t *doc, int viewport_w) {
    if (!doc || !doc->document) return NULL;
    html_layout_t *ly = (html_layout_t *)kmalloc(sizeof(html_layout_t));
    if (!ly) return NULL;
    ly->boxes = (html_box_t *)kmalloc(sizeof(html_box_t) * MAX_BOXES);
    if (!ly->boxes) { kfree(ly); return NULL; }
    ly->count = 0; ly->capacity = MAX_BOXES; ly->total_height = 0;

    /* Initialize Lexbor CSS system and process <style> blocks + inline styles */
    css_init_document(doc);
    /* CSS diagnostics removed */

    /* CSS diagnostics removed — system verified working */

    int fh = chaos_gl_font_height(-1);
    if (fh < 8) fh = 14;

    layout_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.layout = ly; ctx.viewport_w = viewport_w;
    ctx.container_w = viewport_w;
    ctx.cx = 8; ctx.cy = 4; ctx.indent = 8;
    ctx.line_h = fh; ctx.color = DEFAULT_FG;
    ctx.float_count = 0;

    lxb_html_body_element_t *body = lxb_html_document_body_element(doc->document);
    if (body) layout_children(&ctx, lxb_dom_interface_node(body));
    else {
        lxb_dom_node_t *root = lxb_dom_interface_node(doc->document);
        layout_children(&ctx, root);
    }

    ly->total_height = ctx.cy + ctx.line_h + 8;
    serial_printf("[css] layout: %u boxes, height=%d\n", ly->count, ly->total_height);
    return ly;
}

void html_layout_free(html_layout_t *layout) {
    if (!layout) return;
    if (layout->boxes) kfree(layout->boxes);
    kfree(layout);
}
