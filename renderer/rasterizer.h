#pragma once
#include "pipeline.h"

/* Screen-space vertex for rasterizer */
typedef struct {
    float sx, sy;       /* screen-space position */
    float ndc_z;        /* NDC depth [-1,1] */
    float inv_w;        /* 1.0 / clip_w (for perspective-correct interp) */
    vec3_t normal;
    vec2_t uv;
    float  intensity;
} raster_vertex_t;

/* Rasterize a single triangle. Called by the pipeline after all clipping/transforms. */
void rasterize_triangle(chaos_gl_surface_t* s, raster_vertex_t v[3]);
