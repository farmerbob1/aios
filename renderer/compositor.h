/* ChaosGL Compositor — surface compositing and VRAM output (Phase 5) */

#pragma once

#include "surface.h"

/* Initialize compositor -- allocate compositing buffer from PMM.
 * Must be called after fb_get_info() returns valid data.
 * Returns 0 on success, -1 on failure. */
int chaos_gl_compositor_init(void);

/* Shutdown -- free compositing buffer. */
void chaos_gl_compositor_shutdown(void);

/* Compose all visible surfaces into compositing buffer, blit to VRAM.
 * Called by compositor task once per frame. */
void chaos_gl_compose(uint32_t desktop_clear_color);

/* Get compositor stats from last compose pass. */
chaos_gl_stats_t chaos_gl_get_compose_stats(void);

/* Get screen resolution. */
void chaos_gl_get_screen_size(int *w, int *h);
