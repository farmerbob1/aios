/* AIOS v2 — VESA Framebuffer HAL (Phase 3) */

#include "framebuffer.h"

static fb_info_t fb;
static bool fb_available = false;

init_result_t fb_init(struct boot_info* info) {
    if (!info || info->fb_addr == 0) {
        fb_available = false;
        return INIT_WARN;  /* No VBE framebuffer — text mode only */
    }
    if (info->fb_bpp != 32) {
        fb_available = false;
        return INIT_FAIL;  /* Only 32bpp BGRX supported */
    }

    fb.fb_addr = info->fb_addr;
    fb.width   = info->fb_width;
    fb.height  = info->fb_height;
    fb.pitch   = info->fb_pitch;
    fb.bpp     = info->fb_bpp;
    fb_available = true;

    return INIT_OK;
}

bool fb_get_info(fb_info_t* out) {
    if (!fb_available || !out) return false;
    *out = fb;
    return true;
}
