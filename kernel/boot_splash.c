/* AIOS v2 — Graphical Boot Splash (Phase 9)
 * Mac OS 9-style icon parade during KAOS module loading. */

#include "boot_splash.h"
#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "../renderer/chaos_gl.h"

static int splash_surface = -1;
static int icon_x = 0;
static int generic_icon_tex = -1;

void boot_splash_init(void) {
    splash_surface = chaos_gl_surface_create(1024, 768, false);
    if (splash_surface < 0) return;
    chaos_gl_surface_set_position(splash_surface, 0, 0);
    chaos_gl_surface_set_zorder(splash_surface, 50);
    chaos_gl_surface_set_visible(splash_surface, true);

    chaos_gl_surface_bind(splash_surface);
    chaos_gl_surface_clear(splash_surface, 0x00201830);

    /* Logo — rounded rect badge, centered */
    int logo_w = 200, logo_h = 100;
    int logo_x = (1024 - logo_w) / 2, logo_y = 250;
    chaos_gl_rect_rounded(logo_x, logo_y, logo_w, logo_h, 12, 0x00302848);
    chaos_gl_rect_rounded_outline(logo_x, logo_y, logo_w, logo_h, 12, 0x006644AA, 1);
    /* "AIOS" text: ~4 chars * 8px = 32px wide */
    chaos_gl_text((1024 - 32) / 2, logo_y + 28, "AIOS", 0x00FFFFFF, 0, 0);
    /* "v2" text: ~2 chars * 8px = 16px wide */
    chaos_gl_text((1024 - 16) / 2, logo_y + 56, "v2", 0x006644AA, 0, 0);

    /* Load generic module icon (gear/settings) — fallback */
    generic_icon_tex = chaos_gl_texture_load("/system/icons/mod_default_32.raw");
    if (generic_icon_tex < 0) {
        generic_icon_tex = chaos_gl_texture_load("/system/icons/settings_32.raw");
    }

    icon_x = 200;

    chaos_gl_surface_present(splash_surface);
    serial_print("[boot_splash] initialized\n");
}

void boot_splash_status(const char *text) {
    if (splash_surface < 0) return;
    chaos_gl_surface_bind(splash_surface);

    /* Clear status area */
    chaos_gl_rect(0, 400, 1024, 24, 0x00201830);
    /* Draw centered status text */
    int tw = chaos_gl_text_width(text);
    chaos_gl_text((1024 - tw) / 2, 404, text, 0x00888899, 0, 0);

    chaos_gl_surface_present(splash_surface);
}

void boot_splash_module(const char *name, int loaded, int total) {
    if (splash_surface < 0) return;
    chaos_gl_surface_bind(splash_surface);

    /* Try loading module-specific icon: /system/icons/mod_<name>_32.raw */
    char path[64];
    strcpy(path, "/system/icons/mod_");
    uint32_t plen = strlen(path);
    uint32_t nlen = strlen(name);
    if (nlen > sizeof(path) - plen - 8) nlen = sizeof(path) - plen - 8;
    memcpy(path + plen, name, nlen);
    strcpy(path + plen + nlen, "_32.raw");

    int tex = chaos_gl_texture_load(path);
    if (tex < 0) tex = generic_icon_tex;

    /* Draw icon at parade position */
    int parade_y = 500;
    if (tex >= 0) {
        const chaos_gl_texture_t *t = chaos_gl_texture_get(tex);
        if (t && t->data) {
            chaos_gl_blit_keyed(icon_x, parade_y, 32, 32,
                                t->data, t->pitch, 0x00FF00FF);
        }
    }

    /* Module name below icon */
    int nw = chaos_gl_text_width(name);
    int name_x = icon_x + (32 - nw) / 2;
    chaos_gl_text(name_x, parade_y + 36, name, 0x00666677, 0, 0);

    icon_x += 40;

    /* Progress bar */
    int bar_x = 200, bar_y = 580, bar_w = 624, bar_h = 8;
    chaos_gl_rect(bar_x, bar_y, bar_w, bar_h, 0x00302848);
    int fill_w = (loaded * bar_w) / (total > 0 ? total : 1);
    chaos_gl_rect(bar_x, bar_y, fill_w, bar_h, 0x006644AA);

    /* Free module-specific icon if it was loaded (not the generic one) */
    if (tex >= 0 && tex != generic_icon_tex) {
        chaos_gl_texture_free(tex);
    }

    chaos_gl_surface_present(splash_surface);
}

void boot_splash_destroy(void) {
    if (splash_surface < 0) return;
    if (generic_icon_tex >= 0) chaos_gl_texture_free(generic_icon_tex);
    chaos_gl_surface_destroy(splash_surface);
    splash_surface = -1;
    generic_icon_tex = -1;
    serial_print("[boot_splash] destroyed\n");
}
