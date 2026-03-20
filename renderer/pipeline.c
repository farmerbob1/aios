/* ChaosGL 3D Pipeline — vertex processing, clipping, guard-band (Phase 5) */

#include "pipeline.h"
#include "rasterizer.h"
#include "../include/string.h"
#include "../drivers/serial.h"

/* ── Helpers ──────────────────────────────────────────── */

static float lerp_f(float a, float b, float t) {
    return a + t * (b - a);
}

static vec2_t lerp_vec2(vec2_t a, vec2_t b, float t) {
    return (vec2_t){ lerp_f(a.x, b.x, t), lerp_f(a.y, b.y, t) };
}

static vec3_t lerp_vec3(vec3_t a, vec3_t b, float t) {
    return (vec3_t){ lerp_f(a.x, b.x, t), lerp_f(a.y, b.y, t), lerp_f(a.z, b.z, t) };
}

static vec4_t lerp_vec4(vec4_t a, vec4_t b, float t) {
    return (vec4_t){ lerp_f(a.x, b.x, t), lerp_f(a.y, b.y, t),
                     lerp_f(a.z, b.z, t), lerp_f(a.w, b.w, t) };
}

static gl_vertex_out_t lerp_vertex(gl_vertex_out_t a, gl_vertex_out_t b, float t) {
    gl_vertex_out_t r;
    r.clip_pos  = lerp_vec4(a.clip_pos, b.clip_pos, t);
    r.normal    = lerp_vec3(a.normal, b.normal, t);
    r.uv        = lerp_vec2(a.uv, b.uv, t);
    r.intensity = lerp_f(a.intensity, b.intensity, t);
    return r;
}

/* Convert a clipped vertex to a raster vertex (perspective divide + viewport) */
static raster_vertex_t to_raster(gl_vertex_out_t v, int w, int h) {
    raster_vertex_t r;
    float inv_w = 1.0f / v.clip_pos.w;
    float ndc_x = v.clip_pos.x * inv_w;
    float ndc_y = v.clip_pos.y * inv_w;
    float ndc_z = v.clip_pos.z * inv_w;

    r.sx    = (ndc_x + 1.0f) * (float)w * 0.5f;
    r.sy    = (1.0f - ndc_y) * (float)h * 0.5f;
    r.ndc_z = ndc_z;
    r.inv_w = inv_w;

    r.normal    = v.normal;
    r.uv        = v.uv;
    r.intensity = v.intensity;
    return r;
}

/* ── Near-plane clipping (Sutherland-Hodgman) ─────────── */

/* A vertex is inside the near plane when clip_pos.z + clip_pos.w >= 0 */
static bool near_inside(gl_vertex_out_t v) {
    return (v.clip_pos.z + v.clip_pos.w) >= 0.0f;
}

int chaos_gl_clip_near(gl_vertex_out_t in[3], gl_vertex_out_t out_verts[4], float z_near) {
    (void)z_near; /* We test in clip space: z + w >= 0 */

    gl_vertex_out_t poly[4];
    int poly_count = 0;

    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        bool i_in = near_inside(in[i]);
        bool j_in = near_inside(in[j]);

        if (i_in) {
            poly[poly_count++] = in[i];
            if (!j_in) {
                /* Edge exits: interpolate at boundary */
                float di = in[i].clip_pos.z + in[i].clip_pos.w;
                float dj = in[j].clip_pos.z + in[j].clip_pos.w;
                float t = di / (di - dj);
                poly[poly_count++] = lerp_vertex(in[i], in[j], t);
            }
        } else {
            if (j_in) {
                /* Edge enters: interpolate at boundary */
                float di = in[i].clip_pos.z + in[i].clip_pos.w;
                float dj = in[j].clip_pos.z + in[j].clip_pos.w;
                float t = di / (di - dj);
                poly[poly_count++] = lerp_vertex(in[i], in[j], t);
            }
        }
    }

    if (poly_count < 3) {
        return 0; /* All clipped away */
    }

    /* Output as triangle fan */
    out_verts[0] = poly[0];
    out_verts[1] = poly[1];
    out_verts[2] = poly[2];

    if (poly_count == 4) {
        out_verts[3] = poly[3];
        return 2; /* Quad: two triangles (0,1,2) and (0,2,3) */
    }

    return 1; /* Single triangle */
}

/* ── Guard-band reject ────────────────────────────────── */

bool chaos_gl_guardband_reject(gl_vertex_out_t v[3]) {
    float mult = CHAOS_GL_GUARD_BAND_MULT;

    /* Check each of 4 edges: if all 3 vertices are outside the same edge, reject */
    bool all_left   = true, all_right = true;
    bool all_bottom = true, all_top   = true;

    for (int i = 0; i < 3; i++) {
        float x = v[i].clip_pos.x;
        float y = v[i].clip_pos.y;
        float w = v[i].clip_pos.w;
        float mw = mult * w;

        if (x >= -mw) all_left   = false;
        if (x <=  mw) all_right  = false;
        if (y >= -mw) all_bottom = false;
        if (y <=  mw) all_top    = false;
    }

    return all_left || all_right || all_bottom || all_top;
}

/* ── Guard-band clip (v1: pass-through, rasterizer BB clamp handles it) ── */

int chaos_gl_guardband_clip(gl_vertex_out_t in[3], gl_vertex_out_t out_verts[8]) {
    out_verts[0] = in[0];
    out_verts[1] = in[1];
    out_verts[2] = in[2];
    return 3; /* 3 vertices = 1 triangle, pass through */
}

/* ── Check if any vertex is outside the guard band ────── */

static bool any_outside_guardband(gl_vertex_out_t v[3]) {
    float mult = CHAOS_GL_GUARD_BAND_MULT;
    for (int i = 0; i < 3; i++) {
        float x = v[i].clip_pos.x;
        float y = v[i].clip_pos.y;
        float w = v[i].clip_pos.w;
        float mw = mult * w;
        if (x < -mw || x > mw || y < -mw || y > mw)
            return true;
    }
    return false;
}

/* ── Emit a single triangle to the rasterizer ─────────── */

static void emit_triangle(chaos_gl_surface_t* s, gl_vertex_out_t tri[3]) {
    raster_vertex_t rv[3];
    rv[0] = to_raster(tri[0], s->width, s->height);
    rv[1] = to_raster(tri[1], s->width, s->height);
    rv[2] = to_raster(tri[2], s->width, s->height);
    rasterize_triangle(s, rv);
}

/* ── Main pipeline entry point ────────────────────────── */

void chaos_gl_triangle(gl_vertex_in_t v0, gl_vertex_in_t v1, gl_vertex_in_t v2) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s)
        return;

    s->stats.triangles_submitted++;

    /* Must have shaders bound */
    if (!s->active_vert || !s->active_frag)
        return;

    /* Run vertex shader on all 3 vertices */
    gl_vertex_out_t out[3];
    out[0] = s->active_vert(v0, s->active_uniforms);
    out[1] = s->active_vert(v1, s->active_uniforms);
    out[2] = s->active_vert(v2, s->active_uniforms);

    /* Backface cull in screen space (only if all w > 0) */
    if (out[0].clip_pos.w > 0.0f && out[1].clip_pos.w > 0.0f && out[2].clip_pos.w > 0.0f) {
        float w0 = out[0].clip_pos.w;
        float w1 = out[1].clip_pos.w;
        float w2 = out[2].clip_pos.w;

        float sx0 = (out[0].clip_pos.x / w0 + 1.0f) * (float)s->width  * 0.5f;
        float sy0 = (1.0f - out[0].clip_pos.y / w0)  * (float)s->height * 0.5f;
        float sx1 = (out[1].clip_pos.x / w1 + 1.0f) * (float)s->width  * 0.5f;
        float sy1 = (1.0f - out[1].clip_pos.y / w1)  * (float)s->height * 0.5f;
        float sx2 = (out[2].clip_pos.x / w2 + 1.0f) * (float)s->width  * 0.5f;
        float sy2 = (1.0f - out[2].clip_pos.y / w2)  * (float)s->height * 0.5f;

        /* Y is flipped in viewport transform (NDC up → screen down), so
         * front-facing triangles (CCW in world) have negative signed area
         * in screen space. Cull back-facing (positive signed area). */
        float signed_area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
        if (signed_area >= 0.0f) {
            s->stats.triangles_culled++;
            return;
        }
    }

    /* Near-plane clip */
    gl_vertex_out_t clipped[4];
    int num_tris = chaos_gl_clip_near(out, clipped, 0.0f);
    if (num_tris == 0)
        return;

    /* Process each triangle from near clip */
    for (int t = 0; t < num_tris; t++) {
        gl_vertex_out_t tri[3];
        if (t == 0) {
            tri[0] = clipped[0];
            tri[1] = clipped[1];
            tri[2] = clipped[2];
        } else {
            /* Second triangle of quad fan: 0, 2, 3 */
            tri[0] = clipped[0];
            tri[1] = clipped[2];
            tri[2] = clipped[3];
        }

        /* Guard-band reject */
        if (chaos_gl_guardband_reject(tri)) {
            s->stats.triangles_clipped++;
            continue;
        }

        /* Guard-band clip (v1: pass-through) */
        if (any_outside_guardband(tri)) {
            gl_vertex_out_t gb_out[8];
            int gb_count = chaos_gl_guardband_clip(tri, gb_out);

            /* gb_count is number of vertices; emit as triangle fan */
            for (int i = 1; i + 1 < gb_count; i++) {
                gl_vertex_out_t fan[3];
                fan[0] = gb_out[0];
                fan[1] = gb_out[i];
                fan[2] = gb_out[i + 1];
                emit_triangle(s, fan);
                s->stats.triangles_drawn++;
            }
        } else {
            emit_triangle(s, tri);
            s->stats.triangles_drawn++;
        }
    }
}
