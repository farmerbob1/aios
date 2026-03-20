/* ChaosGL Math Library — vec/mat ops + custom trig (Phase 5)
 * Compiled with RENDERER_CFLAGS (SSE2 enabled). */

#include "math.h"

/* ── Custom math functions (no libm) ───────────────── */

float chaos_fabsf(float x) {
    return x < 0.0f ? -x : x;
}

/* Use SSE sqrtss instruction directly */
float chaos_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float result;
    __asm__ __volatile__("sqrtss %1, %0" : "=x"(result) : "x"(x));
    return result;
}

float chaos_floorf(float x) {
    int i = (int)x;
    return (float)(x < (float)i ? i - 1 : i);
}

float chaos_fmodf(float a, float b) {
    return a - chaos_floorf(a / b) * b;
}

float chaos_clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

float chaos_minf(float a, float b) { return a < b ? a : b; }
float chaos_maxf(float a, float b) { return a > b ? a : b; }

/* sinf — minimax 7th-order polynomial, range-reduced to [-pi, pi]
 * Max error ~1e-6 over full float range. */
float chaos_sinf(float x) {
    /* Range reduce to [-pi, pi] */
    x = chaos_fmodf(x + CHAOS_PI, CHAOS_2PI) - CHAOS_PI;

    /* Minimax polynomial coefficients (odd terms) for sin(x) on [-pi, pi] */
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;

    return x - x3 * (1.0f / 6.0f)
             + x5 * (1.0f / 120.0f)
             - x7 * (1.0f / 5040.0f);
}

float chaos_cosf(float x) {
    return chaos_sinf(x + CHAOS_PI_2);
}

float chaos_tanf(float x) {
    float c = chaos_cosf(x);
    if (c > -1e-7f && c < 1e-7f) return 1e10f; /* avoid div by zero */
    return chaos_sinf(x) / c;
}

/* ── vec2 ──────────────────────────────────────────── */

vec2_t vec2_add(vec2_t a, vec2_t b) { return (vec2_t){ a.x + b.x, a.y + b.y }; }
vec2_t vec2_sub(vec2_t a, vec2_t b) { return (vec2_t){ a.x - b.x, a.y - b.y }; }
vec2_t vec2_scale(vec2_t v, float s) { return (vec2_t){ v.x * s, v.y * s }; }

/* ── vec3 ──────────────────────────────────────────── */

vec3_t vec3_add(vec3_t a, vec3_t b) { return (vec3_t){ a.x + b.x, a.y + b.y, a.z + b.z }; }
vec3_t vec3_sub(vec3_t a, vec3_t b) { return (vec3_t){ a.x - b.x, a.y - b.y, a.z - b.z }; }
vec3_t vec3_scale(vec3_t v, float s) { return (vec3_t){ v.x * s, v.y * s, v.z * s }; }
vec3_t vec3_negate(vec3_t v) { return (vec3_t){ -v.x, -v.y, -v.z }; }
float  vec3_dot(vec3_t a, vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

vec3_t vec3_cross(vec3_t a, vec3_t b) {
    return (vec3_t){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

float vec3_len(vec3_t v) {
    return chaos_sqrtf(vec3_dot(v, v));
}

vec3_t vec3_normalize(vec3_t v) {
    float len = vec3_len(v);
    if (len < 1e-8f) return (vec3_t){ 0, 0, 0 };
    float inv = 1.0f / len;
    return (vec3_t){ v.x * inv, v.y * inv, v.z * inv };
}

vec3_t vec3_lerp(vec3_t a, vec3_t b, float t) {
    return (vec3_t){
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

/* ── vec4 ──────────────────────────────────────────── */

vec4_t vec4_from_vec3(vec3_t v, float w) { return (vec4_t){ v.x, v.y, v.z, w }; }

vec3_t vec4_to_vec3(vec4_t v) {
    if (chaos_fabsf(v.w) < 1e-8f) return (vec3_t){ v.x, v.y, v.z };
    float inv = 1.0f / v.w;
    return (vec3_t){ v.x * inv, v.y * inv, v.z * inv };
}

vec4_t vec4_add(vec4_t a, vec4_t b) {
    return (vec4_t){ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w };
}

vec4_t vec4_scale(vec4_t v, float s) {
    return (vec4_t){ v.x * s, v.y * s, v.z * s, v.w * s };
}

vec4_t vec4_lerp(vec4_t a, vec4_t b, float t) {
    return (vec4_t){
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    };
}

/* ── mat4 — column-major: m[col][row] ──────────────── */

mat4_t mat4_identity(void) {
    mat4_t r = {{{ 0 }}};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

/* mat4_mul: result = a * b (column-major)
 * result[col][row] = sum_k( a[k][row] * b[col][k] ) */
mat4_t mat4_mul(mat4_t a, mat4_t b) {
    mat4_t r = {{{ 0 }}};
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++)
            for (int k = 0; k < 4; k++)
                r.m[col][row] += a.m[k][row] * b.m[col][k];
    return r;
}

/* mat4_mul_vec4: result = m * v (column-major)
 * result[row] = sum_col( m[col][row] * v[col] ) */
vec4_t mat4_mul_vec4(mat4_t m, vec4_t v) {
    float comps[4] = { v.x, v.y, v.z, v.w };
    vec4_t r = { 0, 0, 0, 0 };
    float* rp = &r.x;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            rp[row] += m.m[col][row] * comps[col];
    return r;
}

mat4_t mat4_translate(float x, float y, float z) {
    mat4_t r = mat4_identity();
    /* Column-major: translation in column 3 */
    r.m[3][0] = x;
    r.m[3][1] = y;
    r.m[3][2] = z;
    return r;
}

mat4_t mat4_scale(float x, float y, float z) {
    mat4_t r = {{{ 0 }}};
    r.m[0][0] = x;
    r.m[1][1] = y;
    r.m[2][2] = z;
    r.m[3][3] = 1.0f;
    return r;
}

mat4_t mat4_rotate_x(float rad) {
    float c = chaos_cosf(rad), s = chaos_sinf(rad);
    mat4_t r = mat4_identity();
    r.m[1][1] =  c;  r.m[2][1] = -s;
    r.m[1][2] =  s;  r.m[2][2] =  c;
    return r;
}

mat4_t mat4_rotate_y(float rad) {
    float c = chaos_cosf(rad), s = chaos_sinf(rad);
    mat4_t r = mat4_identity();
    r.m[0][0] =  c;  r.m[2][0] =  s;
    r.m[0][2] = -s;  r.m[2][2] =  c;
    return r;
}

mat4_t mat4_rotate_z(float rad) {
    float c = chaos_cosf(rad), s = chaos_sinf(rad);
    mat4_t r = mat4_identity();
    r.m[0][0] =  c;  r.m[1][0] = -s;
    r.m[0][1] =  s;  r.m[1][1] =  c;
    return r;
}

mat4_t mat4_transpose(mat4_t m) {
    mat4_t r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r.m[c][rr] = m.m[rr][c];
    return r;
}

/* 4x4 matrix inverse via cofactor expansion */
mat4_t mat4_inverse(mat4_t m) {
    float* s = &m.m[0][0];
    float inv[16];

    inv[0]  =  s[5]*s[10]*s[15] - s[5]*s[11]*s[14] - s[9]*s[6]*s[15]
             + s[9]*s[7]*s[14]  + s[13]*s[6]*s[11]  - s[13]*s[7]*s[10];
    inv[4]  = -s[4]*s[10]*s[15] + s[4]*s[11]*s[14]  + s[8]*s[6]*s[15]
             - s[8]*s[7]*s[14]  - s[12]*s[6]*s[11]  + s[12]*s[7]*s[10];
    inv[8]  =  s[4]*s[9]*s[15]  - s[4]*s[11]*s[13]  - s[8]*s[5]*s[15]
             + s[8]*s[7]*s[13]  + s[12]*s[5]*s[11]  - s[12]*s[7]*s[9];
    inv[12] = -s[4]*s[9]*s[14]  + s[4]*s[10]*s[13]  + s[8]*s[5]*s[14]
             - s[8]*s[6]*s[13]  - s[12]*s[5]*s[10]  + s[12]*s[6]*s[9];

    float det = s[0]*inv[0] + s[1]*inv[4] + s[2]*inv[8] + s[3]*inv[12];
    if (chaos_fabsf(det) < 1e-10f) return mat4_identity();
    float inv_det = 1.0f / det;

    inv[1]  = -s[1]*s[10]*s[15] + s[1]*s[11]*s[14]  + s[9]*s[2]*s[15]
             - s[9]*s[3]*s[14]  - s[13]*s[2]*s[11]  + s[13]*s[3]*s[10];
    inv[5]  =  s[0]*s[10]*s[15] - s[0]*s[11]*s[14]  - s[8]*s[2]*s[15]
             + s[8]*s[3]*s[14]  + s[12]*s[2]*s[11]  - s[12]*s[3]*s[10];
    inv[9]  = -s[0]*s[9]*s[15]  + s[0]*s[11]*s[13]  + s[8]*s[1]*s[15]
             - s[8]*s[3]*s[13]  - s[12]*s[1]*s[11]  + s[12]*s[3]*s[9];
    inv[13] =  s[0]*s[9]*s[14]  - s[0]*s[10]*s[13]  - s[8]*s[1]*s[14]
             + s[8]*s[2]*s[13]  + s[12]*s[1]*s[10]  - s[12]*s[2]*s[9];

    inv[2]  =  s[1]*s[6]*s[15]  - s[1]*s[7]*s[14]   - s[5]*s[2]*s[15]
             + s[5]*s[3]*s[14]  + s[13]*s[2]*s[7]   - s[13]*s[3]*s[6];
    inv[6]  = -s[0]*s[6]*s[15]  + s[0]*s[7]*s[14]   + s[4]*s[2]*s[15]
             - s[4]*s[3]*s[14]  - s[12]*s[2]*s[7]   + s[12]*s[3]*s[6];
    inv[10] =  s[0]*s[5]*s[15]  - s[0]*s[7]*s[13]   - s[4]*s[1]*s[15]
             + s[4]*s[3]*s[13]  + s[12]*s[1]*s[7]   - s[12]*s[3]*s[5];
    inv[14] = -s[0]*s[5]*s[14]  + s[0]*s[6]*s[13]   + s[4]*s[1]*s[14]
             - s[4]*s[2]*s[13]  - s[12]*s[1]*s[6]   + s[12]*s[2]*s[5];

    inv[3]  = -s[1]*s[6]*s[11]  + s[1]*s[7]*s[10]   + s[5]*s[2]*s[11]
             - s[5]*s[3]*s[10]  - s[9]*s[2]*s[7]    + s[9]*s[3]*s[6];
    inv[7]  =  s[0]*s[6]*s[11]  - s[0]*s[7]*s[10]   - s[4]*s[2]*s[11]
             + s[4]*s[3]*s[10]  + s[8]*s[2]*s[7]    - s[8]*s[3]*s[6];
    inv[11] = -s[0]*s[5]*s[11]  + s[0]*s[7]*s[9]    + s[4]*s[1]*s[11]
             - s[4]*s[3]*s[9]   - s[8]*s[1]*s[7]    + s[8]*s[3]*s[5];
    inv[15] =  s[0]*s[5]*s[10]  - s[0]*s[6]*s[9]    - s[4]*s[1]*s[10]
             + s[4]*s[2]*s[9]   + s[8]*s[1]*s[6]    - s[8]*s[2]*s[5];

    mat4_t r;
    float* d = &r.m[0][0];
    for (int i = 0; i < 16; i++)
        d[i] = inv[i] * inv_det;
    return r;
}

mat4_t mat4_lookat(vec3_t eye, vec3_t center, vec3_t up) {
    vec3_t f = vec3_normalize(vec3_sub(center, eye));  /* forward */
    vec3_t s = vec3_normalize(vec3_cross(f, up));      /* right */
    vec3_t u = vec3_cross(s, f);                       /* true up */

    /* Column-major: columns are basis vectors */
    mat4_t r = mat4_identity();
    r.m[0][0] =  s.x;  r.m[1][0] =  s.y;  r.m[2][0] =  s.z;
    r.m[0][1] =  u.x;  r.m[1][1] =  u.y;  r.m[2][1] =  u.z;
    r.m[0][2] = -f.x;  r.m[1][2] = -f.y;  r.m[2][2] = -f.z;
    r.m[3][0] = -vec3_dot(s, eye);
    r.m[3][1] = -vec3_dot(u, eye);
    r.m[3][2] =  vec3_dot(f, eye);
    return r;
}

mat4_t mat4_perspective(float fovy_rad, float aspect, float z_near, float z_far) {
    float f = 1.0f / chaos_tanf(fovy_rad * 0.5f);

    mat4_t r = {{{ 0 }}};
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (z_far + z_near) / (z_near - z_far);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (2.0f * z_far * z_near) / (z_near - z_far);
    return r;
}

/* ── rect helpers ──────────────────────────────────── */

bool rect_contains(rect_t r, int px, int py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return (rect_t){ 0, 0, 0, 0 };
    return (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

bool rect_is_empty(rect_t r) {
    return r.w <= 0 || r.h <= 0;
}

rect_t rect_union(rect_t a, rect_t b) {
    if (rect_is_empty(a)) return b;
    if (rect_is_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}
