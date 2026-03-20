# ChaosGL — Graphics Subsystem Specification
## AIOS v2 Universal Graphics Pipeline

---

## Founding Premise

ChaosGL is the AIOS graphics subsystem. It is not a game engine bolted onto an OS that already has a graphics layer. It IS the graphics layer. Everything that appears on screen — the desktop, window chrome, text, icons, HUD overlays, 3D viewports, the Doom clone, a spinning model in a file browser, a terminal, a settings panel — goes through ChaosGL. There is no other path to the framebuffer.

This is how modern OS graphics stacks work. Metal, Wayland compositors, DirectComposition — one pipeline, one back buffer, one present call per frame. The distinction between "2D" and "3D" is not an architectural boundary. It is a rendering mode. A window border and a textured polygon are both primitives in the same frame, drawn into the same back buffer, presented in one blit.

**ChaosGL is:**
- The display server
- The compositor
- The 2D drawing API (desktop, windows, text, icons)
- The 3D rendering pipeline (games, 3D widgets, model viewers)
- The window system's rendering backend
- The only entity that ever writes to VRAM

**The VBE framebuffer driver (Phase 3)** is demoted to a hardware abstraction layer. Its job is: initialise VBE, provide VRAM address and dimensions via `fb_get_info()`. It has no drawing primitives. ChaosGL is the drawing API.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    AIOS Applications                     │
│   Desktop  │  Shell  │  File Browser  │  Doom  │  Lua   │
└─────────────────────────┬───────────────────────────────┘
                          │ ChaosGL API
┌─────────────────────────▼───────────────────────────────┐
│                       ChaosGL                            │
│                                                          │
│  ┌─────────────────┐      ┌──────────────────────────┐  │
│  │   2D Subsystem  │      │     3D Pipeline          │  │
│  │                 │      │                          │  │
│  │  rect / circle  │      │  vertex shader           │  │
│  │  text / blit    │      │  clip / cull             │  │
│  │  clip regions   │      │  rasterize               │  │
│  │  bitmap font    │      │  z-buffer                │  │
│  │  rounded rects  │      │  fragment shader         │  │
│  └────────┬────────┘      └────────────┬─────────────┘  │
│           │                            │                 │
│           └──────────┬─────────────────┘                 │
│                      ▼                                   │
│              Back Buffer (RAM)                           │
│              Z-Buffer (RAM)                              │
│                      │                                   │
│              end_frame(): rep movsd → VRAM               │
└─────────────────────────────────────────────────────────┘
                        │
┌─────────────────────────────────────────────────────────┐
│           VBE Framebuffer Driver (HAL only)              │
│           fb_get_info() — VRAM addr, dims, pitch         │
└─────────────────────────────────────────────────────────┘
                        │
                   VBE Linear Framebuffer (VRAM)
```

---

## Frame Model

Every frame follows the same structure regardless of what is being rendered:

```
chaos_gl_begin_frame(clear_color)
    Clear back buffer to clear_color (BGRX)
    Clear z-buffer to CHAOS_GL_ZDEPTH_MAX

    ... application draws 3D scene (if any) ...
    ... application draws 2D GUI on top ...

chaos_gl_end_frame()
    rep movsd: back buffer → VRAM (one copy, no intermediate)
```

Draw order is compositing. 3D renders first, then 2D draws on top. There is no separate composite pass — order of draw calls is order of composition. This is intentional and simple.

**Frame ownership:** between `begin_frame()` and `end_frame()`, the back buffer belongs to the calling task. On a multitasking OS, the GUI task calls begin/end_frame at the top of its render loop. Other tasks submit draw commands (future: command queue). For AIOS v2, the GUI task owns the frame loop directly.

**Frame pacing:** the scheduler ticks at 250Hz. The target render rate is 60fps — one frame every ~16.6ms, or roughly every 4 scheduler ticks. The GUI task does NOT call begin/end_frame on every scheduler tick. It uses `task_sleep(16)` (16ms) at the end of each frame to yield the CPU and pace itself to ~60fps. The actual rate will be ~62.5fps (250/4) due to tick granularity — this is acceptable. If a frame takes longer than 16ms, the next sleep is skipped and the renderer runs as fast as it can.

**Target rate:** 60 fps at 1024×768. Frame budget ~16.6ms. The 2D desktop easily fits this budget. The 3D pipeline is the variable — see Stats for measurement.

---

## Math Library (renderer/math.c / math.h)

No dependencies on any other AIOS subsystem. Pure math.

### Types

```c
typedef struct { float x, y; }       vec2_t;
typedef struct { float x, y, z; }    vec3_t;
typedef struct { float x, y, z, w; } vec4_t;
typedef struct { int   x, y; }       vec2i_t;
typedef struct { int   x, y, w, h; } rect_t;   /* integer rect for 2D */
```

### Matrix

```c
/* Column-major 4×4 matrix — m[col][row].
 * m[0][0..3] = column 0, m[1][0..3] = column 1, etc.
 * Translation is in m[3][0], m[3][1], m[3][2].
 * First index is COLUMN, second is ROW — do not confuse with C row-major intuition. */
typedef struct { float m[4][4]; } mat4_t;
```

### Operations

```c
/* vec2 */
vec2_t  vec2_add(vec2_t a, vec2_t b);
vec2_t  vec2_sub(vec2_t a, vec2_t b);
vec2_t  vec2_scale(vec2_t v, float s);

/* vec3 */
vec3_t  vec3_add(vec3_t a, vec3_t b);
vec3_t  vec3_sub(vec3_t a, vec3_t b);
vec3_t  vec3_scale(vec3_t v, float s);
float   vec3_dot(vec3_t a, vec3_t b);
vec3_t  vec3_cross(vec3_t a, vec3_t b);
float   vec3_len(vec3_t v);
vec3_t  vec3_normalize(vec3_t v);
vec3_t  vec3_lerp(vec3_t a, vec3_t b, float t);

/* vec4 */
vec4_t  vec4_from_vec3(vec3_t v, float w);
vec3_t  vec4_to_vec3(vec4_t v);       /* divides xyz by w */

/* mat4 */
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
/* fovy_rad: vertical FOV in radians. aspect: width/height. */
mat4_t  mat4_perspective(float fovy_rad, float aspect, float z_near, float z_far);

/* rect helpers */
bool    rect_contains(rect_t r, int x, int y);
rect_t  rect_intersect(rect_t a, rect_t b);
bool    rect_is_empty(rect_t r);
```

**Float throughout.** SSE2 is enabled for renderer code (`RENDERER_CFLAGS`). The compiler uses XMM registers for float math — faster than x87 for this workload. Fixed-point was correct for the v1 BSP renderer. For a full perspective pipeline and 2D compositing, float + SSE2 is cleaner and fast enough.

---

## Back Buffer and Z-Buffer

### Back Buffer

PMM-allocated BGRX pixel array, same dimensions as the VBE framebuffer. All rendering — 2D and 3D — targets this buffer. Never written directly by anything other than ChaosGL.

Allocated from PMM during `chaos_gl_init()`. Marked `PAGE_RESERVED` via `heap_mark_reserved()`. Identity-mapped in VMM with PRESENT | WRITABLE (cached RAM — this is a normal memory buffer, not MMIO).

**Pixel format: BGRX 32bpp.** Blue in the low byte, matching VBE hardware layout. No conversion needed on blit. Alpha byte (X) is unused and ignored.

```c
/* Colour construction helpers */
#define CHAOS_GL_RGB(r, g, b)  ((uint32_t)((b) | ((g) << 8) | ((r) << 16)))
#define CHAOS_GL_BGRX(b,g,r)   ((uint32_t)((b) | ((g) << 8) | ((r) << 16)))
```

### Z-Buffer

PMM-allocated uint16_t array, same dimensions as framebuffer. Used by the 3D pipeline only — 2D draws bypass the z-buffer entirely.

**16-bit fixed-point depth.** Maps post-perspective-divide NDC z [-1, 1] to [0, 65535]. 0 = nearest, 65535 = farthest. Cleared to 0xFFFF at `begin_frame()`.

Why 16-bit: 1024×768 z-buffer = 1.5MB vs 3MB for float. Sufficient precision for Doom-scale scenes. SSE2 can compare 8 × uint16_t per instruction.

```c
#define CHAOS_GL_ZDEPTH_MAX  0xFFFF
#define CHAOS_GL_ZDEPTH_MIN  0x0000

static inline uint16_t ndc_to_zdepth(float ndc_z) {
    if (ndc_z < -1.0f) ndc_z = -1.0f;
    if (ndc_z >  1.0f) ndc_z =  1.0f;
    return (uint16_t)((ndc_z * 0.5f + 0.5f) * 65535.0f);
}
```

---

## 2D Subsystem

### Clip Stack

The 2D subsystem maintains a clip stack. All 2D draw calls are silently clipped to the current clip rectangle. This is how window content stays inside window borders — the window manager pushes a clip rect before drawing the window's contents, pops it after.

```c
#define CHAOS_GL_CLIP_STACK_DEPTH  16

/* Push a new clip rect (intersected with current top — clips never expand) */
void chaos_gl_push_clip(rect_t r);

/* Pop the top clip rect */
void chaos_gl_pop_clip(void);

/* Clear clip stack — full framebuffer is drawable */
void chaos_gl_reset_clip(void);
```

**Clip intersection contract:** pushing a child clip never expands beyond the parent. If a child rect extends outside the parent, it is clamped. This prevents child windows from drawing outside their parent.

### Primitives

All 2D draws write directly to the back buffer. No z-test, no z-write. Drawn in call order — later calls overdraw earlier calls.

```c
/* Filled rectangle */
void chaos_gl_rect(int x, int y, int w, int h, uint32_t color);

/* Rectangle outline */
void chaos_gl_rect_outline(int x, int y, int w, int h,
                            uint32_t color, int thickness);

/* Rounded rectangle (filled) */
void chaos_gl_rect_rounded(int x, int y, int w, int h,
                            int radius, uint32_t color);

/* Rounded rectangle outline */
void chaos_gl_rect_rounded_outline(int x, int y, int w, int h,
                                    int radius, uint32_t color, int thickness);

/* Circle (filled) */
void chaos_gl_circle(int cx, int cy, int radius, uint32_t color);

/* Circle outline */
void chaos_gl_circle_outline(int cx, int cy, int radius,
                              uint32_t color, int thickness);

/* Line (Bresenham) */
void chaos_gl_line(int x0, int y0, int x1, int y1, uint32_t color);

/* Single pixel */
void chaos_gl_pixel(int x, int y, uint32_t color);

/* Alpha blend a pixel (src over dst, 8-bit alpha in high byte of color)
 * Used for anti-aliased text and smooth UI elements. */
void chaos_gl_pixel_blend(int x, int y, uint32_t color_with_alpha);
```

### Image Blit

```c
/* Blit a BGRX pixel array to the back buffer at (x, y).
 * src_pitch: row stride of source in pixels (may differ from w for sub-image blits).
 * Clips to current clip rect.
 * Used for: icons, image thumbnails, sprites, UI artwork. */
void chaos_gl_blit(int x, int y, int w, int h,
                   const uint32_t* src, int src_pitch);

/* Blit with colour key transparency — pixels matching key_color are not written */
void chaos_gl_blit_keyed(int x, int y, int w, int h,
                          const uint32_t* src, int src_pitch,
                          uint32_t key_color);

/* Blit with per-pixel alpha.
 * Source pixels are BGRA — alpha in the high byte (bits [24..31]).
 * The back buffer remains BGRX — alpha is consumed during the blit, not stored.
 * Performs src-over compositing per pixel. Slower than plain blit — use for
 * icons with transparency, UI overlays with smooth edges. */
void chaos_gl_blit_alpha(int x, int y, int w, int h,
                          const uint32_t* src, int src_pitch);
```

### Text

Uses Claude Mono — the custom 8×16 bitmap font. Each glyph is 8 pixels wide, 16 pixels tall. Full printable ASCII.

```c
/* Draw a single character. Returns x advance (always 8 for Claude Mono). */
int  chaos_gl_char(int x, int y, char c, uint32_t fg, uint32_t bg);

/* Draw a null-terminated string. Returns width in pixels drawn.
 * bg = 0 means transparent background (only fg pixels written). */
int  chaos_gl_text(int x, int y, const char* str, uint32_t fg, uint32_t bg);

/* Draw string with word-wrap within max_w pixels.
 * Returns total height in pixels used. */
int  chaos_gl_text_wrapped(int x, int y, int max_w,
                            const char* str, uint32_t fg, uint32_t bg);

/* Measure string width in pixels without drawing. */
int  chaos_gl_text_width(const char* str);

/* Measure wrapped text height without drawing. */
int  chaos_gl_text_height_wrapped(int max_w, const char* str);
```

**Transparent background:** when `bg == 0` (not black — 0 is the sentinel), only foreground pixels are written. Background pixels are skipped. This allows text to be drawn over any surface. If you actually want black background, use `CHAOS_GL_RGB(0,0,1)` and it's close enough, or we add an explicit transparency flag in a future revision.

**Note:** bg transparency via sentinel 0 is a known wart. A cleaner solution is a flags parameter or a separate `chaos_gl_text_transparent()` call. Deferred to v2.1 — for now, 0 means transparent.

---

## 3D Pipeline

### Overview

```
Per-triangle call: chaos_gl_triangle(v0, v1, v2)
    │
    ├─► Vertex shader (C fn ptr) × 3
    │       Inputs:  model-space position, normal, UV + uniforms
    │       Outputs: clip-space position, view-space normal, UV, intensity
    │       Vertex shader owns the ModelView × Projection multiply
    │
    ├─► Backface cull — signed area in clip space, skip if back-facing
    │
    ├─► Near-plane clip — Sutherland-Hodgman, 0–2 output triangles
    │
    ├─► Perspective divide — clip.xyz / clip.w → NDC [-1,1]³
    │
    ├─► Viewport transform
    │       screen_x = (ndc_x + 1.0) * viewport_w / 2
    │       screen_y = (1.0 - ndc_y) * viewport_h / 2   (Y-flip: NDC up = screen down)
    │
    └─► Rasterize (barycentric coords, perspective-correct interpolation)
            Per pixel:
                Fragment shader → gl_frag_out_t (color + discard flag)
                Z-test (16-bit)
                Write to back buffer (if not discarded and z passes)
```

### Shader Interface

```c
/* Vertex shader input — one vertex in model space */
typedef struct {
    vec3_t position;
    vec3_t normal;
    vec2_t uv;
} gl_vertex_in_t;

/* Vertex shader output — one vertex in clip space.
 *
 * Field usage by shader type:
 *   flat:      intensity = constant, normal unused in fragment stage
 *   gouraud:   intensity = dot(normal, light) per vertex, interpolated
 *   diffuse:   normal passed through (view-space), intensity unused
 *   normalmap: normal = tangent-space basis, intensity unused
 *
 * All fields are always interpolated regardless of shader — unused fields
 * are just interpolated and ignored. No cost. */
typedef struct {
    vec4_t clip_pos;   /* clip-space position — before perspective divide */
    vec3_t normal;     /* view-space normal */
    vec2_t uv;
    float  intensity;  /* pre-computed lighting (flat / Gouraud) */
} gl_vertex_out_t;

/* Fragment shader input — interpolated values for current pixel */
typedef struct {
    vec2_t uv;         /* perspective-correct interpolated UV */
    vec3_t normal;     /* perspective-correct interpolated normal */
    float  intensity;  /* perspective-correct interpolated intensity */
    int    x, y;       /* screen coordinates */
} gl_fragment_in_t;

/* Fragment shader output */
typedef struct {
    uint32_t color;    /* BGRX pixel */
    bool     discard;  /* true = skip pixel, no z-write */
} gl_frag_out_t;

/* Convenience macros */
#define GL_COLOR(bgrx)  ((gl_frag_out_t){ .color = (bgrx), .discard = false })
#define GL_DISCARD      ((gl_frag_out_t){ .color = 0,      .discard = true  })

/* Shader function pointer types */
typedef gl_vertex_out_t (*gl_vert_fn)(gl_vertex_in_t in, void* uniforms);
typedef gl_frag_out_t   (*gl_frag_fn)(gl_fragment_in_t in, void* uniforms);
```

**Z-test ordering: late-z (after fragment shader).** This correctly supports `GL_DISCARD` — a discarded fragment never writes z. The performance cost (running fragment shader on occluded pixels) is acceptable for our scene complexity. Early-z prepass is a future optimisation.

### Near-Plane Clipping

```c
/* Clip one triangle against the near plane.
 * out_verts: output array, room for 4 vertices (max polygon after one-plane clip).
 * Returns 0 (fully behind), 1, or 2 output triangles.
 *   result == 1: use out_verts[0..2]
 *   result == 2: use out_verts[0..2] AND out_verts[0,2,3]
 * Vertex attributes (normal, uv, intensity) are linearly interpolated at clip point. */
int chaos_gl_clip_near(gl_vertex_out_t in[3],
                       gl_vertex_out_t out_verts[4],
                       float z_near);
```

### Built-in Shaders

```c
/* Flat colour — uniform: { uint32_t color } */
extern gl_vert_fn shader_flat_vert;
extern gl_frag_fn shader_flat_frag;

/* Diffuse texture + directional light — uniform: { chaos_gl_texture_t* tex,
 *   vec3_t light_dir, float ambient } */
extern gl_vert_fn shader_diffuse_vert;
extern gl_frag_fn shader_diffuse_frag;

/* Gouraud — per-vertex lighting, interpolated — uniform: { vec3_t light_dir,
 *   float ambient, uint32_t color } */
extern gl_vert_fn shader_gouraud_vert;
extern gl_frag_fn shader_gouraud_frag;

/* Tangent-space normal map — uniform: { chaos_gl_texture_t* tex,
 *   chaos_gl_texture_t* normalmap, vec3_t light_dir, float ambient } */
extern gl_vert_fn shader_normalmap_vert;
extern gl_frag_fn shader_normalmap_frag;
```

### Shader Registry

```c
#define CHAOS_GL_MAX_SHADERS  32  /* 4 built-in + 28 user */

/* Register a named shader pair. Name must be a string literal or permanently
 * allocated — registry stores pointer, does not copy.
 * Returns 0 on success, -1 if full or name already registered. */
int chaos_gl_shader_register(const char* name, gl_vert_fn vert, gl_frag_fn frag);

/* Set active shader by name. Returns 0 on success, -1 if not found. */
int chaos_gl_shader_set_by_name(const char* name, void* uniforms);

/* Set active shader directly by function pointers. */
void chaos_gl_shader_set(gl_vert_fn vert, gl_frag_fn frag, void* uniforms);
```

---

## Textures

```c
#define CHAOS_GL_MAX_TEXTURES   64
#define CHAOS_GL_MAX_TEX_SIZE   1024   /* max width or height */

typedef struct {
    uint32_t* data;    /* BGRX pixels, row-major */
    int       width;
    int       height;
    int       pitch;   /* row stride in pixels — equals width for .raw files */
    bool      in_use;
} chaos_gl_texture_t;
```

**Texture memory:** pixel data allocated from PMM (`pmm_alloc_pages()`), not the heap. Textures are large, page-aligned, long-lived — they don't belong in the slab/buddy allocator. Pages marked `PAGE_RESERVED`. Identity-mapped in VMM with PRESENT | WRITABLE (cached RAM).

**Texture pitch:** for `.raw` format, pitch == width always. The field exists for forward compatibility. All sampling code must use pitch for row stride, never width.

### Texture Sampling

```c
/* Nearest-neighbour sampling with UV wrap.
 * v2 only. Bilinear filtering is a future extension. */
static inline uint32_t chaos_gl_tex_sample(const chaos_gl_texture_t* tex,
                                            float u, float v) {
    /* Wrap to [0, 1) */
    u -= (int)u; if (u < 0.0f) u += 1.0f;
    v -= (int)v; if (v < 0.0f) v += 1.0f;
    int x = (int)(u * (tex->width  - 1) + 0.5f);
    int y = (int)(v * (tex->height - 1) + 0.5f);
    return tex->data[y * tex->pitch + x];
}
```

### Texture File Format (.raw)

```c
#define RAW_TEX_MAGIC  0x52415754  /* 'RAWT' */

struct raw_tex_header {
    uint32_t magic;     /* must be RAW_TEX_MAGIC */
    uint32_t width;
    uint32_t height;
    uint32_t reserved;  /* zero — reserved for flags (mipmap count, format, etc.) */
} __attribute__((packed));
/* Followed by width × height × 4 bytes of BGRX pixel data, row-major, no padding */
```

Reject if: magic mismatch, width or height == 0, width or height > CHAOS_GL_MAX_TEX_SIZE, file truncated.

### Texture API

```c
/* Load from ChaosFS. Returns handle (>= 0) or -1. */
int chaos_gl_texture_load(const char* path);

/* Free texture — releases PMM pages. Handle is invalid after this call.
 * Any pointers from texture_get() for this handle are dangling after free. */
void chaos_gl_texture_free(int handle);

/* Get read-only pointer. SAFETY: do not store past current frame.
 * Texture could be freed between frames. Returns NULL if handle invalid. */
const chaos_gl_texture_t* chaos_gl_texture_get(int handle);
```

---

## Models

### .cobj Binary Format

Text .obj files are fine as source assets but slow to parse at runtime. The build system converts them to .cobj binary format. AIOS never parses text .obj files.

```c
#define COBJ_MAGIC    0x434F424A  /* 'COBJ' */
#define COBJ_VERSION  1

struct cobj_header {
    uint32_t magic;
    uint32_t version;
    uint32_t vertex_count;
    uint32_t normal_count;
    uint32_t uv_count;
    uint32_t face_count;      /* triangles only — quads pre-triangulated at export */
    uint32_t vertex_offset;   /* byte offset from file start to vertex array */
    uint32_t normal_offset;
    uint32_t uv_offset;
    uint32_t face_offset;
} __attribute__((packed));

struct cobj_vertex { float x, y, z; } __attribute__((packed));
struct cobj_normal { float x, y, z; } __attribute__((packed));
struct cobj_uv     { float u, v;    } __attribute__((packed));
struct cobj_face   {
    uint32_t v[3];   /* indices into vertex array */
    uint32_t n[3];   /* indices into normal array */
    uint32_t t[3];   /* indices into uv array */
} __attribute__((packed));
```

### Model API

```c
#define CHAOS_GL_MAX_MODEL_VERTS  65536
#define CHAOS_GL_MAX_MODEL_FACES  131072

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

/* Load .cobj from ChaosFS. Returns model or NULL if not found, corrupt,
 * or exceeds sanity limits. Geometry allocated from heap (kmalloc). */
chaos_gl_model_t* chaos_gl_model_load(const char* path);
void              chaos_gl_model_free(chaos_gl_model_t* model);
```

---

## Public API

### Lifecycle

```c
/* Initialise ChaosGL. Must be called after fb_get_info() returns valid data.
 * Allocates back buffer and z-buffer from PMM, maps them in VMM,
 * registers built-in shaders, initialises texture table.
 * Returns 0 on success, -1 on failure (OOM). */
int  chaos_gl_init(void);

/* Shutdown — free back buffer, z-buffer, all loaded textures.
 * PMM stats should return to pre-init levels after this call. */
void chaos_gl_shutdown(void);
```

### Frame

```c
/* Begin frame: clear back buffer to clear_color_bgrx, clear z-buffer to 0xFFFF.
 * Example colours (BGRX — blue in low byte):
 *   Black:  0x00000000
 *   White:  0x00FFFFFF
 *   Red:    0x000000FF
 *   Green:  0x0000FF00
 *   Blue:   0x00FF0000 */
void chaos_gl_begin_frame(uint32_t clear_color_bgrx);

/* End frame: blit back buffer → VRAM via rep movsd. One copy. */
void chaos_gl_end_frame(void);
```

### Camera and Projection

```c
/* Set view matrix from camera parameters. Stored internally.
 * Combined with model matrix on each 3D draw call: MVP = proj × view × model. */
void chaos_gl_set_camera(vec3_t eye, vec3_t center, vec3_t up);

/* Set view matrix directly (bypasses lookat). */
void chaos_gl_set_view(mat4_t view);

/* Set perspective projection.
 * fovy_deg: vertical FOV in degrees. aspect: width/height (0 = auto from fb dims).
 * z_near, z_far: positive clip distances (e.g. 0.1, 1000.0). */
void chaos_gl_set_perspective(float fovy_deg, float aspect,
                               float z_near, float z_far);

/* Set projection matrix directly. */
void chaos_gl_set_projection(mat4_t proj);
```

### Model Transform

```c
/* Set model transform from TRS components.
 * Builds: model = T(tx,ty,tz) × Ry(ry) × Rx(rx) × Rz(rz) × S(sx,sy,sz)
 * Rotation order Y→X→Z (yaw→pitch→roll, matching FPS camera convention).
 * Combined with current view on each 3D draw call. */
void chaos_gl_set_transform(float tx, float ty, float tz,
                             float rx, float ry, float rz,
                             float sx, float sy, float sz);

/* Set model matrix directly. */
void chaos_gl_set_model(mat4_t model);
```

### 3D Draw Calls

```c
/* Set active shader. All subsequent 3D draw calls use this shader. */
void chaos_gl_shader_set(gl_vert_fn vert, gl_frag_fn frag, void* uniforms);
int  chaos_gl_shader_set_by_name(const char* name, void* uniforms);

/* Draw one triangle. */
void chaos_gl_triangle(gl_vertex_in_t v0, gl_vertex_in_t v1, gl_vertex_in_t v2);

/* Draw all faces of a model with current shader and transform. */
void chaos_gl_draw_model(chaos_gl_model_t* model);

/* Draw model as wireframe (no shader, no z-write, debug only). */
void chaos_gl_draw_model_wire(chaos_gl_model_t* model, uint32_t color);
```

### 2D Draw Calls

```c
/* Clip stack */
void chaos_gl_push_clip(rect_t r);
void chaos_gl_pop_clip(void);
void chaos_gl_reset_clip(void);

/* Primitives */
void chaos_gl_rect(int x, int y, int w, int h, uint32_t color);
void chaos_gl_rect_outline(int x, int y, int w, int h,
                            uint32_t color, int thickness);
void chaos_gl_rect_rounded(int x, int y, int w, int h,
                            int radius, uint32_t color);
void chaos_gl_rect_rounded_outline(int x, int y, int w, int h,
                                    int radius, uint32_t color, int thickness);
void chaos_gl_circle(int cx, int cy, int radius, uint32_t color);
void chaos_gl_circle_outline(int cx, int cy, int radius,
                              uint32_t color, int thickness);
void chaos_gl_line(int x0, int y0, int x1, int y1, uint32_t color);
void chaos_gl_pixel(int x, int y, uint32_t color);

/* Alpha blend. Source alpha in bits [24..31]. src-over composite.
 * Used for anti-aliased text edges and smooth UI transitions. */
void chaos_gl_pixel_blend(int x, int y, uint32_t color_bgra);

/* Image blit */
void chaos_gl_blit(int x, int y, int w, int h,
                   const uint32_t* src, int src_pitch);
void chaos_gl_blit_keyed(int x, int y, int w, int h,
                          const uint32_t* src, int src_pitch,
                          uint32_t key_color);
void chaos_gl_blit_alpha(int x, int y, int w, int h,
                          const uint32_t* src, int src_pitch);

/* Text (Claude Mono 8×16) */
int  chaos_gl_char(int x, int y, char c, uint32_t fg, uint32_t bg);
int  chaos_gl_text(int x, int y, const char* str, uint32_t fg, uint32_t bg);
int  chaos_gl_text_wrapped(int x, int y, int max_w,
                            const char* str, uint32_t fg, uint32_t bg);
int  chaos_gl_text_width(const char* str);
int  chaos_gl_text_height_wrapped(int max_w, const char* str);
```

**bg == 0 in text calls means transparent background** — only foreground pixels written. This is a known wart (can't render true-black background). Addressed in v2.1 with an explicit flags parameter.

### Stats

```c
typedef struct {
    /* 3D pipeline counters — reset each begin_frame() */
    uint32_t triangles_submitted;
    uint32_t triangles_culled;     /* backface culled */
    uint32_t triangles_clipped;    /* near-plane clip removed or reduced */
    uint32_t triangles_drawn;      /* reached rasterizer (may exceed submitted
                                    * minus culled/clipped due to clip splits) */
    uint32_t pixels_written;       /* passed z-test, written to back buffer */
    uint32_t pixels_zfailed;       /* failed z-test */
    uint32_t pixels_discarded;     /* discarded by fragment shader */

    /* 2D counters — reset each begin_frame() */
    uint32_t draw_calls_2d;        /* total 2D primitive calls this frame */

    /* Timing */
    uint32_t frame_time_us;        /* last frame time in microseconds (RDTSC) */
    uint32_t frame_3d_us;          /* time spent in 3D pipeline */
    uint32_t frame_2d_us;          /* time spent in 2D draws */
    uint32_t frame_blit_us;        /* time spent in end_frame() blit */
} chaos_gl_stats_t;

chaos_gl_stats_t chaos_gl_get_stats(void);
```

---

## Lua API

All functions registered via `lua_register()` during `chaos_gl_init()`.

```lua
-- Frame
chaos_gl.begin_frame(clear_color_bgrx)
chaos_gl.end_frame()

-- Camera / projection
chaos_gl.set_camera(ex,ey,ez, cx,cy,cz, ux,uy,uz)
chaos_gl.set_perspective(fovy_deg, aspect, near, far)  -- aspect=0 for auto

-- 3D transform
chaos_gl.set_transform(tx,ty,tz, rx,ry,rz, sx,sy,sz)

-- 3D draw
local model = chaos_gl.load_model("/models/barrel.cobj")
chaos_gl.draw_model(model, shader_name, uniforms_table)
chaos_gl.draw_wireframe(model, color_bgrx)
chaos_gl.free_model(model)

-- Textures
local tex = chaos_gl.load_texture("/textures/wall.raw")
chaos_gl.free_texture(tex)
-- tex can be passed in uniforms: {texture = tex} or {texture = "/path/to/tex.raw"}

-- 2D clip
chaos_gl.push_clip(x, y, w, h)
chaos_gl.pop_clip()
chaos_gl.reset_clip()

-- 2D primitives
chaos_gl.rect(x, y, w, h, color)
chaos_gl.rect_outline(x, y, w, h, color, thickness)
chaos_gl.rect_rounded(x, y, w, h, radius, color)
chaos_gl.circle(x, y, radius, color)
chaos_gl.line(x0, y0, x1, y1, color)
chaos_gl.pixel(x, y, color)

-- 2D image (texture handle from load_texture — Lua cannot pass raw pixel pointers)
chaos_gl.blit(x, y, w, h, tex_handle)
chaos_gl.blit_keyed(x, y, w, h, tex_handle, key_color)

-- 2D text (Claude Mono 8×16, bg=0 means transparent)
chaos_gl.text(x, y, str, fg, bg)
chaos_gl.text_wrapped(x, y, max_w, str, fg, bg)
chaos_gl.text_width(str)                    -- returns pixel width
chaos_gl.text_height_wrapped(max_w, str)    -- returns pixel height without drawing
chaos_gl.char(x, y, c, fg, bg)

-- Stats
local s = chaos_gl.get_stats()
-- s.triangles_drawn, s.frame_time_us, s.frame_3d_us, etc.
```

### Lua Uniform Table — Built-in Shader Layouts

| Shader | Required | Optional | Defaults |
|--------|----------|----------|---------|
| `"flat"` | — | `color` (BGRX uint32) | color=0x00FFFFFF |
| `"diffuse"` | `texture` (path or handle) | `light_dir_x/y/z`, `ambient` | light=(0,-1,0), ambient=0.1 |
| `"gouraud"` | — | `light_dir_x/y/z`, `ambient`, `color` | light=(0,-1,0), ambient=0.1, color=0x00FFFFFF |
| `"normalmap"` | `texture`, `normalmap` | `light_dir_x/y/z`, `ambient` | light=(0,-1,0), ambient=0.1 |

**Texture in uniforms:** either a path string (`"/textures/wall.raw"`) or an integer handle from `load_texture()`. Path strings are resolved once per unique path per frame and cached. Missing path → 1×1 magenta fallback, logged as warning, draw call continues.

---

## Dependencies

### VBE Framebuffer Driver (Phase 3)

The framebuffer driver is now a HAL only. It provides one function that ChaosGL calls during init:

```c
typedef struct {
    uint32_t fb_addr;    /* physical address of VBE linear framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint8_t  bpp;        /* bits per pixel — must be 32 */
} fb_info_t;

/* Returns true if VBE is initialised and framebuffer is available.
 * Returns false if no VBE (system is in text mode — chaos_gl_init() will fail). */
bool fb_get_info(fb_info_t* out);
```

**The Phase 3 framebuffer driver spec is revised accordingly:** `fb_putpixel`, `fb_rect`, `fb_text`, `fb_char`, `fb_swap` and all other drawing functions are removed. Only `fb_init()` (called at boot), `fb_get_info()`, and internal VBE management remain.

### PMM

Back buffer, z-buffer, and texture pixel data allocated from PMM. All marked `PAGE_RESERVED`.

### VMM

All PMM-allocated ChaosGL buffers mapped in VMM with PRESENT | WRITABLE (cached RAM).

### Heap

Model geometry allocated from heap (kmalloc). Small, variable-size, relatively short-lived.

### ChaosFS

Texture and model loading reads from ChaosFS.

### RDTSC

Frame timing via `rdtsc()` / `rdtsc_to_us()` from the RDTSC subsystem.

---

## Project Structure

```
renderer/
├── math.c / math.h           # vec2/3/4, mat4, rect_t, all math
├── backbuffer.c / .h         # back buffer + z-buffer alloc, begin/end_frame, blit
├── 2d.c / 2d.h               # all 2D primitives, clip stack, text, blit
├── font.c / font.h           # Claude Mono 8×16 bitmap glyph data
├── pipeline.c / .h           # vertex transform, clip, perspective divide, viewport
├── rasterizer.c / .h         # triangle rasterization, z-test, barycentric interp
├── shaders.c / .h            # built-in shader implementations + registry
├── texture.c / .h            # texture table, load (.raw), sample
├── model.c / .h              # .cobj load/free
├── chaos_gl.c / chaos_gl.h   # public C API — single include for everything
└── lua_bindings.c            # Lua API registration and dispatch

tools/
├── obj2cobj.c                # host tool: .obj + .mtl → .cobj
└── gen_texture.c             # host tool: generate test .raw textures
```

`chaos_gl.h` is the single public header. Applications include only this. Internal modules include their own headers directly.

---

## Memory Budget (1024×768)

| Region | Size | Allocator |
|--------|------|-----------|
| Back buffer (BGRX) | 3.0 MB | PMM |
| Z-buffer (uint16_t) | 1.5 MB | PMM |
| Typical textures (8 × 256×256) | 2.0 MB | PMM |
| Model geometry (typical scene) | 1–4 MB | Heap |
| Clip stack + internal state | < 4 KB | Heap |
| **Total typical** | **~8 MB** | |

Fixed overhead: 4.5MB (back buffer + z-buffer). With 256MB RAM and kernel taking ~10MB, comfortable headroom.

Texture budget is the variable. 64 × 1024×1024 = 256MB theoretical maximum — never approach this in practice. Doom-style 256×256 textures at 16 active = 4MB. PMM enforces the limit at runtime.

---

## Test Assets

Generated by the build system. Required on disk before Phase 9 tests run.

| Path | Contents | Used in tests |
|------|----------|---------------|
| `/test/cube.cobj` | Unit cube, 12 triangles | 3D pipeline tests |
| `/test/quad.cobj` | Flat quad, 2 triangles, UV 0..1 | Texture, UV, normal map tests |
| `/test/sphere.cobj` | UV sphere, ~320 triangles | Gouraud shading test |
| `/test/white.raw` | 64×64 solid white | Flat shading, z-buffer tests |
| `/test/grid.raw` | 64×64 UV grid | Texture mapping, perspective-correct UV |
| `/test/flat_normal.raw` | 64×64 flat normal map (0.5, 0.5, 1.0) | Normal map baseline |
| `/test/bump_normal.raw` | 64×64 bumped normal map | Normal map effect test |

---

## Phase 9 Acceptance Tests

### 2D Subsystem

1. **Clear:** `begin_frame(0x000000FF)` fills screen red (BGRX). `end_frame()` blits. Visual verify.
2. **Rect:** draw a 200×100 white rect at (50,50). Correct position, correct fill, no bleed.
3. **Rect outline:** draw a 3px red outline rect. Correct thickness, corners correct.
4. **Rounded rect:** draw a rounded rect with radius 10. Corners are visibly rounded.
5. **Circle:** draw a filled circle and an outline circle. Both correct shape.
6. **Line:** draw from (0,0) to (1023,767). Both endpoints hit exactly.
7. **Text:** render "Hello AIOS" with Claude Mono. Characters correct shape, spacing correct. Transparent background — underlying colour shows through where bg==0.
8. **Text wrap:** render a long string with max_w=200. Wraps at word boundary. `text_height_wrapped()` matches actual rendered height.
9. **Clip rect:** push clip (100,100,200,200). Draw a large rect that extends beyond it. Only the intersection is visible. Pop clip — full rect draws.
10. **Clip stack:** push two nested clips. Inner clip is correctly intersected with outer. Pop both — full region drawable again.
11. **Blit:** blit `/test/grid.raw` to screen. UV grid appears correct orientation, no distortion.
12. **Blit keyed:** blit with key colour = black. Black pixels transparent, others visible.

### 3D Pipeline

13. **Flat triangle:** draw one triangle, flat white shader. Correct shape, no gaps, no bleed.
14. **Z-buffer:** two overlapping triangles at different depths. Draw far one first, then near — near occludes. Swap draw order — same result. Proves z-buffer, not painter's algorithm.
15. **Backface cull:** draw `/test/cube.cobj`. ~6 of 12 triangles culled (back faces). Stats confirm. No visual artifact.
16. **Perspective:** draw `/test/cube.cobj`. Parallel edges converge. Looks 3D.
17. **Texture:** draw `/test/quad.cobj` with `/test/grid.raw`. UV grid correct orientation (U left→right, V top→bottom).
18. **Perspective-correct UV:** draw `/test/quad.cobj` at steep angle. Grid does not swim across the diagonal.
19. **Camera:** move camera via `set_camera()`. Scene changes correctly each frame.
20. **Normal lighting:** diffuse shader on cube. Lit face bright, opposite face at ambient level. Terminator on correct edge.
21. **Gouraud:** `"gouraud"` shader on `/test/sphere.cobj`. Smooth gradient. No per-triangle breaks.
22. **Normal map:** `"normalmap"` on `/test/quad.cobj`. `/test/flat_normal.raw` → no bump. `/test/bump_normal.raw` → bumpy.
23. **Near clip:** camera near plane intersects triangle. `triangles_clipped > 0` in stats. No crash, no divide-by-zero, no garbage.
24. **Discard:** custom shader discards pixels where UV.u < 0.5. Left half of quad transparent, right half renders. Z not written for discarded pixels (verify by drawing something behind — it shows through the left half).
25. **Stats:** no-clip scene: `triangles_culled + triangles_drawn == triangles_submitted`. Pixel counters sum to total rasterised fragments. Frame timers > 0 and plausible.

### Integration

26. **2D over 3D:** render a 3D cube, then draw 2D text overlay on top. Text appears over the 3D scene, not behind it. Z-buffer not involved in 2D draw.
27. **Clip over 3D:** 3D scene renders, then 2D clip rect restricts a GUI panel drawn on top. 3D scene visible outside the panel, panel content clipped inside.
28. **Lua scene:** Lua script: load `/test/cube.cobj`, set perspective, spin cube for 60 frames, draw 2D FPS counter text on top each frame. No crash, no leak.
29. **Memory:** after 1000 frames, PMM and heap stats identical to post-init. Zero per-frame allocation.
30. **Shutdown:** `chaos_gl_shutdown()`. PMM returns to pre-init levels. Re-calling `chaos_gl_init()` succeeds.

---

## Summary

| Property | Value |
|----------|-------|
| Role | Universal OS graphics subsystem — 2D and 3D |
| Language | C, RENDERER_CFLAGS (SSE2, -march=core2) |
| Pixel format | BGRX 32bpp throughout |
| Back buffer | PMM, PAGE_RESERVED, single rep movsd blit to VRAM |
| Z-buffer | 16-bit fixed-point, NDC z → [0, 65535], late-z |
| 2D | Full primitive set, clip stack, Claude Mono text, image blit |
| 3D | Full perspective pipeline, z-buffer, shader interface |
| Clipping | Near-plane only (Sutherland-Hodgman, 0–2 output tris) |
| Backface culling | Clip-space signed area |
| Fragment discard | gl_frag_out_t.discard — no magic colour |
| UV interpolation | Perspective-correct |
| Texture sampling | Nearest-neighbour (v2), bilinear future |
| Matrix convention | Column-major, m[col][row] |
| Rotation order | Y→X→Z for set_transform |
| Max textures | 64 |
| Max tex size | 1024×1024 |
| Max shaders | 32 |
| Max model verts | 65536 |
| Max model faces | 131072 |
| Max clip depth | 16 |
| Model format | .cobj binary (host-side converter from .obj) |
| Texture format | .raw (RAWT magic + dims + BGRX pixels) |
| Font | Claude Mono 8×16 bitmap |
| Lua API | Full 2D + 3D + frame control |
| fb driver role | HAL only — fb_get_info() + VBE init. No drawing. |
| Reference | tinyrenderer (ssloy) lessons 0–6bis for 3D pipeline |
