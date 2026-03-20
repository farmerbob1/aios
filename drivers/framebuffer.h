/* AIOS v2 — VESA Framebuffer HAL (Phase 3)
 * Hardware abstraction only. No drawing primitives.
 * All rendering goes through ChaosGL. */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

typedef struct {
    uint32_t fb_addr;    /* physical address of VBE linear framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint8_t  bpp;        /* bits per pixel — must be 32 */
} fb_info_t;

init_result_t fb_init(struct boot_info* info);
bool fb_get_info(fb_info_t* out);
