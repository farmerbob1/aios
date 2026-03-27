/* AIOS HTML Layout Engine
 * Walks a Lexbor DOM tree and produces a flat array of positioned boxes
 * for rendering by the Lua browser app via ChaosGL. */

#pragma once

#include "../../include/types.h"

/* Box types */
#define HTML_BOX_TEXT    1
#define HTML_BOX_HR     2
#define HTML_BOX_RECT   3
#define HTML_BOX_BULLET 4
#define HTML_BOX_IMAGE  5

/* Style flags */
#define HTML_BOLD      0x01
#define HTML_ITALIC    0x02
#define HTML_UNDERLINE 0x04
#define HTML_MONOSPACE 0x08

/* A single layout box */
typedef struct {
    int x, y, w, h;
    int type;
    uint32_t fg_color;
    uint32_t bg_color;
    uint8_t  style_flags;
    char     text[512];
    char     url[256];
} html_box_t;

/* Layout result */
typedef struct {
    html_box_t *boxes;
    uint32_t    count;
    uint32_t    capacity;
    int         total_height;
} html_layout_t;

/* Opaque document handle */
typedef struct html_doc html_doc_t;

/* Parse HTML source into a Lexbor document */
html_doc_t *html_doc_parse(const char *html, uint32_t len);

/* Get document title */
const char *html_doc_title(html_doc_t *doc);

/* Compute layout for a viewport width */
html_layout_t *html_layout_compute(html_doc_t *doc, int viewport_w);

/* Free resources */
void html_layout_free(html_layout_t *layout);
void html_doc_free(html_doc_t *doc);

/* Access internal Lexbor document (for QuickJS DOM bridge) */
struct lxb_html_document;
struct lxb_html_document *html_doc_get_lxb(html_doc_t *doc);

/* Attach a CSS stylesheet to the document (for external <link> stylesheets) */
int html_doc_attach_css(html_doc_t *doc, const char *css, uint32_t len);
