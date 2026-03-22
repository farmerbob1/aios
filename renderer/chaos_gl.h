/* ChaosGL — Public API (Phase 5)
 * Single include for all ChaosGL functionality. */

#pragma once

#include "math.h"
#include "surface.h"
#include "2d.h"
#include "compositor.h"
#include "pipeline.h"
#include "rasterizer.h"
#include "shaders.h"
#include "texture.h"
#include "model.h"
#include "ttf_font.h"

/* ── Lifecycle ─────────────────────────────────────── */

int  chaos_gl_init(void);
void chaos_gl_shutdown(void);

/* ── Camera and Projection (bound surface) ─────────── */

void chaos_gl_set_camera(vec3_t eye, vec3_t center, vec3_t up);
void chaos_gl_set_view(mat4_t view);
void chaos_gl_set_perspective(float fovy_deg, float aspect, float z_near, float z_far);
void chaos_gl_set_projection(mat4_t proj);

/* ── Model Transform (bound surface) ───────────────── */

void chaos_gl_set_transform(float tx, float ty, float tz,
                             float rx, float ry, float rz,
                             float sx, float sy, float sz);
void chaos_gl_set_model(mat4_t model);
