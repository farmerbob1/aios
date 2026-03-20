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

**The VBE framebuffer driver** is demoted to a hardware abstraction layer. Its job is: initialise VBE, provide VRAM address and dimensions via `fb_get_info()`. It has no drawing primitives. ChaosGL is the drawing API.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    AIOS Applications                     │
│   Shell task  │  File Browser task  │  Doom task  │ ... │
└──────┬──────────────────┬───────────────────┬───────────┘
       │                  │                   │
       ▼                  ▼                   ▼
  Surface A          Surface B           Surface C
  (shell window)     (file browser)      (fullscreen 3D)
  front/back buf     front/back buf      front/back buf
                                         + z-buffer
       │                  │                   │
       └──────────────────┼───────────────────┘
                          │
              ┌───────────▼──────────────┐
              │       Compositor         │
              │  reads front buffers     │
              │  sorts by z-order        │
              │  blits into comp buffer  │
              └───────────┬──────────────┘
                          │
              ┌───────────▼──────────────┐
              │  Compositing Buffer (RAM) │
              │  rep movsd → VRAM        │
              └──────────────────────────┘
                          │
              ┌───────────▼──────────────┐
              │  VBE Framebuffer (HAL)    │
              │  fb_get_info() — addr,    │
              │  dims, pitch              │
              └──────────────────────────┘
                          │
                   VBE Linear Framebuffer (VRAM)
```

**Surface model:** every app task renders into its own off-screen surface — a pixel buffer it owns entirely. The compositor assembles all visible surfaces into the final frame and blits to VRAM. Apps never touch the compositing buffer or VRAM directly.

**Double buffering per surface:** each surface has two pixel buffers (`bufs[0]` and `bufs[1]`) and a `buf_index` byte. The compositor reads `bufs[buf_index]` (the front buffer) and the app draws to `bufs[1 - buf_index]` (the back buffer). `surface_present()` flips `buf_index` with a single byte write, which is atomic on x86 — no locks, no `cli/sti`. The compositor always reads a complete frame, and the app always writes without stalling.

**Per-task surface binding:** the currently bound surface is stored in the task struct, not a global. When a draw call executes, it looks up the current task's bound surface. This prevents preemption bugs — if task A is preempted mid-draw, task B binds its own surface, and when A resumes it continues drawing to its own surface correctly.

---

## Frame Model

Two independent loops run concurrently:

### Compositor Task (owns the screen, runs at 60fps)

```
while true:
    chaos_gl_compose(desktop_background_color)
    task_sleep(16)
```

The compositor does not wait for apps. If an app hasn't called `surface_present()` since the last compose, the compositor uses the surface's previous front buffer. The compositor never stalls.

### App Tasks (render when state changes)

```
chaos_gl_surface_bind(my_surface)
chaos_gl_surface_clear(my_surface, bg_color)
... draw calls — same 2D/3D API, targets bound surface's back buffer ...
chaos_gl_surface_present(my_surface)
task_sleep(16)  -- or event-driven: only redraw on input/state change
```

Apps do not wait for the compositor. If an app renders faster than 60fps, only the latest presented frame gets composited. These are fully decoupled.

**Frame pacing:** the scheduler ticks at 250Hz. The target compose rate is 60fps — one frame every ~16.6ms, or roughly every 4 scheduler ticks. The compositor task uses `task_sleep(16)` at the end of each compose to yield the CPU and pace itself to ~60fps. The actual rate will be ~62.5fps (250/4) due to tick granularity — this is acceptable.

**Target rate:** 60 fps compositing at 1024x768. The compose pass is a series of surface blits — fast. The bottleneck is app rendering, which is decoupled.

---

## Surfaces

### Surface Struct

```c
#define CHAOS_GL_MAX_SURFACES  32

typedef struct {
    /* Double-buffered pixel data — PMM allocated, BGRX.
     * bufs[buf_index] is the front buffer (compositor reads).
     * bufs[1 - buf_index] is the back buffer (app draws).
     * surface_present() flips buf_index (single byte write, atomic on x86). */
    uint32_t*  bufs[2];
    uint8_t    buf_index;   /* 0 or 1 — compositor reads bufs[buf_index] */
    uint16_t*  zbuffer;     /* depth buffer — PMM allocated, NULL if 2D-only */
    int        width;
    int        height;

    /* Position and ordering in the composited frame */
    int        screen_x;
    int        screen_y;
    int        z_order;     /* lower = further back, compositor sorts ascending */

    /* Compositor blending */
    uint8_t    alpha;       /* surface-wide opacity: 255 = fully opaque (default),
                             * 0 = fully transparent. Compositor uses this for
                             * modal overlays, fade effects, etc. */

    /* State */
    bool       in_use;
    bool       visible;     /* false = compositor skips this surface */
    bool       dirty;       /* set by present(), cleared by compositor */

    /* Per-surface clip stack (not global) */
    rect_t     clip_stack[CHAOS_GL_CLIP_STACK_DEPTH];
    int        clip_depth;

    /* Per-surface 3D state */
    mat4_t     view;
    mat4_t     projection;
    mat4_t     model;
    gl_vert_fn active_vert;
    gl_frag_fn active_frag;
    void*      active_uniforms;

    /* PMM tracking (implementation detail — needed for destroy/resize) */
    uint32_t   bufs_pages[2]; /* page count for each pixel buffer */
    uint32_t   zbuf_pages;    /* page count for z-buffer (0 if none) */

    /* Per-surface stats — reset by surface_clear() */
    chaos_gl_stats_t stats;
} chaos_gl_surface_t;
```

**Most surfaces are 2D-only.** Window chrome, text, icons, taskbar — none of these need a z-buffer. Only 3D viewports (Doom, model viewer) need depth. Create with `has_depth = false` by default. A 400x300 2D surface costs ~480KB. The same surface with a z-buffer costs ~720KB. Don't waste memory on depth buffers for UI.

### Surface API

```c
/* Create a surface. has_depth = true allocates a z-buffer (needed for 3D).
 * Both front and back pixel buffers allocated from PMM, marked PAGE_RESERVED.
 * Identity-mapped in VMM with PRESENT | WRITABLE (cached RAM).
 * Returns handle >= 0 on success, -1 if surface table full or OOM. */
int  chaos_gl_surface_create(int w, int h, bool has_depth);

/* Destroy surface and free all PMM pages (front, back, zbuffer).
 * Automatically becomes invisible to compositor. */
void chaos_gl_surface_destroy(int handle);

/* Bind this surface as the current draw target for the calling task.
 * Stored in the task struct — not a global. Each task has its own binding.
 * All subsequent 2D/3D draw calls target this surface's back buffer.
 * Must be called before any draw calls. */
void chaos_gl_surface_bind(int handle);

/* Clear the surface's back buffer to color and reset z-buffer
 * (if present) to ZDEPTH_MAX. Call at the start of each draw sequence. */
void chaos_gl_surface_clear(int handle, uint32_t color_bgrx);

/* Flip buf_index (single byte write — atomic on x86, no lock needed).
 * The compositor will read the new front buffer on its next compose pass.
 * The app can immediately start drawing to the (now-swapped) back buffer.
 * Sets dirty = true. */
void chaos_gl_surface_present(int handle);

/* Position and ordering */
void chaos_gl_surface_set_position(int handle, int x, int y);
void chaos_gl_surface_get_position(int handle, int* x, int* y);
void chaos_gl_surface_set_zorder(int handle, int z);
int  chaos_gl_surface_get_zorder(int handle);
void chaos_gl_surface_set_visible(int handle, bool visible);

/* Surface-wide opacity for compositor blending.
 * 255 = fully opaque (default), 0 = fully transparent.
 * Used for modal overlays, fade effects, window transparency.
 * The compositor blends surfaces with alpha < 255 over the layers below. */
void chaos_gl_surface_set_alpha(int handle, uint8_t alpha);

/* Resize a surface. Frees old PMM pages, allocates new ones.
 * Content is lost — both front and back are cleared.
 * Returns 0 on success, -1 on OOM. */
int  chaos_gl_surface_resize(int handle, int w, int h);

/* Get surface dimensions (for apps that need to query their own size). */
void chaos_gl_surface_get_size(int handle, int* w, int* h);
```

### Z-Order Convention

```
z_order 0:       desktop wallpaper surface
z_order 1-99:    normal application windows
z_order 100:     taskbar / always-on-top UI
z_order 200:     system dialogs
z_order 300:     Claude overlay
z_order 1000:    screen lock, crash overlay
```

### Input Routing Note

The window manager uses surface positions, dimensions, z-order, and visibility to hit-test mouse clicks and route keyboard input to the correct app task. ChaosGL provides the spatial data; input routing is the WM's responsibility. The WM iterates visible surfaces in reverse z-order (front to back) to find which surface the cursor is over.

---

## Compositor

```c
/* Compose all visible surfaces into the compositing buffer, then blit to VRAM.
 * Called by the compositor task once per frame. NOT called by apps.
 *
 * Compose pass:
 *   1. Build dirty region: union of screen-space rects for all surfaces
 *      with dirty==true. If no surface is dirty, skip steps 2-5 entirely
 *      (no VRAM write — pure idle frame).
 *   2. Clear dirty region of compositing buffer to desktop_clear_color
 *   3. Sort visible surfaces by z_order (ascending — low z drawn first)
 *   4. For each visible surface that intersects the dirty region:
 *      - If surface alpha == 255: opaque blit of surface->bufs[buf_index]
 *        to compositing buffer at (screen_x, screen_y), clipped to
 *        screen bounds and dirty region
 *      - If surface alpha == 0: skip entirely
 *      - If surface alpha 1-254: alpha-blend each pixel:
 *          dst = (src * alpha + dst * (255 - alpha)) / 255
 *        (fast approximation: (src * alpha + dst * (255 - alpha) + 128) >> 8)
 *   5. rep movsd: only dirty rows of compositing buffer → VRAM
 *   6. Clear dirty flags on all composited surfaces
 *
 * Surfaces with visible=false are skipped entirely.
 * No register/unregister — visibility controls inclusion.
 *
 * Dirty-region optimisation: when a surface with alpha < 255 is dirty,
 * the dirty region expands to include ALL surfaces below it that overlap
 * (since their content shows through). This is the cost of transparency. */
void chaos_gl_compose(uint32_t desktop_clear_color);
```

**No separate register/unregister step.** A surface exists or it doesn't. `visible` controls whether the compositor includes it. Destroying a surface automatically removes it from compositing.

**Dirty-region tracking:** the compositor maintains a dirty rectangle (union of all dirty surface screen-space rects). Only the dirty region is re-composited and blitted to VRAM. For a mostly-static desktop, this reduces VRAM writes from 3MB/frame to near zero. When a transparent surface (alpha < 255) is dirty, the dirty region must expand to include surfaces below it that it overlaps, since their content shows through and must be re-composited.

**Surface alpha blending:** surfaces with `alpha == 255` (the default) are opaque — fast `rep movsd` blit, no per-pixel math. Surfaces with `alpha` between 1 and 254 use per-pixel alpha blending during compositing. `alpha == 0` surfaces are invisible (equivalent to `visible = false` but preserves the visible flag for restore). This enables modal dialog overlays, fade-in/out effects, and window transparency without per-pixel alpha in the surface buffer itself.

**The compositing buffer** is the old "back buffer" — a single BGRX pixel array the size of the screen, PMM-allocated. Only the compositor writes to it. Only `chaos_gl_compose()` blits it to VRAM.

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
/* Column-major 4x4 matrix — m[col][row].
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

## Z-Buffer

PMM-allocated uint16_t array, same dimensions as the **surface** (not the screen). Only allocated for surfaces created with `has_depth = true`. Used by the 3D pipeline only — 2D draws bypass the z-buffer entirely.

**16-bit fixed-point depth.** Maps post-perspective-divide NDC z [-1, 1] to [0, 65535]. 0 = nearest, 65535 = farthest. Cleared to 0xFFFF by `surface_clear()`.

Why 16-bit: a 1024x768 z-buffer = 1.5MB vs 3MB for float. Sufficient precision for Doom-scale scenes. SSE2 can compare 8 x uint16_t per instruction.

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

## Pixel Format

**BGRX 32bpp throughout.** Blue in the low byte, matching VBE hardware layout. No conversion needed on blit. Surface pixel buffers and the compositing buffer store BGRX — the high byte (X) is unused per-pixel. Surface-wide transparency is controlled by the `alpha` field on the surface struct (see Compositor section), not by per-pixel alpha in the surface buffer. Per-pixel alpha is only used during `blit_alpha()` source data (BGRA input → BGRX surface).

```c
/* Colour construction helper — takes R,G,B, packs as BGRX for VBE */
#define CHAOS_GL_RGB(r, g, b)  ((uint32_t)((b) | ((g) << 8) | ((r) << 16)))
```

---

## 2D Subsystem

All 2D draw calls target the **currently bound surface's back buffer**. The clip stack is per-surface.

### Clip Stack

```c
#define CHAOS_GL_CLIP_STACK_DEPTH  16

/* Push a new clip rect (intersected with current top — clips never expand).
 * Operates on the calling task's bound surface. */
void chaos_gl_push_clip(rect_t r);

/* Pop the top clip rect */
void chaos_gl_pop_clip(void);

/* Clear clip stack — full surface is drawable */
void chaos_gl_reset_clip(void);
```

**Clip intersection contract:** pushing a child clip never expands beyond the parent. If a child rect extends outside the parent, it is clamped. This prevents child windows from drawing outside their parent.

**Per-surface clip state:** each surface has its own clip stack. Binding a different surface does not affect another surface's clip state. When you bind surface A, push a clip, bind surface B, B has a clean clip stack. Bind A again — A's clip is still pushed.

### Primitives

All 2D draws write directly to the bound surface's back buffer. No z-test, no z-write. Drawn in call order — later calls overdraw earlier calls.

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
/* Blit a BGRX pixel array to the bound surface at (x, y).
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
 * The surface buffer remains BGRX — alpha is consumed during the blit, not stored.
 * Performs src-over compositing per pixel. Slower than plain blit — use for
 * icons with transparency, UI overlays with smooth edges. */
void chaos_gl_blit_alpha(int x, int y, int w, int h,
                          const uint32_t* src, int src_pitch);
```

### Text

Uses Claude Mono — the custom 8x16 bitmap font. Each glyph is 8 pixels wide, 16 pixels tall. Full printable ASCII.

```c
/* Text flags — OR together */
#define CHAOS_GL_TEXT_BG_TRANSPARENT  0x0   /* default: skip bg pixels */
#define CHAOS_GL_TEXT_BG_FILL        0x1   /* fill bg pixels with bg colour */

/* Draw a single character. Returns x advance (always 8 for Claude Mono).
 * flags: CHAOS_GL_TEXT_BG_TRANSPARENT or CHAOS_GL_TEXT_BG_FILL.
 * When BG_TRANSPARENT (default, 0), only foreground pixels are written.
 * When BG_FILL, background pixels are filled with bg colour (including black). */
int  chaos_gl_char(int x, int y, char c, uint32_t fg, uint32_t bg, uint32_t flags);

/* Draw a null-terminated string. Returns width in pixels drawn.
 * flags: same as chaos_gl_char. */
int  chaos_gl_text(int x, int y, const char* str,
                   uint32_t fg, uint32_t bg, uint32_t flags);

/* Draw string with word-wrap within max_w pixels.
 * Returns total height in pixels used. */
int  chaos_gl_text_wrapped(int x, int y, int max_w, const char* str,
                            uint32_t fg, uint32_t bg, uint32_t flags);

/* Measure string width in pixels without drawing. */
int  chaos_gl_text_width(const char* str);

/* Measure wrapped text height without drawing. */
int  chaos_gl_text_height_wrapped(int max_w, const char* str);
```

**Transparent background (default):** when `flags == 0` (CHAOS_GL_TEXT_BG_TRANSPARENT), only foreground pixels are written. Background pixels are skipped. This allows text to be drawn over any surface content. To fill the background with a solid colour (including true black `0x00000000`), pass `CHAOS_GL_TEXT_BG_FILL`.

---

## 3D Pipeline

### Overview

All 3D draw calls target the **currently bound surface's back buffer and z-buffer**. The surface must have been created with `has_depth = true` — calling 3D draw functions on a 2D-only surface is undefined behaviour (debug builds assert).

3D state (view, projection, model matrices, active shader) is stored per-surface and restored when a surface is bound.

```
Per-triangle call: chaos_gl_triangle(v0, v1, v2)
    |
    +-> Vertex shader (C fn ptr) x 3
    |       Inputs:  model-space position, normal, UV + uniforms
    |       Outputs: clip-space position, view-space normal, UV, intensity
    |       Vertex shader owns the ModelView x Projection multiply
    |
    +-> Backface cull — signed area in clip space, skip if back-facing
    |
    +-> Near-plane clip — Sutherland-Hodgman, 0-2 output triangles
    |
    +-> Guard-band clip — reject triangles entirely outside 4x viewport
    |       in clip space (see Guard-Band Clipping section below)
    |
    +-> Perspective divide — clip.xyz / clip.w -> NDC [-1,1]^3
    |
    +-> Viewport transform
    |       screen_x = (ndc_x + 1.0) * surface_w / 2
    |       screen_y = (1.0 - ndc_y) * surface_h / 2   (Y-flip: NDC up = screen down)
    |
    +-> Rasterize (barycentric coords, perspective-correct interpolation)
    |       Bounding box clamped to surface dimensions (0..w-1, 0..h-1)
    |       Per pixel:
    |           Fragment shader -> gl_frag_out_t (color + discard flag)
    |           Z-test (16-bit)
    |           Write to surface back buffer (if not discarded and z passes)
```

**Viewport transform uses surface dimensions**, not screen dimensions. A 3D app rendering into a 400x300 surface has a 400x300 viewport.

**Rasterizer bounding-box clamp:** the rasterizer computes the triangle's screen-space bounding box and clamps it to `[0, surface_width-1]` x `[0, surface_height-1]` before iterating pixels. This is critical for correctness (prevents out-of-bounds writes) and performance (limits iteration for triangles that extend off-screen). Combined with guard-band clipping, this keeps the iteration region small.

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
    int    x, y;       /* screen coordinates (relative to surface, not screen) */
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

### Guard-Band Clipping

After near-plane clipping and before perspective divide, triangles can still have vertices that project far outside the viewport. Without guard-band clipping, these triangles produce enormous screen-space bounding boxes, causing the rasterizer to iterate over millions of off-screen pixels — a classic software renderer performance trap.

**Guard-band clip** tests post-near-clip triangles against an expanded clip volume: `±GUARD_BAND_MULT * w` on X and Y (where `w` is the clip-space W coordinate). Triangles entirely outside the guard band are trivially rejected. Triangles that cross the guard band but stay within it are passed through to the rasterizer — the BB clamp handles the rest. Only triangles that extend beyond the guard band on X or Y need Sutherland-Hodgman clipping against those planes.

```c
#define CHAOS_GL_GUARD_BAND_MULT  4.0f  /* 4x viewport = generous guard band */

/* Test if a post-near-clip triangle is entirely outside the guard band.
 * Returns true if the triangle should be discarded (all 3 verts outside
 * the same guard-band edge). Called after near-plane clip, before
 * perspective divide.
 *
 * Guard-band test in clip space (per vertex):
 *   inside guard band if: -MULT*w <= x <= MULT*w  AND  -MULT*w <= y <= MULT*w
 * Trivial reject if all 3 vertices are outside the SAME edge. */
bool chaos_gl_guardband_reject(gl_vertex_out_t v[3]);

/* Clip triangle against guard-band edges (±MULT*w on X and Y).
 * Only called when a triangle extends beyond the guard band but is not
 * trivially rejected. Uses Sutherland-Hodgman against the 4 side planes.
 * out_verts: room for 8 vertices (convex polygon after 4-plane clip).
 * Returns number of output vertices (3-8), or 0 if fully clipped.
 * Caller triangulates the output polygon as a fan: (0,1,2), (0,2,3), ... */
int chaos_gl_guardband_clip(gl_vertex_out_t in[3],
                             gl_vertex_out_t out_verts[8]);
```

**In practice, most triangles pass the guard-band trivial-accept test** (all vertices inside `±4w`) and go straight to the rasterizer with just a BB clamp — no per-vertex clipping needed. Only extreme cases (very large triangles near the camera edges) trigger the full guard-band Sutherland-Hodgman clip. The `GUARD_BAND_MULT` of 4 means a triangle must extend to 4x the viewport before clipping kicks in — at that point the rasterizer BB would be 16x the viewport area, so clipping is justified.

**Pipeline integration:**
```
near_clip_result = chaos_gl_clip_near(...)
for each output triangle from near clip:
    if chaos_gl_guardband_reject(tri): skip (stats: triangles_clipped++)
    else if all 3 verts inside guard band: proceed to perspective divide
    else: chaos_gl_guardband_clip(tri, ...) → 0-6 sub-triangles
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

/* Set active shader by name on the currently bound surface.
 * Returns 0 on success, -1 if not found. */
int chaos_gl_shader_set_by_name(const char* name, void* uniforms);

/* Set active shader directly by function pointers on the currently bound surface. */
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
    uint32_t  phys_addr; /* PMM physical address (for free) */
    uint32_t  pages;     /* PMM page count (for free) */
} chaos_gl_texture_t;
```

**Texture memory:** pixel data allocated from PMM (`pmm_alloc_pages()`), not the heap. Textures are large, page-aligned, long-lived — they don't belong in the slab/buddy allocator. Pages marked `PAGE_RESERVED`. Identity-mapped in VMM with PRESENT | WRITABLE (cached RAM).

**Texture pitch:** for `.raw` format, pitch == width always. The field exists for forward compatibility. All sampling code must use pitch for row stride, never width.

**Textures are global, not per-surface.** Any surface can reference any loaded texture. The texture table is shared.

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
/* Followed by width x height x 4 bytes of BGRX pixel data, row-major, no padding */
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

**Models are global, not per-surface.** Any surface can draw any loaded model.

---

## Public API

### Lifecycle

```c
/* Initialise ChaosGL. Must be called after fb_get_info() returns valid data.
 * Allocates compositing buffer from PMM, maps in VMM,
 * registers built-in shaders, initialises texture and surface tables.
 * Returns 0 on success, -1 on failure (OOM). */
int  chaos_gl_init(void);

/* Shutdown — free compositing buffer, all surfaces, all loaded textures.
 * PMM stats should return to pre-init levels after this call. */
void chaos_gl_shutdown(void);
```

### Camera and Projection

These set state on the **currently bound surface**.

```c
/* Set view matrix from camera parameters. Stored in bound surface.
 * Combined with model matrix on each 3D draw call: MVP = proj x view x model. */
void chaos_gl_set_camera(vec3_t eye, vec3_t center, vec3_t up);

/* Set view matrix directly (bypasses lookat). */
void chaos_gl_set_view(mat4_t view);

/* Set perspective projection.
 * fovy_deg: vertical FOV in degrees. aspect: width/height (0 = auto from surface dims).
 * z_near, z_far: positive clip distances (e.g. 0.1, 1000.0). */
void chaos_gl_set_perspective(float fovy_deg, float aspect,
                               float z_near, float z_far);

/* Set projection matrix directly. */
void chaos_gl_set_projection(mat4_t proj);
```

### Model Transform

```c
/* Set model transform from TRS components.
 * Builds: model = T(tx,ty,tz) x Ry(ry) x Rx(rx) x Rz(rz) x S(sx,sy,sz)
 * Rotation order Y->X->Z (yaw->pitch->roll, matching FPS camera convention).
 * Stored in bound surface. */
void chaos_gl_set_transform(float tx, float ty, float tz,
                             float rx, float ry, float rz,
                             float sx, float sy, float sz);

/* Set model matrix directly. */
void chaos_gl_set_model(mat4_t model);
```

### 3D Draw Calls

```c
/* Set active shader on the bound surface. All subsequent 3D draw calls use this. */
void chaos_gl_shader_set(gl_vert_fn vert, gl_frag_fn frag, void* uniforms);
int  chaos_gl_shader_set_by_name(const char* name, void* uniforms);

/* Draw one triangle to the bound surface. */
void chaos_gl_triangle(gl_vertex_in_t v0, gl_vertex_in_t v1, gl_vertex_in_t v2);

/* Draw all faces of a model with current shader and transform. */
void chaos_gl_draw_model(chaos_gl_model_t* model);

/* Draw model as wireframe (no shader, no z-write, debug only). */
void chaos_gl_draw_model_wire(chaos_gl_model_t* model, uint32_t color);
```

### 2D Draw Calls

```c
/* Clip stack (per-surface) */
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

/* Alpha blend. Source alpha in bits [24..31]. src-over composite. */
void chaos_gl_pixel_blend(int x, int y, uint32_t color_bgra);

/* Image blit */
void chaos_gl_blit(int x, int y, int w, int h,
                   const uint32_t* src, int src_pitch);
void chaos_gl_blit_keyed(int x, int y, int w, int h,
                          const uint32_t* src, int src_pitch,
                          uint32_t key_color);
void chaos_gl_blit_alpha(int x, int y, int w, int h,
                          const uint32_t* src, int src_pitch);

/* Text (Claude Mono 8x16) — flags: 0 = transparent bg, CHAOS_GL_TEXT_BG_FILL = solid bg */
int  chaos_gl_char(int x, int y, char c, uint32_t fg, uint32_t bg, uint32_t flags);
int  chaos_gl_text(int x, int y, const char* str,
                   uint32_t fg, uint32_t bg, uint32_t flags);
int  chaos_gl_text_wrapped(int x, int y, int max_w, const char* str,
                            uint32_t fg, uint32_t bg, uint32_t flags);
int  chaos_gl_text_width(const char* str);
int  chaos_gl_text_height_wrapped(int max_w, const char* str);
```

### Stats

```c
typedef struct {
    /* 3D pipeline counters — reset each surface_clear() */
    uint32_t triangles_submitted;
    uint32_t triangles_culled;     /* backface culled */
    uint32_t triangles_clipped;    /* near-plane clip removed or reduced */
    uint32_t triangles_drawn;      /* reached rasterizer (may exceed submitted
                                    * minus culled/clipped due to clip splits) */
    uint32_t pixels_written;       /* passed z-test, written to surface */
    uint32_t pixels_zfailed;       /* failed z-test */
    uint32_t pixels_discarded;     /* discarded by fragment shader */

    /* 2D counters — reset each surface_clear() */
    uint32_t draw_calls_2d;        /* total 2D primitive calls this frame */

    /* Timing */
    uint32_t frame_time_us;        /* last frame time in microseconds (RDTSC) */
    uint32_t frame_3d_us;          /* time spent in 3D pipeline */
    uint32_t frame_2d_us;          /* time spent in 2D draws */

    /* Compositor timing — set by chaos_gl_compose() */
    uint32_t compose_time_us;      /* time spent in last compose pass */
    uint32_t compose_blit_us;      /* time spent in VRAM blit */
    uint32_t surfaces_composited;  /* number of visible surfaces blitted */
} chaos_gl_stats_t;

/* Per-surface stats (from the bound surface) */
chaos_gl_stats_t chaos_gl_get_stats(void);

/* Compositor stats (global, from last compose pass) */
chaos_gl_stats_t chaos_gl_get_compose_stats(void);
```

---

## Lua API (Deferred — Phase 7)

**Not implemented in Phase 5.** Lua bindings will be added when the Lua runtime is integrated (Phase 7). The API below is the target interface — `lua_bindings.c` will register these functions via `lua_register()` during Lua init.

All functions registered via `lua_register()` during Lua subsystem init (not `chaos_gl_init()`).

```lua
-- Surface management
local surf = chaos_gl.surface_create(w, h, has_depth)  -- has_depth: boolean
chaos_gl.surface_destroy(surf)
chaos_gl.surface_bind(surf)
chaos_gl.surface_clear(surf, color_bgrx)
chaos_gl.surface_present(surf)
chaos_gl.surface_set_position(surf, x, y)
local x, y = chaos_gl.surface_get_position(surf)       -- returns x, y
chaos_gl.surface_set_zorder(surf, z)
local z = chaos_gl.surface_get_zorder(surf)             -- returns z
chaos_gl.surface_set_visible(surf, visible)
chaos_gl.surface_set_alpha(surf, alpha)                 -- 0-255, default 255
chaos_gl.surface_resize(surf, w, h)

-- Camera / projection (operates on bound surface)
chaos_gl.set_camera(ex,ey,ez, cx,cy,cz, ux,uy,uz)
chaos_gl.set_perspective(fovy_deg, aspect, near, far)  -- aspect=0 for auto

-- 3D transform (operates on bound surface)
chaos_gl.set_transform(tx,ty,tz, rx,ry,rz, sx,sy,sz)

-- 3D draw (draws to bound surface)
local model = chaos_gl.load_model("/models/barrel.cobj")
chaos_gl.draw_model(model, shader_name, uniforms_table)
chaos_gl.draw_wireframe(model, color_bgrx)
chaos_gl.free_model(model)

-- Textures (global, not per-surface)
local tex = chaos_gl.load_texture("/textures/wall.raw")
chaos_gl.free_texture(tex)

-- 2D clip (per-surface)
chaos_gl.push_clip(x, y, w, h)
chaos_gl.pop_clip()
chaos_gl.reset_clip()

-- 2D primitives (draws to bound surface)
chaos_gl.rect(x, y, w, h, color)
chaos_gl.rect_outline(x, y, w, h, color, thickness)
chaos_gl.rect_rounded(x, y, w, h, radius, color)
chaos_gl.circle(x, y, radius, color)
chaos_gl.line(x0, y0, x1, y1, color)
chaos_gl.pixel(x, y, color)

-- 2D image (texture handle from load_texture)
chaos_gl.blit(x, y, w, h, tex_handle)
chaos_gl.blit_keyed(x, y, w, h, tex_handle, key_color)

-- 2D text (Claude Mono 8x16)
-- flags: 0 = transparent bg (default), 1 = solid bg fill
chaos_gl.text(x, y, str, fg, bg, flags)
chaos_gl.text_wrapped(x, y, max_w, str, fg, bg, flags)
chaos_gl.text_width(str)                    -- returns pixel width
chaos_gl.text_height_wrapped(max_w, str)    -- returns pixel height without drawing
chaos_gl.char(x, y, c, fg, bg, flags)

-- Stats
local s = chaos_gl.get_stats()           -- per-surface stats
local c = chaos_gl.get_compose_stats()   -- compositor stats
```

### Lua Uniform Table — Built-in Shader Layouts

| Shader | Required | Optional | Defaults |
|--------|----------|----------|---------|
| `"flat"` | — | `color` (BGRX uint32) | color=0x00FFFFFF |
| `"diffuse"` | `texture` (path or handle) | `light_dir_x/y/z`, `ambient` | light=(0,-1,0), ambient=0.1 |
| `"gouraud"` | — | `light_dir_x/y/z`, `ambient`, `color` | light=(0,-1,0), ambient=0.1, color=0x00FFFFFF |
| `"normalmap"` | `texture`, `normalmap` | `light_dir_x/y/z`, `ambient` | light=(0,-1,0), ambient=0.1 |

**Texture in uniforms:** either a path string (`"/textures/wall.raw"`) or an integer handle from `load_texture()`. Path strings are resolved once per unique path per frame and cached. Missing path -> 1x1 magenta fallback, logged as warning, draw call continues.

---

## Dependencies

### VBE Framebuffer Driver (Phase 3 HAL)

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

**The framebuffer driver spec is revised accordingly:** `fb_putpixel`, `fb_rect`, `fb_text`, `fb_char`, `fb_swap` and all other drawing functions are removed. Only `fb_init()` (called at boot), `fb_get_info()`, and internal VBE management remain.

### PMM

Compositing buffer, surface pixel buffers (front + back), z-buffers, and texture pixel data allocated from PMM. All marked `PAGE_RESERVED`.

### VMM

All PMM-allocated ChaosGL buffers mapped in VMM with PRESENT | WRITABLE (cached RAM).

### Heap

Model geometry allocated from heap (kmalloc). Small, variable-size, relatively short-lived. Surface table and internal state also from heap.

### ChaosFS

Texture and model loading reads from ChaosFS.

### RDTSC

Frame timing via `rdtsc()` / `rdtsc_to_us()` from the RDTSC subsystem.

### Scheduler / Task Struct

Per-task surface binding requires a `chaos_gl_surface_t*` (or handle) field in the task struct. The scheduler does not need to know about ChaosGL otherwise — binding is set by the app task and read by ChaosGL draw calls via `current_task()`.

---

## Project Structure

```
renderer/
+-- math.c / math.h           # vec2/3/4, mat4, rect_t, all math + custom trig
+-- surface.c / .h            # surface create/destroy/bind/clear/present/resize
+-- compositor.c / .h         # compose pass, compositing buffer, VRAM blit
+-- 2d.c / 2d.h               # all 2D primitives, clip stack, text, blit
+-- font.c / font.h           # Claude Mono 8x16 bitmap glyph data (IBM VGA CP437)
+-- pipeline.c / .h           # vertex transform, clip, perspective divide, viewport
+-- rasterizer.c / .h         # triangle rasterization, z-test, barycentric interp
+-- shaders.c / .h            # built-in shader implementations + registry
+-- texture.c / .h            # texture table, load (.raw), sample
+-- model.c / .h              # .cobj load/free, draw_model, draw_model_wire
+-- chaos_gl.c / chaos_gl.h   # public C API — single include for everything
+-- lua_bindings.c            # (Phase 7) Lua API registration and dispatch

tools/
+-- gen_assets.py             # generates test .raw textures and .cobj models into ChaosFS
+-- obj2cobj.c                # (future) host tool: .obj + .mtl -> .cobj
```

`chaos_gl.h` is the single public header. Applications include only this. Internal modules include their own headers directly.

---

## Memory Budget (1024x768)

| Region | Size | Allocator |
|--------|------|-----------|
| Compositing buffer (BGRX) | 3.0 MB | PMM |
| Desktop surface (fullscreen, 2D) | 6.0 MB | PMM (front + back) |
| 3x app windows ~400x300, 2D | 2.9 MB | PMM (front + back each) |
| 1x 3D surface 400x300 + z-buf | 1.2 MB | PMM (front + back + zbuf) |
| Typical textures (8 x 256x256) | 2.0 MB | PMM |
| Model geometry (typical scene) | 1-4 MB | Heap |
| Surface table + internal state | < 4 KB | Heap |
| **Total typical** | **~16 MB** | |

Fixed overhead: 3.0MB (compositing buffer). Per-surface overhead: 2x pixel buffer (front+back) + optional z-buffer. With 256MB RAM and kernel taking ~10MB, comfortable headroom.

Texture budget is the variable. 64 x 1024x1024 = 256MB theoretical maximum — never approach this in practice. Doom-style 256x256 textures at 16 active = 4MB. PMM enforces the limit at runtime.

---

## Test Assets

Generated by the build system. Required on disk before Phase 5 tests run.

| Path | Contents | Used in tests |
|------|----------|---------------|
| `/test/cube.cobj` | Unit cube, 12 triangles | 3D pipeline tests |
| `/test/quad.cobj` | Flat quad, 2 triangles, UV 0..1 | Texture, UV, normal map tests |
| `/test/sphere.cobj` | UV sphere, ~320 triangles | Gouraud shading test |
| `/test/white.raw` | 64x64 solid white | Flat shading, z-buffer tests |
| `/test/grid.raw` | 64x64 UV grid | Texture mapping, perspective-correct UV |
| `/test/flat_normal.raw` | 64x64 flat normal map (0.5, 0.5, 1.0) | Normal map baseline |
| `/test/bump_normal.raw` | 64x64 bumped normal map | Normal map effect test |

---

## Phase 5 Acceptance Tests

**Implementation status:** 21 of the tests below are implemented and passing in `kernel/main.c`. Tests marked with (DEFERRED) are specified but not yet implemented — they cover advanced features like perspective-correct UV validation, per-task concurrency, dirty-region optimization verification, and Lua integration. The core pipeline is fully tested; deferred tests are for edge cases and visual correctness that can be validated visually via `run.bat`.

### 2D Subsystem

1. **Clear:** create surface, `surface_clear(0x000000FF)` fills red (BGRX). Compose. Visual verify.
2. **Rect:** draw a 200x100 white rect at (50,50) on a surface. Correct position, correct fill, no bleed.
3. **Rect outline:** draw a 3px red outline rect. Correct thickness, corners correct.
4. **Rounded rect:** draw a rounded rect with radius 10. Corners are visibly rounded.
5. **Circle:** draw a filled circle and an outline circle. Both correct shape.
6. **Line:** draw from (0,0) to (w-1,h-1) on a surface. Both endpoints hit exactly.
7. **Text transparent bg:** render "Hello AIOS" with Claude Mono and `flags=0` (transparent bg). Characters correct shape, spacing correct. Underlying colour shows through background pixels.
7b. **Text solid bg:** render "Hello AIOS" with `flags=CHAOS_GL_TEXT_BG_FILL` and `bg=0x00000000` (true black). Background pixels filled with black — no transparency. Proves flags-based bg works for all colours including black.
8. **Text wrap:** render a long string with max_w=200. Wraps at word boundary. `text_height_wrapped()` matches actual rendered height.
9. **Clip rect:** push clip (100,100,200,200). Draw a large rect that extends beyond it. Only the intersection is visible. Pop clip — full rect draws.
10. **Clip stack:** push two nested clips. Inner clip is correctly intersected with outer. Pop both — full region drawable again.
11. **Blit:** blit `/test/grid.raw` to surface. UV grid appears correct orientation, no distortion.
12. **Blit keyed:** blit with key colour = black. Black pixels transparent, others visible.

### 3D Pipeline

13. **Flat triangle:** draw one triangle on a depth-enabled surface, flat white shader. Correct shape, no gaps, no bleed.
14. **Z-buffer:** two overlapping triangles at different depths. Draw far one first, then near — near occludes. Swap draw order — same result. Proves z-buffer, not painter's algorithm.
15. **Backface cull:** draw `/test/cube.cobj`. ~6 of 12 triangles culled (back faces). Stats confirm. No visual artifact.
16. **Perspective:** draw `/test/cube.cobj`. Parallel edges converge. Looks 3D.
17. **Texture:** draw `/test/quad.cobj` with `/test/grid.raw`. UV grid correct orientation (U left->right, V top->bottom).
18. **Perspective-correct UV:** draw `/test/quad.cobj` at steep angle. Grid does not swim across the diagonal.
19. **Camera:** move camera via `set_camera()`. Scene changes correctly each frame.
20. **Normal lighting:** diffuse shader on cube. Lit face bright, opposite face at ambient level. Terminator on correct edge.
21. **Gouraud:** `"gouraud"` shader on `/test/sphere.cobj`. Smooth gradient. No per-triangle breaks.
22. **Normal map:** `"normalmap"` on `/test/quad.cobj`. `/test/flat_normal.raw` -> no bump. `/test/bump_normal.raw` -> bumpy.
23. **Near clip:** camera near plane intersects triangle. `triangles_clipped > 0` in stats. No crash, no divide-by-zero, no garbage.
23b. **Guard-band reject:** draw a triangle positioned entirely off-screen to the right (all vertices outside 4x viewport). `triangles_clipped` increments, no pixels drawn, no crash.
23c. **Guard-band clip:** draw a very large triangle with one vertex on-screen and two far off-screen (beyond 4x viewport). Triangle clips correctly — visible portion renders, no rasterizer explosion, frame time remains reasonable.
23d. **Rasterizer BB clamp:** draw a triangle partially off-screen (vertices at negative coordinates). Only visible portion renders. No out-of-bounds write. Stats show correct pixel counts.
24. **Discard:** custom shader discards pixels where UV.u < 0.5. Left half of quad transparent, right half renders. Z not written for discarded pixels (verify by drawing something behind — it shows through the left half).
25. **Stats:** no-clip scene: `triangles_culled + triangles_drawn == triangles_submitted`. Pixel counters sum to total rasterised fragments. Frame timers > 0 and plausible.

### Surface & Compositor

26. **Surface create/destroy:** create surface, verify PMM pages allocated (front + back). Destroy, verify all pages freed.
27. **Surface render:** create surface, bind it, draw a red rect, present, set visible, compose. Red rect appears at correct screen position.
28. **Multi-surface compose:** create 3 surfaces at different z-orders with different colours. Compose — verify z-order is correct (higher z draws on top of lower z).
29. **Double buffer correctness:** app draws to back buffer (bufs[1-buf_index]), compositor reads front buffer (bufs[buf_index]) simultaneously. No tearing — front buffer contains previous complete frame until present() flips buf_index.
29b. **Surface alpha opaque:** create surface with default alpha (255). Compose over a coloured background — surface fully covers background at its position.
29c. **Surface alpha blended:** create surface, `set_alpha(128)`. Compose over a coloured background — surface pixels blend with background (50% mix). Both colours visible.
29d. **Surface alpha zero:** create surface, `set_alpha(0)`. Compose — surface not visible despite `visible=true`. Background shows through completely.
29e. **Position/zorder getters:** create surface, `set_position(100, 200)`, `set_zorder(5)`. `get_position()` returns (100, 200). `get_zorder()` returns 5.
30. **Surface resize:** create surface, render to it, resize it, verify old content gone, render again — works. PMM pages from old size freed.
31. **Clip stack per surface:** bind surface A, push clip. Bind surface B — B has its own clean clip stack. Bind A again — A's clip is still pushed.
32. **3D state per surface:** bind surface A, set camera to position X. Bind surface B, set camera to position Y. Bind A — camera is still at position X.
33. **Per-task binding:** two tasks each bind their own surface and draw concurrently. Neither task corrupts the other's surface. (Requires preemptive scheduler running.)

### Integration

34. **2D over 3D on same surface:** render a 3D cube on a depth-enabled surface, then draw 2D text overlay on top. Text appears over the 3D scene. Z-buffer not involved in 2D draw.
35. **Clip over 3D:** 3D scene renders on a surface, then 2D clip rect restricts a GUI panel drawn on top. 3D scene visible outside the panel, panel content clipped inside.
36. **Lua scene:** Lua script: create surface with depth, load `/test/cube.cobj`, set perspective, spin cube for 60 frames, draw 2D FPS counter text each frame, present each frame. Compose shows spinning cube with overlay. No crash, no leak.
37. **Memory stability:** after 1000 compose cycles, PMM and heap stats identical to post-init. Zero per-frame allocation.
37b. **Dirty-region idle:** no surface has dirty flag set. `chaos_gl_compose()` skips compositing and VRAM blit entirely. Compose time is near-zero.
37c. **Dirty-region partial:** one small surface (100x100) is dirty in the corner of the screen. Only that region is re-composited and blitted to VRAM. Compose time is significantly less than a full-screen compose.
38. **Shutdown:** `chaos_gl_shutdown()`. All surfaces destroyed, compositing buffer freed, PMM returns to pre-init levels. Re-calling `chaos_gl_init()` succeeds.

---

## Summary

| Property | Value |
|----------|-------|
| Role | Universal OS graphics subsystem — 2D and 3D |
| Language | C, RENDERER_CFLAGS (SSE2, -march=core2) |
| Pixel format | BGRX 32bpp throughout |
| Surface model | Double-buffered (index-based swap — single byte write, atomic on x86) |
| Surface alpha | Per-surface uint8_t opacity (255=opaque default, 0=transparent) |
| Compositor | Dirty-region tracked, per-surface alpha blending, rep movsd dirty rows to VRAM |
| Per-task binding | Bound surface stored in task struct, not global |
| Compositing buffer | PMM, PAGE_RESERVED, dirty-region blit to VRAM |
| Z-buffer | 16-bit fixed-point, NDC z -> [0, 65535], late-z, per-surface (opt-in) |
| 2D | Full primitive set, clip stack (per-surface), Claude Mono text (flags-based bg), image blit |
| 3D | Full perspective pipeline, z-buffer, shader interface, state per-surface |
| Clipping | Near-plane + guard-band (4x viewport, Sutherland-Hodgman), rasterizer BB clamp |
| Backface culling | Clip-space signed area |
| Fragment discard | gl_frag_out_t.discard — no magic colour |
| UV interpolation | Perspective-correct |
| Texture sampling | Nearest-neighbour (v2), bilinear future |
| Matrix convention | Column-major, m[col][row] |
| Rotation order | Y->X->Z for set_transform |
| Max surfaces | 32 |
| Max textures | 64 |
| Max tex size | 1024x1024 |
| Max shaders | 32 |
| Max model verts | 65536 |
| Max model faces | 131072 |
| Max clip depth | 16 |
| Model format | .cobj binary (host-side converter from .obj) |
| Texture format | .raw (RAWT magic + dims + BGRX pixels) |
| Font | Claude Mono 8x16 bitmap |
| Lua API | Full surface + 2D + 3D + compositor control |
| fb driver role | HAL only — fb_get_info() + VBE init. No drawing. |
| Input routing | WM uses surface position/size/z-order for hit-testing (not ChaosGL's job) |
| Reference | tinyrenderer (ssloy) lessons 0-6bis for 3D pipeline |
