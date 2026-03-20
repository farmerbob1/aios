/* ChaosGL Math Library — vec2/3/4, mat4, rect, trig (Phase 5) */

#pragma once

#include "../include/types.h"

/* ── Constants ─────────────────────────────────────── */

#define CHAOS_PI       3.14159265358979323846f
#define CHAOS_PI_2     1.57079632679489661923f
#define CHAOS_2PI      6.28318530717958647692f
#define CHAOS_DEG2RAD  (CHAOS_PI / 180.0f)
#define CHAOS_RAD2DEG  (180.0f / CHAOS_PI)

/* ── Custom math (no libm) ─────────────────────────── */

float chaos_fabsf(float x);
float chaos_sqrtf(float x);
float chaos_sinf(float x);
float chaos_cosf(float x);
float chaos_tanf(float x);
float chaos_floorf(float x);
float chaos_fmodf(float a, float b);
float chaos_clampf(float x, float lo, float hi);
float chaos_minf(float a, float b);
float chaos_maxf(float a, float b);

/* ── Vector types ──────────────────────────────────── */

typedef struct { float x, y; }       vec2_t;
typedef struct { float x, y, z; }    vec3_t;
typedef struct { float x, y, z, w; } vec4_t;
typedef struct { int   x, y; }       vec2i_t;
typedef struct { int   x, y, w, h; } rect_t;

/* ── Matrix — column-major m[col][row] ─────────────── */

typedef struct { float m[4][4]; } mat4_t;

/* ── vec2 ops ──────────────────────────────────────── */

vec2_t  vec2_add(vec2_t a, vec2_t b);
vec2_t  vec2_sub(vec2_t a, vec2_t b);
vec2_t  vec2_scale(vec2_t v, float s);

/* ── vec3 ops ──────────────────────────────────────── */

vec3_t  vec3_add(vec3_t a, vec3_t b);
vec3_t  vec3_sub(vec3_t a, vec3_t b);
vec3_t  vec3_scale(vec3_t v, float s);
vec3_t  vec3_negate(vec3_t v);
float   vec3_dot(vec3_t a, vec3_t b);
vec3_t  vec3_cross(vec3_t a, vec3_t b);
float   vec3_len(vec3_t v);
vec3_t  vec3_normalize(vec3_t v);
vec3_t  vec3_lerp(vec3_t a, vec3_t b, float t);

/* ── vec4 ops ──────────────────────────────────────── */

vec4_t  vec4_from_vec3(vec3_t v, float w);
vec3_t  vec4_to_vec3(vec4_t v);       /* xyz / w */
vec4_t  vec4_add(vec4_t a, vec4_t b);
vec4_t  vec4_scale(vec4_t v, float s);
vec4_t  vec4_lerp(vec4_t a, vec4_t b, float t);

/* ── mat4 ops ──────────────────────────────────────── */

mat4_t  mat4_identity(void);
mat4_t  mat4_mul(mat4_t a, mat4_t b);
vec4_t  mat4_mul_vec4(mat4_t m, vec4_t v);
mat4_t  mat4_translate(float x, float y, float z);
mat4_t  mat4_scale(float x, float y, float z);
mat4_t  mat4_rotate_x(float radians);
mat4_t  mat4_rotate_y(float radians);
mat4_t  mat4_rotate_z(float radians);
mat4_t  mat4_transpose(mat4_t m);
mat4_t  mat4_inverse(mat4_t m);
mat4_t  mat4_lookat(vec3_t eye, vec3_t center, vec3_t up);
mat4_t  mat4_perspective(float fovy_rad, float aspect, float z_near, float z_far);

/* ── rect helpers ──────────────────────────────────── */

bool    rect_contains(rect_t r, int px, int py);
rect_t  rect_intersect(rect_t a, rect_t b);
bool    rect_is_empty(rect_t r);
rect_t  rect_union(rect_t a, rect_t b);
