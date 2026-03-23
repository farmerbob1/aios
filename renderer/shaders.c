/* ChaosGL Built-in Shaders and Shader Registry */

#include "shaders.h"
#include "../include/string.h"
#include "../drivers/serial.h"

/* ── Shader registry ──────────────────────────────────── */

static struct {
    const char* name;
    gl_vert_fn  vert;
    gl_frag_fn  frag;
    bool        in_use;
} shader_registry[CHAOS_GL_MAX_SHADERS];

void chaos_gl_shaders_init(void) {
    memset(shader_registry, 0, sizeof(shader_registry));

    /* Register built-in shaders */
    chaos_gl_shader_register("flat",      shader_flat_vert,      shader_flat_frag);
    chaos_gl_shader_register("diffuse",   shader_diffuse_vert,   shader_diffuse_frag);
    chaos_gl_shader_register("gouraud",   shader_gouraud_vert,   shader_gouraud_frag);
    chaos_gl_shader_register("normalmap", shader_normalmap_vert, shader_normalmap_frag);
    chaos_gl_shader_register("sprite",    shader_sprite_vert,    shader_sprite_frag);
}

int chaos_gl_shader_register(const char* name, gl_vert_fn vert, gl_frag_fn frag) {
    for (int i = 0; i < CHAOS_GL_MAX_SHADERS; i++) {
        if (!shader_registry[i].in_use) {
            shader_registry[i].name   = name;
            shader_registry[i].vert   = vert;
            shader_registry[i].frag   = frag;
            shader_registry[i].in_use = true;
            return 0;
        }
    }
    serial_printf("[shaders] registry full, cannot register '%s'\n", name);
    return -1;
}

int chaos_gl_shader_set_by_name(const char* name, void* uniforms) {
    for (int i = 0; i < CHAOS_GL_MAX_SHADERS; i++) {
        if (shader_registry[i].in_use &&
            strcmp(shader_registry[i].name, name) == 0) {
            chaos_gl_shader_set(shader_registry[i].vert,
                                shader_registry[i].frag,
                                uniforms);
            return 0;
        }
    }
    serial_printf("[shaders] shader '%s' not found\n", name);
    return -1;
}

void chaos_gl_shader_set(gl_vert_fn vert, gl_frag_fn frag, void* uniforms) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) {
        serial_printf("[shaders] no surface bound\n");
        return;
    }
    s->active_vert     = vert;
    s->active_frag     = frag;
    s->active_uniforms = uniforms;
}

/* ── Helper: get MVP from bound surface ───────────────── */

static mat4_t get_mvp(void) {
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return mat4_identity();
    return mat4_mul(s->projection, mat4_mul(s->view, s->model));
}

/* ── Flat shader ──────────────────────────────────────── */

gl_vertex_out_t shader_flat_vert(gl_vertex_in_t in, void* uniforms) {
    (void)uniforms;
    gl_vertex_out_t out;
    mat4_t mvp = get_mvp();
    out.clip_pos  = mat4_mul_vec4(mvp, vec4_from_vec3(in.position, 1.0f));
    out.normal    = in.normal;
    out.uv        = in.uv;
    out.intensity = 1.0f;
    return out;
}

gl_frag_out_t shader_flat_frag(gl_fragment_in_t in, void* uniforms) {
    (void)in;
    flat_uniforms_t* u = (flat_uniforms_t*)uniforms;
    return GL_COLOR(u->color);
}

/* ── Gouraud shader ───────────────────────────────────── */

gl_vertex_out_t shader_gouraud_vert(gl_vertex_in_t in, void* uniforms) {
    gouraud_uniforms_t* u = (gouraud_uniforms_t*)uniforms;
    gl_vertex_out_t out;

    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    mat4_t mvp = get_mvp();
    out.clip_pos = mat4_mul_vec4(mvp, vec4_from_vec3(in.position, 1.0f));

    /* Transform normal to view space */
    mat4_t modelview = s ? mat4_mul(s->view, s->model) : mat4_identity();
    vec4_t vn = mat4_mul_vec4(modelview, vec4_from_vec3(in.normal, 0.0f));
    vec3_t view_normal = vec3_normalize((vec3_t){ vn.x, vn.y, vn.z });

    /* Per-vertex lighting */
    vec3_t light = vec3_normalize(u->light_dir);
    float ndotl = vec3_dot(view_normal, light);
    out.intensity = chaos_maxf(ndotl, u->ambient);

    out.normal = view_normal;
    out.uv     = in.uv;
    return out;
}

gl_frag_out_t shader_gouraud_frag(gl_fragment_in_t in, void* uniforms) {
    gouraud_uniforms_t* u = (gouraud_uniforms_t*)uniforms;

    uint8_t b = u->color & 0xFF;
    uint8_t g = (u->color >> 8) & 0xFF;
    uint8_t r = (u->color >> 16) & 0xFF;

    int ri = (int)(r * in.intensity);
    int gi = (int)(g * in.intensity);
    int bi = (int)(b * in.intensity);

    if (ri > 255) ri = 255;
    if (gi > 255) gi = 255;
    if (bi > 255) bi = 255;

    return GL_COLOR(CHAOS_GL_RGB(ri, gi, bi));
}

/* ── Diffuse shader ───────────────────────────────────── */

gl_vertex_out_t shader_diffuse_vert(gl_vertex_in_t in, void* uniforms) {
    (void)uniforms;
    gl_vertex_out_t out;

    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    mat4_t mvp = get_mvp();
    out.clip_pos = mat4_mul_vec4(mvp, vec4_from_vec3(in.position, 1.0f));

    /* Transform normal to view space */
    mat4_t modelview = s ? mat4_mul(s->view, s->model) : mat4_identity();
    vec4_t vn = mat4_mul_vec4(modelview, vec4_from_vec3(in.normal, 0.0f));
    out.normal = vec3_normalize((vec3_t){ vn.x, vn.y, vn.z });

    out.uv        = in.uv;
    out.intensity  = 0.0f;  /* computed per-pixel in fragment shader */
    return out;
}

gl_frag_out_t shader_diffuse_frag(gl_fragment_in_t in, void* uniforms) {
    diffuse_uniforms_t* u = (diffuse_uniforms_t*)uniforms;

    /* Sample the diffuse texture */
    const chaos_gl_texture_t* tex = chaos_gl_texture_get(u->tex_handle);
    if (!tex) return GL_COLOR(0xFF00FF);  /* magenta = missing texture */

    uint32_t tex_color = chaos_gl_tex_sample(tex, in.uv.x, in.uv.y);

    /* Per-pixel lighting */
    vec3_t n = vec3_normalize(in.normal);
    vec3_t l = vec3_normalize(u->light_dir);
    float ndotl = vec3_dot(n, l);
    float light = chaos_maxf(ndotl, u->ambient);

    /* Scale texture color by lighting */
    uint8_t b = tex_color & 0xFF;
    uint8_t g = (tex_color >> 8) & 0xFF;
    uint8_t r = (tex_color >> 16) & 0xFF;

    int ri = (int)(r * light); if (ri > 255) ri = 255;
    int gi = (int)(g * light); if (gi > 255) gi = 255;
    int bi = (int)(b * light); if (bi > 255) bi = 255;

    return GL_COLOR(CHAOS_GL_RGB(ri, gi, bi));
}

/* ── Normal map shader ────────────────────────────────── */

gl_vertex_out_t shader_normalmap_vert(gl_vertex_in_t in, void* uniforms) {
    /* Same as diffuse vertex shader */
    (void)uniforms;
    gl_vertex_out_t out;

    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    mat4_t mvp = get_mvp();
    out.clip_pos = mat4_mul_vec4(mvp, vec4_from_vec3(in.position, 1.0f));

    mat4_t modelview = s ? mat4_mul(s->view, s->model) : mat4_identity();
    vec4_t vn = mat4_mul_vec4(modelview, vec4_from_vec3(in.normal, 0.0f));
    out.normal = vec3_normalize((vec3_t){ vn.x, vn.y, vn.z });

    out.uv        = in.uv;
    out.intensity  = 0.0f;
    return out;
}

gl_frag_out_t shader_normalmap_frag(gl_fragment_in_t in, void* uniforms) {
    normalmap_uniforms_t* u = (normalmap_uniforms_t*)uniforms;

    /* Sample the diffuse texture for base color */
    const chaos_gl_texture_t* diffuse_tex = chaos_gl_texture_get(u->tex_handle);
    if (!diffuse_tex) return GL_COLOR(0xFF00FF);  /* magenta = missing texture */

    uint32_t base_color = chaos_gl_tex_sample(diffuse_tex, in.uv.x, in.uv.y);

    /* Sample the normal map */
    const chaos_gl_texture_t* nmap = chaos_gl_texture_get(u->normalmap_handle);
    vec3_t sampled_normal;
    if (nmap) {
        uint32_t nm_sample = chaos_gl_tex_sample(nmap, in.uv.x, in.uv.y);
        /* Decode from [0,255] to [-1,1] for each channel */
        float nm_b = (float)(nm_sample & 0xFF);
        float nm_g = (float)((nm_sample >> 8) & 0xFF);
        float nm_r = (float)((nm_sample >> 16) & 0xFF);
        sampled_normal.x = (nm_r / 255.0f) * 2.0f - 1.0f;
        sampled_normal.y = (nm_g / 255.0f) * 2.0f - 1.0f;
        sampled_normal.z = (nm_b / 255.0f) * 2.0f - 1.0f;
        sampled_normal = vec3_normalize(sampled_normal);
    } else {
        /* No normal map — fall back to interpolated surface normal */
        sampled_normal = vec3_normalize(in.normal);
    }

    /* Compute lighting with the sampled normal */
    vec3_t l = vec3_normalize(u->light_dir);
    float ndotl = vec3_dot(sampled_normal, l);
    float light = chaos_maxf(ndotl, u->ambient);

    /* Apply lighting to base color */
    uint8_t b = base_color & 0xFF;
    uint8_t g = (base_color >> 8) & 0xFF;
    uint8_t r = (base_color >> 16) & 0xFF;

    int ri = (int)(r * light); if (ri > 255) ri = 255;
    int gi = (int)(g * light); if (gi > 255) gi = 255;
    int bi = (int)(b * light); if (bi > 255) bi = 255;

    return GL_COLOR(CHAOS_GL_RGB(ri, gi, bi));
}

/* ── Sprite shader ───────────────────────────────────── */

gl_vertex_out_t shader_sprite_vert(gl_vertex_in_t in, void* uniforms) {
    (void)uniforms;
    gl_vertex_out_t out;
    mat4_t mvp = get_mvp();
    out.clip_pos  = mat4_mul_vec4(mvp, vec4_from_vec3(in.position, 1.0f));
    out.normal    = in.normal;
    out.uv        = in.uv;
    out.intensity = 1.0f;
    return out;
}

gl_frag_out_t shader_sprite_frag(gl_fragment_in_t in, void* uniforms) {
    sprite_uniforms_t* u = (sprite_uniforms_t*)uniforms;

    const chaos_gl_texture_t* tex = chaos_gl_texture_get(u->tex_handle);
    if (!tex) return GL_DISCARD;

    /* Remap UV to sub-rect in sprite sheet */
    float su = u->u0 + in.uv.x * (u->u1 - u->u0);
    float sv = u->v0 + in.uv.y * (u->v1 - u->v0);
    uint32_t texel = chaos_gl_tex_sample(tex, su, sv);

    if (texel == u->key_color) return GL_DISCARD;

    uint8_t b = texel & 0xFF;
    uint8_t g = (texel >> 8) & 0xFF;
    uint8_t r = (texel >> 16) & 0xFF;
    float l = u->light;

    int ri = (int)(r * l); if (ri > 255) ri = 255;
    int gi = (int)(g * l); if (gi > 255) gi = 255;
    int bi = (int)(b * l); if (bi > 255) bi = 255;

    return GL_COLOR(CHAOS_GL_RGB(ri, gi, bi));
}
