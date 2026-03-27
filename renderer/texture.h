#pragma once
#include "surface.h"

#define CHAOS_GL_MAX_TEXTURES   64
#define CHAOS_GL_MAX_TEX_SIZE   1024
#define RAW_TEX_MAGIC           0x52415754  /* 'RAWT' */

struct raw_tex_header {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
} __attribute__((packed));

typedef struct {
    uint32_t* data;
    int       width;
    int       height;
    int       pitch;
    bool      in_use;
    bool      has_alpha;
    uint32_t  phys_addr;
    uint32_t  pages;
} chaos_gl_texture_t;

int  chaos_gl_texture_load(const char* path);
int  chaos_gl_texture_load_from_memory(const uint8_t* data, uint32_t len);
void chaos_gl_texture_free(int handle);
const chaos_gl_texture_t* chaos_gl_texture_get(int handle);
void chaos_gl_texture_get_size(int handle, int* w, int* h);
void chaos_gl_texture_init(void);
bool chaos_gl_texture_has_alpha(int handle);

/* Nearest-neighbour sampling with UV wrap */
static inline uint32_t chaos_gl_tex_sample(const chaos_gl_texture_t* tex, float u, float v) {
    u -= (int)u; if (u < 0.0f) u += 1.0f;
    v -= (int)v; if (v < 0.0f) v += 1.0f;
    int x = (int)(u * (tex->width  - 1) + 0.5f);
    int y = (int)(v * (tex->height - 1) + 0.5f);
    if (x >= tex->width)  x = tex->width  - 1;
    if (y >= tex->height) y = tex->height - 1;
    return tex->data[y * tex->pitch + x];
}
