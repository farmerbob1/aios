#pragma once

#include "math.h"
#include "../include/types.h"

/* Forward declarations for shader function pointer types */
struct gl_vertex_in;
struct gl_vertex_out;
struct gl_fragment_in;
struct gl_frag_out;

typedef struct gl_vertex_out (*gl_vert_fn)(struct gl_vertex_in in, void* uniforms);
typedef struct gl_frag_out (*gl_frag_fn)(struct gl_fragment_in in, void* uniforms);

/* Constants */
#define CHAOS_GL_MAX_SURFACES      32
#define CHAOS_GL_CLIP_STACK_DEPTH  16
#define CHAOS_GL_ZDEPTH_MAX        0xFFFF

/* Text flags */
#define CHAOS_GL_TEXT_BG_TRANSPARENT  0x0
#define CHAOS_GL_TEXT_BG_FILL         0x1

/* Color macro — BGRX format */
#define CHAOS_GL_RGB(r, g, b)  ((uint32_t)((b) | ((g) << 8) | ((r) << 16)))

typedef struct {
    uint32_t triangles_submitted;
    uint32_t triangles_culled;
    uint32_t triangles_clipped;
    uint32_t triangles_drawn;
    uint32_t pixels_written;
    uint32_t pixels_zfailed;
    uint32_t pixels_discarded;

    uint32_t draw_calls_2d;

    uint32_t frame_time_us;
    uint32_t frame_3d_us;
    uint32_t frame_2d_us;

    uint32_t compose_time_us;
    uint32_t compose_blit_us;
    uint32_t surfaces_composited;
} chaos_gl_stats_t;

typedef struct {
    uint32_t*  bufs[2];
    uint8_t    buf_index;
    uint16_t*  zbuffer;
    int        width;
    int        height;

    int        screen_x;
    int        screen_y;
    int        prev_screen_x;
    int        prev_screen_y;
    bool       position_changed;
    int        z_order;

    uint8_t    alpha;
    bool       has_color_key;
    uint32_t   color_key;

    bool       in_use;
    bool       visible;
    bool       dirty;

    rect_t     clip_stack[CHAOS_GL_CLIP_STACK_DEPTH];
    int        clip_depth;

    mat4_t     view;
    mat4_t     projection;
    mat4_t     model;
    gl_vert_fn active_vert;
    gl_frag_fn active_frag;
    void*      active_uniforms;

    uint32_t   bufs_pages[2];
    uint32_t   zbuf_pages;

    chaos_gl_stats_t stats;
} chaos_gl_surface_t;

/* Surface API */
int  chaos_gl_surface_create(int w, int h, bool has_depth);
void chaos_gl_surface_destroy(int handle);
void chaos_gl_surface_bind(int handle);
void chaos_gl_surface_clear(int handle, uint32_t color_bgrx);
void chaos_gl_surface_present(int handle);
void chaos_gl_surface_set_position(int handle, int x, int y);
void chaos_gl_surface_get_position(int handle, int* x, int* y);
void chaos_gl_surface_set_zorder(int handle, int z);
int  chaos_gl_surface_get_zorder(int handle);
void chaos_gl_surface_set_visible(int handle, bool visible);
void chaos_gl_surface_set_alpha(int handle, uint8_t alpha);
int  chaos_gl_surface_resize(int handle, int w, int h);
void chaos_gl_surface_get_size(int handle, int* w, int* h);
void chaos_gl_surface_set_color_key(int handle, bool enabled, uint32_t key);

chaos_gl_surface_t* chaos_gl_get_surface(int handle);
chaos_gl_surface_t* chaos_gl_get_bound_surface(void);
void chaos_gl_surface_init(void);
chaos_gl_stats_t chaos_gl_get_stats(void);
bool chaos_gl_surface_needs_full_compose(void);
