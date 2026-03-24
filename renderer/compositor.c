/* ChaosGL Compositor — implementation (Phase 5) */

#include "compositor.h"
#include "surface.h"
#include "../drivers/framebuffer.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../kernel/heap.h"
#include "../kernel/rdtsc.h"
#include "../drivers/serial.h"
#include "../include/string.h"

/* ── Static state ─────────────────────────────────────── */

static uint32_t* comp_buffer = 0;
static uint32_t  comp_pages  = 0;
static int       screen_w    = 0;
static int       screen_h    = 0;
static int       screen_pitch = 0;  /* in pixels (bytes / 4) */
static uint32_t* vram        = 0;
static chaos_gl_stats_t compose_stats;
static bool      first_compose = true;

/* ── Helpers ──────────────────────────────────────────── */

static inline int int_max(int a, int b) { return a > b ? a : b; }
static inline int int_min(int a, int b) { return a < b ? a : b; }

/* ── Init / Shutdown ──────────────────────────────────── */

int chaos_gl_compositor_init(void) {
    fb_info_t info;
    if (!fb_get_info(&info)) {
        serial_printf("[compositor] fb_get_info failed\n");
        return -1;
    }

    screen_w     = (int)info.width;
    screen_h     = (int)info.height;
    screen_pitch = (int)(info.pitch / 4);
    vram         = (uint32_t*)(uint32_t)info.fb_addr;

    uint32_t buf_size = (uint32_t)screen_w * (uint32_t)screen_h * 4;
    comp_pages = (buf_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t phys = pmm_alloc_pages(comp_pages);
    if (!phys) {
        serial_printf("[compositor] pmm_alloc_pages(%u) failed\n", comp_pages);
        return -1;
    }

    vmm_map_range(phys, phys, comp_pages * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
    heap_mark_reserved(phys, comp_pages);

    comp_buffer = (uint32_t*)(uint32_t)phys;
    memset(comp_buffer, 0, buf_size);

    first_compose = true;
    memset(&compose_stats, 0, sizeof(compose_stats));

    serial_printf("[compositor] init: %dx%d, %u pages at 0x%x\n",
                  screen_w, screen_h, comp_pages, phys);
    return 0;
}

void chaos_gl_compositor_shutdown(void) {
    if (comp_buffer && comp_pages) {
        pmm_free_pages((uint32_t)(uint32_t)comp_buffer, comp_pages);
        comp_buffer = 0;
        comp_pages  = 0;
    }
}

/* ── Compose ──────────────────────────────────────────── */

void chaos_gl_compose(uint32_t desktop_clear_color) {
    if (!comp_buffer || !vram) return;

    uint64_t t_start = rdtsc();

    /* Gather visible surfaces and determine dirty region */
    chaos_gl_surface_t* sorted[CHAOS_GL_MAX_SURFACES];
    int count = 0;
    bool any_dirty = false;

    /* Dirty region (screen coords) */
    int dirty_x0 = screen_w, dirty_y0 = screen_h;
    int dirty_x1 = 0, dirty_y1 = 0;

    for (int i = 0; i < CHAOS_GL_MAX_SURFACES; i++) {
        chaos_gl_surface_t* s = chaos_gl_get_surface(i);
        if (!s || !s->in_use || !s->visible) continue;

        sorted[count++] = s;

        if (s->dirty) {
            any_dirty = true;
            int ssc = s->scale > 0 ? s->scale : 1;
            /* Union this surface's current screen rect into dirty region */
            int sx0 = s->screen_x;
            int sy0 = s->screen_y;
            int sx1 = s->screen_x + s->width * ssc;
            int sy1 = s->screen_y + s->height * ssc;
            if (sx0 < dirty_x0) dirty_x0 = sx0;
            if (sy0 < dirty_y0) dirty_y0 = sy0;
            if (sx1 > dirty_x1) dirty_x1 = sx1;
            if (sy1 > dirty_y1) dirty_y1 = sy1;

            /* Also include old position if surface moved */
            if (s->position_changed) {
                int px0 = s->prev_screen_x;
                int py0 = s->prev_screen_y;
                int px1 = s->prev_screen_x + s->width * ssc;
                int py1 = s->prev_screen_y + s->height * ssc;
                if (px0 < dirty_x0) dirty_x0 = px0;
                if (py0 < dirty_y0) dirty_y0 = py0;
                if (px1 > dirty_x1) dirty_x1 = px1;
                if (py1 > dirty_y1) dirty_y1 = py1;
                s->position_changed = false;
            }
        }
    }

    /* Force full-screen dirty when a visible surface was destroyed or on first compose */
    if (first_compose || chaos_gl_surface_needs_full_compose()) {
        any_dirty = true;
        dirty_x0 = 0;
        dirty_y0 = 0;
        dirty_x1 = screen_w;
        dirty_y1 = screen_h;
        first_compose = false;
    }

    if (!any_dirty) {
        memset(&compose_stats, 0, sizeof(compose_stats));
        return;
    }

    /* Clamp dirty region to screen */
    dirty_x0 = int_max(dirty_x0, 0);
    dirty_y0 = int_max(dirty_y0, 0);
    dirty_x1 = int_min(dirty_x1, screen_w);
    dirty_y1 = int_min(dirty_y1, screen_h);

    if (dirty_x0 >= dirty_x1 || dirty_y0 >= dirty_y1) {
        memset(&compose_stats, 0, sizeof(compose_stats));
        return;
    }

    /* Insertion sort by z_order (ascending: low z = back) */
    for (int i = 1; i < count; i++) {
        chaos_gl_surface_t* key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->z_order > key->z_order) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    /* Clear dirty region of comp_buffer to desktop color */
    for (int y = dirty_y0; y < dirty_y1; y++) {
        uint32_t* row = &comp_buffer[y * screen_w + dirty_x0];
        int span = dirty_x1 - dirty_x0;
        for (int i = 0; i < span; i++)
            row[i] = desktop_clear_color;
    }

    /* Composite each surface */
    uint32_t surfaces_composited = 0;

    for (int si = 0; si < count; si++) {
        chaos_gl_surface_t* s = sorted[si];

        if (s->alpha == 0) {
            s->dirty = false;
            continue;
        }

        /* Surface screen-space rect (with scale) */
        int sc = s->scale > 0 ? s->scale : 1;
        int sx0 = s->screen_x;
        int sy0 = s->screen_y;
        int sx1 = s->screen_x + s->width * sc;
        int sy1 = s->screen_y + s->height * sc;

        /* Intersect with dirty region */
        int ix0 = int_max(sx0, dirty_x0);
        int iy0 = int_max(sy0, dirty_y0);
        int ix1 = int_min(sx1, dirty_x1);
        int iy1 = int_min(sy1, dirty_y1);

        if (ix0 >= ix1 || iy0 >= iy1) {
            s->dirty = false;
            continue;
        }

        /* Get front buffer */
        const uint32_t* front = s->bufs[s->buf_index];
        uint8_t alpha = s->alpha;

        if (alpha == 255 && !s->has_color_key && sc == 1) {
            /* Opaque blit — fast path (no scale) */
            for (int y = iy0; y < iy1; y++) {
                int src_y = y - sy0;
                int src_x = ix0 - sx0;
                const uint32_t* srow = &front[src_y * s->width + src_x];
                uint32_t* drow = &comp_buffer[y * screen_w + ix0];
                memcpy(drow, srow, (uint32_t)(ix1 - ix0) * 4);
            }
        } else if (alpha == 255 && !s->has_color_key) {
            /* Opaque blit with nearest-neighbor scale */
            for (int y = iy0; y < iy1; y++) {
                int src_y = (y - sy0) / sc;
                uint32_t* drow = &comp_buffer[y * screen_w + ix0];
                int span = ix1 - ix0;
                for (int i = 0; i < span; i++) {
                    int src_x = (ix0 - sx0 + i) / sc;
                    drow[i] = front[src_y * s->width + src_x];
                }
            }
        } else if (alpha == 255 && s->has_color_key) {
            /* Opaque blit with color key transparency (with scale) */
            uint32_t key = s->color_key;
            for (int y = iy0; y < iy1; y++) {
                int src_y = (y - sy0) / sc;
                uint32_t* drow = &comp_buffer[y * screen_w + ix0];
                int span = ix1 - ix0;
                for (int i = 0; i < span; i++) {
                    int src_x = (ix0 - sx0 + i) / sc;
                    uint32_t px = front[src_y * s->width + src_x];
                    if (px != key) drow[i] = px;
                }
            }
        } else {
            /* Alpha blend: surface-level alpha (with scale) */
            uint32_t a = alpha;
            uint32_t inv_a = 255 - a;

            for (int y = iy0; y < iy1; y++) {
                int src_y = (y - sy0) / sc;
                uint32_t* drow = &comp_buffer[y * screen_w + ix0];
                int span = ix1 - ix0;

                for (int i = 0; i < span; i++) {
                    int src_x = (ix0 - sx0 + i) / sc;
                    uint32_t spx = front[src_y * s->width + src_x];
                    uint32_t dc = drow[i];
                    uint32_t rb_s = spx & 0x00FF00FF;
                    uint32_t g_s  = spx & 0x0000FF00;
                    uint32_t rb_d = dc & 0x00FF00FF;
                    uint32_t g_d  = dc & 0x0000FF00;
                    uint32_t rb = ((rb_s * a + rb_d * inv_a + 0x00800080) >> 8) & 0x00FF00FF;
                    uint32_t g  = ((g_s  * a + g_d  * inv_a + 0x00000080) >> 8) & 0x0000FF00;
                    drow[i] = rb | g;
                }
            }
        }

        s->dirty = false;
        surfaces_composited++;
    }

    /* Blit comp_buffer dirty region to VRAM */
    uint64_t t_blit = rdtsc();

    for (int y = dirty_y0; y < dirty_y1; y++) {
        memcpy(&vram[y * screen_pitch + dirty_x0],
               &comp_buffer[y * screen_w + dirty_x0],
               (uint32_t)(dirty_x1 - dirty_x0) * 4);
    }

    uint64_t t_end = rdtsc();

    /* Record stats */
    memset(&compose_stats, 0, sizeof(compose_stats));
    compose_stats.compose_time_us  = rdtsc_to_us(t_end - t_start);
    compose_stats.compose_blit_us  = rdtsc_to_us(t_end - t_blit);
    compose_stats.surfaces_composited = surfaces_composited;
}

/* ── Stats query ──────────────────────────────────────── */

chaos_gl_stats_t chaos_gl_get_compose_stats(void) {
    return compose_stats;
}
