/* ChaosGL Model Loading — .cobj binary format (Phase 5) */

#pragma once

#include "pipeline.h"

#define COBJ_MAGIC    0x434F424A  /* 'COBJ' */
#define COBJ_VERSION  1

#define CHAOS_GL_MAX_MODEL_VERTS  65536
#define CHAOS_GL_MAX_MODEL_FACES  131072

struct cobj_header {
    uint32_t magic;
    uint32_t version;
    uint32_t vertex_count;
    uint32_t normal_count;
    uint32_t uv_count;
    uint32_t face_count;
    uint32_t vertex_offset;
    uint32_t normal_offset;
    uint32_t uv_offset;
    uint32_t face_offset;
} __attribute__((packed));

struct cobj_vertex { float x, y, z; } __attribute__((packed));
struct cobj_normal { float x, y, z; } __attribute__((packed));
struct cobj_uv     { float u, v;    } __attribute__((packed));
struct cobj_face   {
    uint32_t v[3];
    uint32_t n[3];
    uint32_t t[3];
} __attribute__((packed));

typedef struct {
    struct cobj_vertex* vertices;
    struct cobj_normal* normals;
    struct cobj_uv*     uvs;
    struct cobj_face*   faces;
    uint32_t vertex_count;
    uint32_t normal_count;
    uint32_t uv_count;
    uint32_t face_count;
} chaos_gl_model_t;

chaos_gl_model_t* chaos_gl_model_load(const char* path);
void              chaos_gl_model_free(chaos_gl_model_t* model);
void              chaos_gl_draw_model(chaos_gl_model_t* model);
void              chaos_gl_draw_model_wire(chaos_gl_model_t* model, uint32_t color);
