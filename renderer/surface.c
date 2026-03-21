#include "surface.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../kernel/heap.h"
#include "../kernel/scheduler.h"
#include "../drivers/serial.h"
#include "../include/string.h"

static chaos_gl_surface_t* surfaces;

void chaos_gl_surface_init(void) {
    surfaces = (chaos_gl_surface_t*)kzmalloc(CHAOS_GL_MAX_SURFACES * sizeof(chaos_gl_surface_t));
}

static bool valid_handle(int handle) {
    return handle >= 0 && handle < CHAOS_GL_MAX_SURFACES && surfaces[handle].in_use;
}

static uint32_t alloc_buffer(uint32_t pages) {
    uint32_t phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    vmm_map_range(phys, phys, pages * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
    heap_mark_reserved(phys, pages);
    return phys;
}

static void free_buffer(uint32_t phys, uint32_t pages) {
    if (phys && pages) {
        pmm_free_pages(phys, pages);
    }
}

int chaos_gl_surface_create(int w, int h, bool has_depth) {
    int handle = -1;
    for (int i = 0; i < CHAOS_GL_MAX_SURFACES; i++) {
        if (!surfaces[i].in_use) {
            handle = i;
            break;
        }
    }
    if (handle < 0) {
        serial_printf("surface_create: no free slot\n");
        return -1;
    }

    uint32_t pixel_bytes = (uint32_t)w * (uint32_t)h * 4;
    uint32_t pages = (pixel_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t buf0 = alloc_buffer(pages);
    if (!buf0) {
        serial_printf("surface_create: OOM for buf0\n");
        return -1;
    }

    uint32_t buf1 = alloc_buffer(pages);
    if (!buf1) {
        serial_printf("surface_create: OOM for buf1\n");
        free_buffer(buf0, pages);
        return -1;
    }

    uint32_t zbuf = 0;
    uint32_t zbuf_pages = 0;
    if (has_depth) {
        uint32_t zbuf_bytes = (uint32_t)w * (uint32_t)h * 2;
        zbuf_pages = (zbuf_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        zbuf = alloc_buffer(zbuf_pages);
        if (!zbuf) {
            serial_printf("surface_create: OOM for zbuffer\n");
            free_buffer(buf0, pages);
            free_buffer(buf1, pages);
            return -1;
        }
    }

    memset((void*)buf0, 0, pixel_bytes);
    memset((void*)buf1, 0, pixel_bytes);

    chaos_gl_surface_t* s = &surfaces[handle];
    memset(s, 0, sizeof(*s));

    s->bufs[0] = (uint32_t*)buf0;
    s->bufs[1] = (uint32_t*)buf1;
    s->buf_index = 0;
    s->bufs_pages[0] = pages;
    s->bufs_pages[1] = pages;

    s->zbuffer = has_depth ? (uint16_t*)zbuf : NULL;
    s->zbuf_pages = zbuf_pages;

    s->width = w;
    s->height = h;
    s->screen_x = 0;
    s->screen_y = 0;
    s->z_order = 0;
    s->alpha = 255;
    s->visible = false;
    s->dirty = false;

    s->clip_stack[0] = (rect_t){ 0, 0, w, h };
    s->clip_depth = 1;

    s->view = mat4_identity();
    s->projection = mat4_identity();
    s->model = mat4_identity();
    s->active_vert = NULL;
    s->active_frag = NULL;
    s->active_uniforms = NULL;

    s->in_use = true;

    return handle;
}

/* Global flag: when a visible surface is destroyed, force full-screen
   recomposite on next compose() to erase its ghost. */
static bool force_full_compose = false;

bool chaos_gl_surface_needs_full_compose(void) {
    bool r = force_full_compose;
    force_full_compose = false;
    return r;
}

void chaos_gl_surface_destroy(int handle) {
    if (!valid_handle(handle)) return;

    chaos_gl_surface_t* s = &surfaces[handle];

    /* If this surface was visible, force full recomposite */
    if (s->visible) {
        force_full_compose = true;
    }

    free_buffer((uint32_t)s->bufs[0], s->bufs_pages[0]);
    free_buffer((uint32_t)s->bufs[1], s->bufs_pages[1]);
    if (s->zbuffer) {
        free_buffer((uint32_t)s->zbuffer, s->zbuf_pages);
    }

    s->in_use = false;

    for (int i = 0; i < MAX_TASKS; i++) {
        struct task* t = task_get(i);
        if (t && t->chaos_gl_surface_handle == handle) {
            t->chaos_gl_surface_handle = -1;
        }
    }
}

void chaos_gl_surface_bind(int handle) {
    if (handle >= 0 && handle < CHAOS_GL_MAX_SURFACES && surfaces[handle].in_use) {
        task_get_current()->chaos_gl_surface_handle = handle;
    }
}

void chaos_gl_surface_clear(int handle, uint32_t color_bgrx) {
    if (!valid_handle(handle)) return;

    chaos_gl_surface_t* s = &surfaces[handle];
    uint32_t* back = s->bufs[1 - s->buf_index];
    int count = s->width * s->height;

    for (int i = 0; i < count; i++) {
        back[i] = color_bgrx;
    }

    if (s->zbuffer) {
        uint16_t* zb = s->zbuffer;
        int zcount = s->width * s->height;
        for (int i = 0; i < zcount; i++) {
            zb[i] = CHAOS_GL_ZDEPTH_MAX;
        }
    }

    s->clip_stack[0] = (rect_t){ 0, 0, s->width, s->height };
    s->clip_depth = 1;
    memset(&s->stats, 0, sizeof(s->stats));
}

void chaos_gl_surface_present(int handle) {
    if (!valid_handle(handle)) return;

    chaos_gl_surface_t* s = &surfaces[handle];
    s->buf_index ^= 1;
    s->dirty = true;
}

void chaos_gl_surface_set_position(int handle, int x, int y) {
    if (!valid_handle(handle)) return;
    chaos_gl_surface_t *s = &surfaces[handle];
    if (s->screen_x != x || s->screen_y != y) {
        /* Only save prev on first move since last compose — preserves
           the original position across multiple moves in one frame */
        if (!s->position_changed) {
            s->prev_screen_x = s->screen_x;
            s->prev_screen_y = s->screen_y;
        }
        s->position_changed = true;
        s->screen_x = x;
        s->screen_y = y;
        s->dirty = true;
    }
}

void chaos_gl_surface_get_position(int handle, int* x, int* y) {
    if (!valid_handle(handle)) return;
    if (x) *x = surfaces[handle].screen_x;
    if (y) *y = surfaces[handle].screen_y;
}

void chaos_gl_surface_set_zorder(int handle, int z) {
    if (!valid_handle(handle)) return;
    if (surfaces[handle].z_order != z) {
        surfaces[handle].z_order = z;
        surfaces[handle].dirty = true;
    }
}

int chaos_gl_surface_get_zorder(int handle) {
    if (!valid_handle(handle)) return 0;
    return surfaces[handle].z_order;
}

void chaos_gl_surface_set_visible(int handle, bool visible) {
    if (!valid_handle(handle)) return;
    if (surfaces[handle].visible && !visible) {
        /* Hiding a visible surface — force full recomposite to erase it */
        extern bool force_full_compose;
        force_full_compose = true;
    }
    surfaces[handle].visible = visible;
}

void chaos_gl_surface_set_alpha(int handle, uint8_t alpha) {
    if (!valid_handle(handle)) return;
    surfaces[handle].alpha = alpha;
}

int chaos_gl_surface_resize(int handle, int w, int h) {
    if (!valid_handle(handle)) return -1;

    chaos_gl_surface_t* s = &surfaces[handle];

    free_buffer((uint32_t)s->bufs[0], s->bufs_pages[0]);
    free_buffer((uint32_t)s->bufs[1], s->bufs_pages[1]);
    if (s->zbuffer) {
        free_buffer((uint32_t)s->zbuffer, s->zbuf_pages);
    }

    uint32_t pixel_bytes = (uint32_t)w * (uint32_t)h * 4;
    uint32_t pages = (pixel_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t buf0 = alloc_buffer(pages);
    if (!buf0) {
        serial_printf("surface_resize: OOM for buf0\n");
        s->in_use = false;
        return -1;
    }

    uint32_t buf1 = alloc_buffer(pages);
    if (!buf1) {
        serial_printf("surface_resize: OOM for buf1\n");
        free_buffer(buf0, pages);
        s->in_use = false;
        return -1;
    }

    bool had_depth = (s->zbuffer != NULL);
    uint32_t zbuf = 0;
    uint32_t zbuf_pages = 0;
    if (had_depth) {
        uint32_t zbuf_bytes = (uint32_t)w * (uint32_t)h * 2;
        zbuf_pages = (zbuf_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        zbuf = alloc_buffer(zbuf_pages);
        if (!zbuf) {
            serial_printf("surface_resize: OOM for zbuffer\n");
            free_buffer(buf0, pages);
            free_buffer(buf1, pages);
            s->in_use = false;
            return -1;
        }
    }

    memset((void*)buf0, 0, pixel_bytes);
    memset((void*)buf1, 0, pixel_bytes);

    s->bufs[0] = (uint32_t*)buf0;
    s->bufs[1] = (uint32_t*)buf1;
    s->bufs_pages[0] = pages;
    s->bufs_pages[1] = pages;
    s->buf_index = 0;

    s->zbuffer = had_depth ? (uint16_t*)zbuf : NULL;
    s->zbuf_pages = zbuf_pages;

    s->width = w;
    s->height = h;

    s->clip_stack[0] = (rect_t){ 0, 0, w, h };
    s->clip_depth = 1;

    return 0;
}

void chaos_gl_surface_get_size(int handle, int* w, int* h) {
    if (!valid_handle(handle)) return;
    if (w) *w = surfaces[handle].width;
    if (h) *h = surfaces[handle].height;
}

void chaos_gl_surface_set_color_key(int handle, bool enabled, uint32_t key) {
    if (!valid_handle(handle)) return;
    surfaces[handle].has_color_key = enabled;
    surfaces[handle].color_key = key;
}

chaos_gl_surface_t* chaos_gl_get_surface(int handle) {
    if (handle < 0 || handle >= CHAOS_GL_MAX_SURFACES || !surfaces[handle].in_use)
        return NULL;
    return &surfaces[handle];
}

chaos_gl_surface_t* chaos_gl_get_bound_surface(void) {
    struct task* t = task_get_current();
    if (!t) return NULL;
    int handle = t->chaos_gl_surface_handle;
    if (handle < 0 || handle >= CHAOS_GL_MAX_SURFACES || !surfaces[handle].in_use)
        return NULL;
    return &surfaces[handle];
}

chaos_gl_stats_t chaos_gl_get_stats(void) {
    chaos_gl_stats_t empty;
    memset(&empty, 0, sizeof(empty));
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return empty;
    return s->stats;
}
