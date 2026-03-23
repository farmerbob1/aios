/* ChaosGL Model Loading — .cobj binary format (Phase 5) */

#include "model.h"
#include "surface.h"
#include "2d.h"
#include "../kernel/heap.h"
#include "../kernel/chaos/chaos.h"
#include "../drivers/serial.h"
#include "../include/string.h"

/* Helper: read exactly `len` bytes from fd, handling partial reads */
static int read_full(int fd, void* buf, uint32_t len) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t remaining = len;
    while (remaining > 0) {
        int r = chaos_read(fd, p, remaining);
        if (r <= 0) return -1;
        p += r;
        remaining -= (uint32_t)r;
    }
    return (int)len;
}

chaos_gl_model_t* chaos_gl_model_load(const char* path) {
    int fd = chaos_open(path, 0x01); /* CHAOS_O_RDONLY */
    if (fd < 0) {
        serial_printf("[model] open '%s' failed: %d\n", path, fd);
        return NULL;
    }

    struct cobj_header hdr;
    if (read_full(fd, &hdr, sizeof(hdr)) < 0) {
        serial_printf("[model] read header failed\n");
        chaos_close(fd);
        return NULL;
    }

    if (hdr.magic != COBJ_MAGIC || hdr.version != COBJ_VERSION) {
        serial_printf("[model] bad magic/version: 0x%x/%u\n", hdr.magic, hdr.version);
        chaos_close(fd);
        return NULL;
    }

    if (hdr.vertex_count > CHAOS_GL_MAX_MODEL_VERTS ||
        hdr.face_count > CHAOS_GL_MAX_MODEL_FACES ||
        hdr.vertex_count == 0 || hdr.face_count == 0) {
        serial_printf("[model] bad counts: v=%u f=%u\n", hdr.vertex_count, hdr.face_count);
        chaos_close(fd);
        return NULL;
    }

    chaos_gl_model_t* m = (chaos_gl_model_t*)kmalloc(sizeof(chaos_gl_model_t));
    if (!m) { chaos_close(fd); return NULL; }

    m->vertex_count = hdr.vertex_count;
    m->normal_count = hdr.normal_count;
    m->uv_count     = hdr.uv_count;
    m->face_count   = hdr.face_count;

    /* Allocate geometry arrays */
    m->vertices = (struct cobj_vertex*)kmalloc(hdr.vertex_count * sizeof(struct cobj_vertex));
    m->normals  = hdr.normal_count ? (struct cobj_normal*)kmalloc(hdr.normal_count * sizeof(struct cobj_normal)) : NULL;
    m->uvs      = hdr.uv_count ? (struct cobj_uv*)kmalloc(hdr.uv_count * sizeof(struct cobj_uv)) : NULL;
    m->faces    = (struct cobj_face*)kmalloc(hdr.face_count * sizeof(struct cobj_face));

    if (!m->vertices || !m->faces) {
        serial_printf("[model] alloc failed\n");
        chaos_gl_model_free(m);
        chaos_close(fd);
        return NULL;
    }

    /* Read vertex data */
    chaos_seek(fd, (int64_t)hdr.vertex_offset, 0); /* CHAOS_SEEK_SET */
    if (read_full(fd, m->vertices, hdr.vertex_count * sizeof(struct cobj_vertex)) < 0) {
        serial_printf("[model] read vertices failed\n");
        chaos_gl_model_free(m);
        chaos_close(fd);
        return NULL;
    }

    /* Read normals */
    if (m->normals && hdr.normal_count > 0) {
        chaos_seek(fd, (int64_t)hdr.normal_offset, 0);
        if (read_full(fd, m->normals, hdr.normal_count * sizeof(struct cobj_normal)) < 0) {
            serial_printf("[model] read normals failed\n");
            chaos_gl_model_free(m);
            chaos_close(fd);
            return NULL;
        }
    }

    /* Read UVs */
    if (m->uvs && hdr.uv_count > 0) {
        chaos_seek(fd, (int64_t)hdr.uv_offset, 0);
        if (read_full(fd, m->uvs, hdr.uv_count * sizeof(struct cobj_uv)) < 0) {
            serial_printf("[model] read uvs failed\n");
            chaos_gl_model_free(m);
            chaos_close(fd);
            return NULL;
        }
    }

    /* Read faces */
    chaos_seek(fd, (int64_t)hdr.face_offset, 0);
    if (read_full(fd, m->faces, hdr.face_count * sizeof(struct cobj_face)) < 0) {
        serial_printf("[model] read faces failed\n");
        chaos_gl_model_free(m);
        chaos_close(fd);
        return NULL;
    }

    chaos_close(fd);
    return m;
}

void chaos_gl_model_free(chaos_gl_model_t* model) {
    if (!model) return;
    if (model->vertices) kfree(model->vertices);
    if (model->normals)  kfree(model->normals);
    if (model->uvs)      kfree(model->uvs);
    if (model->faces)    kfree(model->faces);
    kfree(model);
}

void chaos_gl_draw_model(chaos_gl_model_t* model) {
    if (!model) return;

    for (uint32_t i = 0; i < model->face_count; i++) {
        struct cobj_face* f = &model->faces[i];
        gl_vertex_in_t v[3];

        for (int j = 0; j < 3; j++) {
            struct cobj_vertex* vert = &model->vertices[f->v[j]];
            v[j].position = (vec3_t){ vert->x, vert->y, vert->z };

            if (model->normals && f->n[j] < model->normal_count) {
                struct cobj_normal* n = &model->normals[f->n[j]];
                v[j].normal = (vec3_t){ n->x, n->y, n->z };
            } else {
                v[j].normal = (vec3_t){ 0, 1, 0 };
            }

            if (model->uvs && f->t[j] < model->uv_count) {
                struct cobj_uv* t = &model->uvs[f->t[j]];
                v[j].uv = (vec2_t){ t->u, t->v };
            } else {
                v[j].uv = (vec2_t){ 0, 0 };
            }
        }

        chaos_gl_triangle(v[0], v[1], v[2]);
    }
}

/* ── Runtime model construction ──────────────────────── */

chaos_gl_model_t* chaos_gl_model_create(uint32_t vertex_count, uint32_t face_count) {
    if (vertex_count == 0 || face_count == 0 ||
        vertex_count > CHAOS_GL_MAX_MODEL_VERTS ||
        face_count > CHAOS_GL_MAX_MODEL_FACES)
        return NULL;

    chaos_gl_model_t* m = (chaos_gl_model_t*)kmalloc(sizeof(chaos_gl_model_t));
    if (!m) return NULL;

    m->vertex_count = vertex_count;
    m->normal_count = vertex_count;
    m->uv_count     = vertex_count;
    m->face_count   = face_count;

    m->vertices = (struct cobj_vertex*)kmalloc(vertex_count * sizeof(struct cobj_vertex));
    m->normals  = (struct cobj_normal*)kmalloc(vertex_count * sizeof(struct cobj_normal));
    m->uvs      = (struct cobj_uv*)kmalloc(vertex_count * sizeof(struct cobj_uv));
    m->faces    = (struct cobj_face*)kmalloc(face_count * sizeof(struct cobj_face));

    if (!m->vertices || !m->normals || !m->uvs || !m->faces) {
        chaos_gl_model_free(m);
        return NULL;
    }

    memset(m->vertices, 0, vertex_count * sizeof(struct cobj_vertex));
    memset(m->normals, 0, vertex_count * sizeof(struct cobj_normal));
    memset(m->uvs, 0, vertex_count * sizeof(struct cobj_uv));
    memset(m->faces, 0, face_count * sizeof(struct cobj_face));
    return m;
}

void chaos_gl_model_set_vertex(chaos_gl_model_t* m, uint32_t idx, float x, float y, float z) {
    if (!m || idx >= m->vertex_count) return;
    m->vertices[idx] = (struct cobj_vertex){ x, y, z };
}

void chaos_gl_model_set_normal(chaos_gl_model_t* m, uint32_t idx, float nx, float ny, float nz) {
    if (!m || idx >= m->normal_count) return;
    m->normals[idx] = (struct cobj_normal){ nx, ny, nz };
}

void chaos_gl_model_set_uv(chaos_gl_model_t* m, uint32_t idx, float u, float v) {
    if (!m || idx >= m->uv_count) return;
    m->uvs[idx] = (struct cobj_uv){ u, v };
}

void chaos_gl_model_set_face(chaos_gl_model_t* m, uint32_t idx, uint32_t v0, uint32_t v1, uint32_t v2) {
    if (!m || idx >= m->face_count) return;
    m->faces[idx].v[0] = v0; m->faces[idx].v[1] = v1; m->faces[idx].v[2] = v2;
    m->faces[idx].n[0] = v0; m->faces[idx].n[1] = v1; m->faces[idx].n[2] = v2;
    m->faces[idx].t[0] = v0; m->faces[idx].t[1] = v1; m->faces[idx].t[2] = v2;
}

void chaos_gl_draw_model_wire(chaos_gl_model_t* model, uint32_t color) {
    if (!model) return;
    chaos_gl_surface_t* s = chaos_gl_get_bound_surface();
    if (!s) return;

    mat4_t mvp = mat4_mul(s->projection, mat4_mul(s->view, s->model));

    for (uint32_t i = 0; i < model->face_count; i++) {
        struct cobj_face* f = &model->faces[i];
        int sx[3], sy[3];
        bool visible = true;

        for (int j = 0; j < 3; j++) {
            struct cobj_vertex* v = &model->vertices[f->v[j]];
            vec4_t clip = mat4_mul_vec4(mvp, (vec4_t){ v->x, v->y, v->z, 1.0f });
            if (clip.w <= 0.001f) { visible = false; break; }
            float ndcx = clip.x / clip.w;
            float ndcy = clip.y / clip.w;
            sx[j] = (int)((ndcx + 1.0f) * s->width * 0.5f);
            sy[j] = (int)((1.0f - ndcy) * s->height * 0.5f);
        }

        if (!visible) continue;

        chaos_gl_line(sx[0], sy[0], sx[1], sy[1], color);
        chaos_gl_line(sx[1], sy[1], sx[2], sy[2], color);
        chaos_gl_line(sx[2], sy[2], sx[0], sy[0], color);
    }
}
