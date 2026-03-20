#pragma once
#include "pipeline.h"
#include "texture.h"

/* Shader registry */
#define CHAOS_GL_MAX_SHADERS  32

int  chaos_gl_shader_register(const char* name, gl_vert_fn vert, gl_frag_fn frag);
int  chaos_gl_shader_set_by_name(const char* name, void* uniforms);
void chaos_gl_shader_set(gl_vert_fn vert, gl_frag_fn frag, void* uniforms);
void chaos_gl_shaders_init(void);

/* Built-in shader uniform structs */
typedef struct {
    uint32_t color;
} flat_uniforms_t;

typedef struct {
    int      tex_handle;
    vec3_t   light_dir;
    float    ambient;
} diffuse_uniforms_t;

typedef struct {
    vec3_t   light_dir;
    float    ambient;
    uint32_t color;
} gouraud_uniforms_t;

typedef struct {
    int      tex_handle;
    int      normalmap_handle;
    vec3_t   light_dir;
    float    ambient;
} normalmap_uniforms_t;

/* Built-in shader functions */
gl_vertex_out_t shader_flat_vert(gl_vertex_in_t in, void* uniforms);
gl_frag_out_t   shader_flat_frag(gl_fragment_in_t in, void* uniforms);

gl_vertex_out_t shader_diffuse_vert(gl_vertex_in_t in, void* uniforms);
gl_frag_out_t   shader_diffuse_frag(gl_fragment_in_t in, void* uniforms);

gl_vertex_out_t shader_gouraud_vert(gl_vertex_in_t in, void* uniforms);
gl_frag_out_t   shader_gouraud_frag(gl_fragment_in_t in, void* uniforms);

gl_vertex_out_t shader_normalmap_vert(gl_vertex_in_t in, void* uniforms);
gl_frag_out_t   shader_normalmap_frag(gl_fragment_in_t in, void* uniforms);
