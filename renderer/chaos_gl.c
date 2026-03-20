/* ChaosGL — Public API implementation (Phase 5) */

#include "chaos_gl.h"
#include "../drivers/framebuffer.h"
#include "../drivers/serial.h"
#include "../include/string.h"

int chaos_gl_init(void) {
    fb_info_t info;
    if (!fb_get_info(&info)) {
        serial_printf("[chaosgl] no framebuffer available\n");
        return -1;
    }

    chaos_gl_surface_init();
    chaos_gl_texture_init();
    chaos_gl_shaders_init();

    int r = chaos_gl_compositor_init();
    if (r < 0) {
        serial_printf("[chaosgl] compositor init failed\n");
        return -1;
    }

    serial_printf("[chaosgl] init: %ux%ux%u\n", info.width, info.height, info.bpp);
    return 0;
}

void chaos_gl_shutdown(void) {
    /* Destroy all surfaces */
    for (int i = 0; i < CHAOS_GL_MAX_SURFACES; i++) {
        chaos_gl_surface_t* s = chaos_gl_get_surface(i);
        if (s && s->in_use) {
            chaos_gl_surface_destroy(i);
        }
    }

    /* Free all textures */
    for (int i = 0; i < CHAOS_GL_MAX_TEXTURES; i++) {
        const chaos_gl_texture_t* t = chaos_gl_texture_get(i);
        if (t && t->in_use) {
            chaos_gl_texture_free(i);
        }
    }

    chaos_gl_compositor_shutdown();
}

/* ── Camera / Projection ───────────────────────────── */

void chaos_gl_set_camera(vec3_t eye, vec3_t center, vec3_t up) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->view = mat4_lookat(eye, center, up);
}

void chaos_gl_set_view(mat4_t view) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->view = view;
}

void chaos_gl_set_perspective(float fovy_deg, float aspect, float z_near, float z_far) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    if (aspect <= 0.0f) {
        aspect = (float)s->width / (float)s->height;
    }
    s->projection = mat4_perspective(fovy_deg * CHAOS_DEG2RAD, aspect, z_near, z_far);
}

void chaos_gl_set_projection(mat4_t proj) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->projection = proj;
}

/* ── Model Transform ───────────────────────────────── */

void chaos_gl_set_transform(float tx, float ty, float tz,
                             float rx, float ry, float rz,
                             float sx, float sy, float sz) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;

    /* Build: T * Ry * Rx * Rz * S (yaw-pitch-roll order) */
    mat4_t T = mat4_translate(tx, ty, tz);
    mat4_t Ry = mat4_rotate_y(ry);
    mat4_t Rx = mat4_rotate_x(rx);
    mat4_t Rz = mat4_rotate_z(rz);
    mat4_t S = mat4_scale(sx, sy, sz);

    s->model = mat4_mul(T, mat4_mul(Ry, mat4_mul(Rx, mat4_mul(Rz, S))));
}

void chaos_gl_set_model(mat4_t model) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;
    s->model = model;
}
