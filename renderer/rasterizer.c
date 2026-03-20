/* ChaosGL Rasterizer — edge-function triangle rasterization (Phase 5) */

#include "rasterizer.h"
#include "../include/string.h"

void rasterize_triangle(chaos_gl_surface_t* s, raster_vertex_t v[3]) {
    /* Compute bounding box */
    float min_sx = v[0].sx;
    float max_sx = v[0].sx;
    float min_sy = v[0].sy;
    float max_sy = v[0].sy;

    for (int i = 1; i < 3; i++) {
        if (v[i].sx < min_sx) min_sx = v[i].sx;
        if (v[i].sx > max_sx) max_sx = v[i].sx;
        if (v[i].sy < min_sy) min_sy = v[i].sy;
        if (v[i].sy > max_sy) max_sy = v[i].sy;
    }

    /* Clamp BB to surface dimensions */
    int x0 = (int)chaos_floorf(min_sx);
    int y0 = (int)chaos_floorf(min_sy);
    int x1 = (int)chaos_floorf(max_sx) + 1;
    int y1 = (int)chaos_floorf(max_sy) + 1;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width)  x1 = s->width;
    if (y1 > s->height) y1 = s->height;

    if (x0 >= x1 || y0 >= y1)
        return;

    /* Get back buffer (app draws to bufs[1 - buf_index]) */
    uint32_t* fb = s->bufs[1 - s->buf_index];
    if (!fb)
        return;

    /* Rasterize with edge functions */
    for (int py = y0; py < y1; py++) {
        float pyc = (float)py + 0.5f; /* pixel center y */

        for (int px = x0; px < x1; px++) {
            float pxc = (float)px + 0.5f; /* pixel center x */

            /* Edge functions (barycentric coordinates) */
            float w0 = (v[1].sy - v[2].sy) * (pxc - v[2].sx) -
                        (v[1].sx - v[2].sx) * (pyc - v[2].sy);
            float w1 = (v[2].sy - v[0].sy) * (pxc - v[0].sx) -
                        (v[2].sx - v[0].sx) * (pyc - v[0].sy);
            float w2 = (v[0].sy - v[1].sy) * (pxc - v[1].sx) -
                        (v[0].sx - v[1].sx) * (pyc - v[1].sy);

            /* Inside test: all same sign (handles both CW and CCW winding).
             * After Y-flip, front-facing triangles have negative edge functions. */
            bool all_pos = (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f);
            bool all_neg = (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
            if (!all_pos && !all_neg)
                continue;

            /* Normalize barycentric coords (use absolute values) */
            float area = w0 + w1 + w2;
            if (area == 0.0f)
                continue;
            if (area < 0.0f) {
                w0 = -w0; w1 = -w1; w2 = -w2;
                area = -area;
            }

            w0 /= area;
            w1 /= area;
            w2 /= area;

            /* Perspective-correct interpolation */
            float inv_w = w0 * v[0].inv_w + w1 * v[1].inv_w + w2 * v[2].inv_w;
            float corr = 1.0f / inv_w;

            float u = (w0 * v[0].uv.x * v[0].inv_w +
                       w1 * v[1].uv.x * v[1].inv_w +
                       w2 * v[2].uv.x * v[2].inv_w) * corr;
            float v_coord = (w0 * v[0].uv.y * v[0].inv_w +
                             w1 * v[1].uv.y * v[1].inv_w +
                             w2 * v[2].uv.y * v[2].inv_w) * corr;

            float nx = (w0 * v[0].normal.x * v[0].inv_w +
                        w1 * v[1].normal.x * v[1].inv_w +
                        w2 * v[2].normal.x * v[2].inv_w) * corr;
            float ny = (w0 * v[0].normal.y * v[0].inv_w +
                        w1 * v[1].normal.y * v[1].inv_w +
                        w2 * v[2].normal.y * v[2].inv_w) * corr;
            float nz = (w0 * v[0].normal.z * v[0].inv_w +
                        w1 * v[1].normal.z * v[1].inv_w +
                        w2 * v[2].normal.z * v[2].inv_w) * corr;

            float intensity = (w0 * v[0].intensity * v[0].inv_w +
                               w1 * v[1].intensity * v[1].inv_w +
                               w2 * v[2].intensity * v[2].inv_w) * corr;

            /* Depth is NOT perspective-corrected (screen-space is fine for z-buffer) */
            float ndc_z = w0 * v[0].ndc_z + w1 * v[1].ndc_z + w2 * v[2].ndc_z;

            /* Convert NDC depth to 16-bit z-buffer value */
            float z_float = ndc_z * 0.5f + 0.5f;
            if (z_float < 0.0f) z_float = 0.0f;
            if (z_float > 1.0f) z_float = 1.0f;
            uint16_t z = (uint16_t)(z_float * 65535.0f);

            int zidx = py * s->width + px;

            /* Build fragment input */
            gl_fragment_in_t frag_in;
            frag_in.uv.x = u;
            frag_in.uv.y = v_coord;
            frag_in.normal.x = nx;
            frag_in.normal.y = ny;
            frag_in.normal.z = nz;
            frag_in.intensity = intensity;
            frag_in.x = px;
            frag_in.y = py;

            /* Call fragment shader */
            gl_frag_out_t frag = s->active_frag(frag_in, s->active_uniforms);

            /* Discard check */
            if (frag.discard) {
                s->stats.pixels_discarded++;
                continue;
            }

            /* Z-test (late-z): lower z value is closer */
            if (s->zbuffer) {
                if (z >= s->zbuffer[zidx]) {
                    s->stats.pixels_zfailed++;
                    continue;
                }
                s->zbuffer[zidx] = z;
            }

            /* Write pixel to back buffer */
            fb[zidx] = frag.color;
            s->stats.pixels_written++;
        }
    }
}
