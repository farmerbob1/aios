#pragma once
#include "surface.h"

/* Vertex shader input */
typedef struct gl_vertex_in {
    vec3_t position;
    vec3_t normal;
    vec2_t uv;
} gl_vertex_in_t;

/* Vertex shader output */
typedef struct gl_vertex_out {
    vec4_t clip_pos;
    vec3_t normal;
    vec2_t uv;
    float  intensity;
} gl_vertex_out_t;

/* Fragment shader input */
typedef struct gl_fragment_in {
    vec2_t uv;
    vec3_t normal;
    float  intensity;
    int    x, y;
} gl_fragment_in_t;

/* Fragment shader output */
typedef struct gl_frag_out {
    uint32_t color;
    bool     discard;
} gl_frag_out_t;

#define GL_COLOR(bgrx)  ((gl_frag_out_t){ .color = (bgrx), .discard = false })
#define GL_DISCARD      ((gl_frag_out_t){ .color = 0,      .discard = true  })

/* Guard-band */
#define CHAOS_GL_GUARD_BAND_MULT  4.0f

/* Pipeline API */
void chaos_gl_triangle(gl_vertex_in_t v0, gl_vertex_in_t v1, gl_vertex_in_t v2);

/* Clipping */
int  chaos_gl_clip_near(gl_vertex_out_t in[3], gl_vertex_out_t out_verts[4], float z_near);
bool chaos_gl_guardband_reject(gl_vertex_out_t v[3]);
int  chaos_gl_guardband_clip(gl_vertex_out_t in[3], gl_vertex_out_t out_verts[8]);
