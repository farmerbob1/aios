# Window Manager & Desktop — Specification
## AIOS v2 Phases 9–10

---

## Preface: What Makes This an OS

Phases 0–8 built the engine. The hardware boots, memory is managed, tasks are scheduled, the filesystem works, the graphics pipeline draws, modules extend the kernel, Lua runs applications, and the widget toolkit provides consistent UI components. But the user doesn't see any of that. They see a desktop with a taskbar, windows they can drag and resize, a file browser, and a place to launch apps. Phases 9 and 10 are where AIOS stops being a tech demo and becomes something a person can sit in front of and use.

**Phase 9** is the window manager — the Lua module that owns the space between applications. It decides which window is in front, routes mouse clicks to the correct app, manages the taskbar, and draws the cursor. It is the air traffic controller of the desktop.

**Phase 10** is the desktop environment itself — the shell, the file browser, the settings panel, and the terminal. Each is a Lua application (~100–300 lines) that follows the app lifecycle pattern from the UI toolkit spec. They are the first real residents of the system Phase 9 manages.

Both phases are entirely Lua. No C code, no kernel changes, no KAOS modules. Everything is built on top of the APIs already specified: ChaosGL surfaces, the UI toolkit widgets, the Lua runtime's `aios.*` libraries, and the input subsystem. If Phase 8 (UI toolkit) passes its acceptance tests, Phases 9 and 10 are pure application-level work.

---

# Graphical Boot Screen — "Icon Parade"

---

## Problem

The current boot sequence prints VGA text to serial. After ChaosGL init (Phase 5), the framebuffer is live but nothing draws to it — the user sees a black screen until the desktop launches. The gap between "ChaosGL init" and "desktop ready" includes KAOS module loading, Lua init, and desktop task startup. This gap should show a graphical boot screen, reminiscent of the classic Mac OS 9 extension loading parade.

## Constraints

- Must be **C code**, not Lua — it runs before Lua is initialized (Phase 7)
- ChaosGL is available (Phase 5 complete)
- ChaosFS is mounted (Phase 4 complete) — can load textures from disk
- KAOS modules load during Phase 6 — this is the icon parade window
- Must not block boot — drawing is synchronous but fast (a few rects + blits per module)
- Must clean up its surface when the desktop shell takes over

## Design

A new C module `kernel/boot_splash.c` owns a single fullscreen ChaosGL surface. It provides hook functions called at key points during boot:

```
Phase 5: ChaosGL init
        |
  boot_splash_init()          <-- create surface, draw logo
        |
Phase 6: KAOS init
        |
  boot_splash_status("Loading modules...")
        |
  kaos_load_all() calls:
    boot_splash_module("hello", icon_tex)   <-- for each module
    boot_splash_module("vfs", icon_tex)
    boot_splash_module("net", icon_tex)
        |
Phase 7: Lua init
        |
  boot_splash_status("Starting Lua runtime...")
        |
  boot_splash_status("Loading desktop...")
        |
  desktop.lua starts --> boot_splash_destroy()
```

## Screen Layout

```
+--------------------------------------------------+
|                                                  |
|                                                  |
|                                                  |
|                    +=========+                   |
|                    |  AIOS   |  <-- logo         |
|                    |   v2    |    (centered)      |
|                    +=========+                   |
|                                                  |
|            Loading modules...       <-- status   |
|                                                  |
|         +--+ +--+ +--+ +--+ +--+               |
|         |  | |  | |  | |  | |  | <-- icon parade|
|         +--+ +--+ +--+ +--+ +--+               |
|                                                  |
|  ==================-----------  <-- progress bar  |
|                                                  |
+--------------------------------------------------+
```

- **Background:** Solid dark color (`0x00201830` — deep navy/purple)
- **Logo:** Drawn programmatically with ChaosGL primitives — a rounded rect "badge" with "AIOS" in large text and "v2" smaller below. No external texture needed.
- **Status text:** Centered below logo, updates at each boot phase.
- **Icon parade:** 32x32 icons appear left-to-right along a horizontal strip near the bottom. Each KAOS module gets an icon. Icons are loaded from `/system/icons/mod_<name>_32.raw` if the file exists on ChaosFS, otherwise a generic "module" icon (gear or puzzle piece) is used.
- **Progress bar:** Below the icon parade. Advances proportionally as modules load. Width = `(modules_loaded / total_modules) * bar_width`.

## API

```c
// kernel/boot_splash.h
#pragma once

// Initialize boot splash — call after chaos_gl_init()
void boot_splash_init(void);

// Update status text (centered below logo)
void boot_splash_status(const char *text);

// Add a module icon to the parade. Called once per module during load.
// name: module name (used to find icon file, and as fallback label)
// loaded: number of modules loaded so far
// total: total number of modules (for progress bar)
void boot_splash_module(const char *name, int loaded, int total);

// Destroy boot splash surface — call when desktop is ready
void boot_splash_destroy(void);
```

## Implementation Notes

```c
// kernel/boot_splash.c

static int splash_surface = -1;
static int icon_x = 0;          // next icon parade X position
static int generic_icon_tex = -1; // fallback icon handle

void boot_splash_init(void) {
    splash_surface = chaos_gl_surface_create(1024, 768, false);
    chaos_gl_surface_set_position(splash_surface, 0, 0);
    chaos_gl_surface_set_zorder(splash_surface, 50);
    chaos_gl_surface_set_visible(splash_surface, true);

    // Draw background
    chaos_gl_surface_bind(splash_surface);
    chaos_gl_surface_clear(splash_surface, 0x00201830);

    // Draw logo — rounded rect badge, centered
    int logo_w = 200, logo_h = 100;
    int logo_x = (1024 - logo_w) / 2, logo_y = 250;
    chaos_gl_draw_rect_rounded(logo_x, logo_y, logo_w, logo_h, 12, 0x00302848);
    chaos_gl_draw_rect_rounded_outline(logo_x, logo_y, logo_w, logo_h, 12, 0x006644AA);
    chaos_gl_draw_text((1024 - 4 * 16) / 2, logo_y + 20, "AIOS", 0x00FFFFFF, 0, 0);
    chaos_gl_draw_text((1024 - 2 * 8) / 2, logo_y + 56, "v2", 0x006644AA, 0, 0);

    // Load generic module icon (gear) — fallback for modules without custom icons
    generic_icon_tex = chaos_gl_texture_load("/system/icons/settings_32.raw");

    // Initialize icon parade X position
    icon_x = 200;

    chaos_gl_surface_present(splash_surface);
}

void boot_splash_status(const char *text) {
    if (splash_surface < 0) return;
    chaos_gl_surface_bind(splash_surface);

    // Clear status area
    chaos_gl_draw_rect(0, 400, 1024, 24, 0x00201830);
    // Draw centered status text
    int tw = chaos_gl_text_width(text);
    chaos_gl_draw_text((1024 - tw) / 2, 404, text, 0x00888899, 0, 0);

    chaos_gl_surface_present(splash_surface);
}

void boot_splash_module(const char *name, int loaded, int total) {
    if (splash_surface < 0) return;
    chaos_gl_surface_bind(splash_surface);

    // Try loading module-specific icon: /system/icons/mod_<name>_32.raw
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/mod_%s_32.raw", name);
    int tex = chaos_gl_texture_load(path);
    if (tex < 0) tex = generic_icon_tex;

    // Draw icon at parade position (vertically centered at y=500)
    int parade_y = 500;
    if (tex >= 0) {
        chaos_gl_blit_keyed(icon_x, parade_y, 32, 32, tex, 0x00FF00FF);
    }
    // Module name below icon (small, dimmed)
    int nw = chaos_gl_text_width(name);
    chaos_gl_draw_text(icon_x + (32 - nw) / 2, parade_y + 36,
                       name, 0x00666677, 0, 0);

    icon_x += 40;  // 32px icon + 8px gap

    // Progress bar
    int bar_x = 200, bar_y = 580, bar_w = 624, bar_h = 8;
    chaos_gl_draw_rect(bar_x, bar_y, bar_w, bar_h, 0x00302848);  // track
    int fill_w = (loaded * bar_w) / (total > 0 ? total : 1);
    chaos_gl_draw_rect(bar_x, bar_y, fill_w, bar_h, 0x006644AA);  // fill

    // Free module-specific icon if it was loaded (not the generic one)
    if (tex >= 0 && tex != generic_icon_tex) {
        chaos_gl_texture_free(tex);
    }

    chaos_gl_surface_present(splash_surface);
}

void boot_splash_destroy(void) {
    if (splash_surface < 0) return;
    if (generic_icon_tex >= 0) chaos_gl_texture_free(generic_icon_tex);
    chaos_gl_surface_destroy(splash_surface);
    splash_surface = -1;
}
```

## Integration Points

**`kernel/main.c`** — add calls at the right moments:

```c
/* Phase 5: ChaosGL */
r = chaos_gl_init();
boot_log("ChaosGL", r >= 0 ? INIT_OK : INIT_FAIL);

/* Boot splash — graphical boot screen starts here */
boot_splash_init();
boot_splash_status("Initializing modules...");

/* Phase 6: KAOS */
r = kaos_init();  // kaos_load_all() calls boot_splash_module() internally
boot_splash_status("Starting Lua runtime...");

/* Phase 7: Lua */
r = lua_init();
boot_splash_status("Loading desktop...");
```

**`kernel/kaos/kaos.c`** — hook into `kaos_load_all()`:

```c
extern void boot_splash_module(const char *name, int loaded, int total);

// Inside the module loading loop:
for (int i = 0; i < module_count; i++) {
    kaos_load_module(modules[i].path);
    boot_splash_module(modules[i].name, i + 1, module_count);
}
```

**`/system/desktop.lua`** — destroy splash when desktop is ready:

The splash surface sits at z=50. When the desktop surface (z=0) and taskbar (z=100) are created and visible, the splash is visually occluded. The desktop shell destroys it explicitly on its first frame:

```lua
-- At the end of desktop initialization, before entering main loop:
chaos_gl.boot_splash_destroy()
```

This requires a single Lua binding in `lua_chaosgl.c` that calls `boot_splash_destroy()`.

## Icon Generation

**`tools/populate_fs.py`** — add to the icon generator:

| Icon | Size | Description |
|------|------|-------------|
| `cursor_24` | 24x24 | Arrow cursor (white arrow, black outline) |
| `mod_hello_32` | 32x32 | Example module icon (speech bubble) |
| `mod_default_32` | 32x32 | Fallback module icon (puzzle piece) |

For the cursor: classic arrow pointer — white filled triangle pointing upper-left, 1px black outline, ~20px tall. This is the only icon that *must* look recognizable.

Module-specific icons follow the naming convention `/system/icons/mod_<name>_32.raw`. Modules without a matching icon file get the generic gear icon as fallback.

## Acceptance Tests

These are verified via serial log output since they involve rendering timing:

1. **Boot splash appears:** after ChaosGL init, the boot splash surface is created and visible. Logo renders. Serial log confirms `boot_splash_init`.
2. **Module parade:** each KAOS module triggers an icon draw. Progress bar advances. Serial log shows module count.
3. **Status updates:** status text changes through "Initializing modules..." -> "Starting Lua runtime..." -> "Loading desktop...".
4. **Splash cleanup:** after desktop shell starts, splash surface is destroyed. No surface leak.

---

# Phase 9 — Window Manager

---

## Founding Premise

The window manager is a single Lua module (`/system/wm.lua`) loaded by the desktop shell at startup. It is not a separate task — it runs within the desktop shell's event loop. The WM owns three things: the window registry (which surfaces exist and their state), input routing (which app gets keyboard and mouse events), and the taskbar (the persistent UI strip at the bottom of the screen).

The WM does not own the compositor — that's a C-level kernel task running `chaos_gl_compose()` at 60fps, fully decoupled. The WM does not own surfaces — each app creates and destroys its own. The WM does not draw window chrome — the Window widget in the UI toolkit does that. The WM is the coordinator that sits between these systems.

**Design rules:**
1. The WM is not privileged. It uses the same Lua APIs as any app. It just happens to run first and manage state that other apps query.
2. Window registration is opt-in. An app calls `wm.register(surface, title)` to participate in WM management. Unregistered surfaces (raw ChaosGL surfaces) still composite but aren't managed — no taskbar entry, no focus management.
3. The WM is a library, not a service. Apps call `wm.*` functions directly. There is no IPC, no message passing, no event bus. Lua global state within the desktop shell task is the shared medium.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                  Desktop Shell Task                          │
│                                                             │
│   wm.lua (window manager)                                   │
│   ├── Window registry (tracked surfaces)                    │
│   ├── Input router (hit-test, focus, dispatch)              │
│   ├── Taskbar renderer (bottom bar, app icons)              │
│   └── Cursor renderer (mouse pointer surface)               │
│                                                             │
│   Desktop surface (z=0, fullscreen, wallpaper + icons)       │
│   Taskbar surface (z=100, bottom strip, always-on-top)       │
│   Cursor surface  (z=999, 16x16, follows mouse)             │
│                                                             │
└──────────────────────────────────────────────────────────────┘
         ↕ wm.register / wm.unregister / wm.focus
┌─────────────────────────────────────────────────────────────┐
│              Application Tasks (each isolated)               │
│   files.lua     settings.lua     game.lua     terminal.lua  │
│   (own surface)  (own surface)   (own surface) (own surface) │
└─────────────────────────────────────────────────────────────┘
```

**Three WM-owned surfaces:**
- **Desktop surface** (z=0): fullscreen 1024×768, draws wallpaper and desktop app icons. This is the "background."
- **Taskbar surface** (z=100): 1024×32 strip at the bottom of the screen. Always on top of normal windows. Shows running apps, clock, system tray area.
- **Cursor surface** (z=999): 16×16 (or 24×24), follows mouse position every frame. Highest z-order so it's always visible. Transparent background via `blit_keyed`.

---

## Window Registry

### Data Structure

```lua
-- /system/wm.lua

-- Tracked windows. Key = surface handle (integer).
local windows = {}

-- Active window = the one receiving keyboard input
local active_window = nil

-- Next z-order to assign (increments on bring_to_front)
local next_z = 1

-- Structure of each entry in `windows`:
-- windows[surface_handle] = {
--     surface   = handle,      -- ChaosGL surface handle
--     title     = "App Name",  -- shown in taskbar
--     icon      = tex_handle,  -- 24x24 icon for taskbar (or nil)
--     task_id   = tid,         -- scheduler task id (for kill)
--     z         = number,      -- current z-order assignment
--     minimized = false,       -- surface hidden but still registered
--     modal     = false,       -- blocks input to windows below
-- }
```

### Registration API

```lua
-- Register a surface with the WM. Called by apps after surface creation.
-- Returns true on success, false if surface already registered or invalid.
function wm.register(surface, opts)
    -- opts.title    (string, required)
    -- opts.icon     (texture handle, optional — 24x24 for taskbar)
    -- opts.task_id  (integer, optional — for task management)
    -- opts.modal    (boolean, optional — default false)

    if windows[surface] then return false end

    next_z = next_z + 1
    chaos_gl.surface_set_zorder(surface, next_z)

    windows[surface] = {
        surface   = surface,
        title     = opts.title or "Untitled",
        icon      = opts.icon or nil,
        task_id   = opts.task_id or nil,
        z         = next_z,
        minimized = false,
        modal     = opts.modal or false,
    }

    wm.focus(surface)
    return true
end

-- Unregister a surface. Called by apps before surface destruction.
-- Also called automatically if the WM detects the surface was destroyed.
function wm.unregister(surface)
    if not windows[surface] then return end

    -- If this was the active window, focus the next highest
    if active_window == surface then
        active_window = nil
        wm._focus_next_highest()
    end

    windows[surface] = nil
end
```

### Focus Management

```lua
-- Bring a window to the front and make it the active (keyboard) target.
function wm.focus(surface)
    if not windows[surface] then return end
    if windows[surface].minimized then
        wm.restore(surface)
    end

    -- Reassign z-order: this window gets the highest z in the 1-99 range.
    -- Other windows keep their relative order.
    next_z = next_z + 1
    if next_z > 90 then
        -- Compact z-orders to prevent overflow
        wm._compact_zorders()
    end

    windows[surface].z = next_z
    chaos_gl.surface_set_zorder(surface, next_z)

    -- Update active window
    if active_window and active_window ~= surface then
        -- Notify old active window it lost focus (optional callback)
        local old = windows[active_window]
        if old and old.on_blur then old.on_blur() end
    end

    active_window = surface
end

-- Internal: compact z-orders when they get too high.
-- Sorts windows by current z, reassigns 1..N.
function wm._compact_zorders()
    local sorted = {}
    for _, w in pairs(windows) do
        if not w.minimized then
            sorted[#sorted + 1] = w
        end
    end
    table.sort(sorted, function(a, b) return a.z < b.z end)

    for i, w in ipairs(sorted) do
        w.z = i
        chaos_gl.surface_set_zorder(w.surface, i)
    end
    next_z = #sorted
end

-- Internal: focus the topmost non-minimized window.
function wm._focus_next_highest()
    local best = nil
    local best_z = -1
    for _, w in pairs(windows) do
        if not w.minimized and w.z > best_z then
            best = w.surface
            best_z = w.z
        end
    end
    if best then wm.focus(best) end
end
```

### Minimize / Restore

```lua
-- Minimize: hide the surface, keep it registered.
function wm.minimize(surface)
    local w = windows[surface]
    if not w then return end

    w.minimized = true
    chaos_gl.surface_set_visible(surface, false)

    if active_window == surface then
        active_window = nil
        wm._focus_next_highest()
    end
end

-- Restore: show a minimized surface and focus it.
function wm.restore(surface)
    local w = windows[surface]
    if not w then return end

    w.minimized = false
    chaos_gl.surface_set_visible(surface, true)
    wm.focus(surface)
end

-- Toggle: if visible, minimize; if minimized, restore.
function wm.toggle(surface)
    local w = windows[surface]
    if not w then return end
    if w.minimized then wm.restore(surface) else wm.minimize(surface) end
end
```

### Query API

```lua
-- Get list of all registered windows (for taskbar rendering).
function wm.get_windows()
    local list = {}
    for _, w in pairs(windows) do
        list[#list + 1] = {
            surface   = w.surface,
            title     = w.title,
            icon      = w.icon,
            task_id   = w.task_id,
            minimized = w.minimized,
            active    = (w.surface == active_window),
        }
    end
    -- Sort by registration order (z-order as proxy)
    table.sort(list, function(a, b) return a.surface < b.surface end)
    return list
end

-- Get the active window's surface handle (or nil).
function wm.get_active()
    return active_window
end

-- Check if a surface is registered.
function wm.is_registered(surface)
    return windows[surface] ~= nil
end
```

---

## Input Routing

The desktop shell task owns the input loop. Raw events from `aios.input.poll()` flow through the WM's input router, which decides where they go.

### Mouse Routing

```lua
-- Called by the desktop shell's event loop for every mouse event.
-- Returns the surface handle that should receive this event, or nil for desktop.
function wm.route_mouse(event)
    -- Update cursor position (always, regardless of routing)
    wm._update_cursor(event.mouse_x, event.mouse_y)

    -- Check modal windows first: if a modal is active, only it gets input
    for _, w in pairs(windows) do
        if w.modal and not w.minimized then
            return w.surface
        end
    end

    -- Hit-test: iterate visible surfaces in reverse z-order (front to back).
    -- First surface whose screen-space rect contains the mouse position wins.
    local hit = nil
    local hit_z = -1

    -- Check taskbar first (z=100)
    if event.mouse_y >= (768 - 32) then
        return "taskbar"  -- special sentinel: desktop handles taskbar clicks
    end

    for _, w in pairs(windows) do
        if not w.minimized and w.z > hit_z then
            local sx, sy = chaos_gl.surface_get_position(w.surface)
            local sw, sh = chaos_gl.surface_get_size(w.surface)
            if event.mouse_x >= sx and event.mouse_x < sx + sw and
               event.mouse_y >= sy and event.mouse_y < sy + sh then
                hit = w.surface
                hit_z = w.z
            end
        end
    end

    -- Click on a window: focus it (bring to front)
    if hit and event.type == EVENT_MOUSE_DOWN then
        wm.focus(hit)
    end

    return hit  -- nil = click landed on desktop background
end
```

### Coordinate Translation

When the WM routes a mouse event to an app, the coordinates must be translated from screen-space to surface-local space:

```lua
-- Translate screen-space mouse coordinates to surface-local.
function wm.translate_event(event, surface)
    local sx, sy = chaos_gl.surface_get_position(surface)
    local translated = {}
    for k, v in pairs(event) do translated[k] = v end
    translated.mouse_x = event.mouse_x - sx
    translated.mouse_y = event.mouse_y - sy
    return translated
end
```

### Keyboard Routing

Keyboard events always go to the active window. No hit-testing needed.

```lua
-- Called by the desktop shell's event loop for every keyboard event.
-- Returns the surface handle that should receive this event, or nil.
function wm.route_keyboard(event)
    -- System-wide shortcuts (handled before app dispatch)
    if event.type == EVENT_KEY_DOWN then
        -- Alt+F4: close active window
        if event.alt and event.key == KEY_F4 then
            if active_window then
                wm._request_close(active_window)
            end
            return nil  -- consumed by WM
        end
        -- Alt+Tab: cycle windows
        if event.alt and event.key == KEY_TAB then
            wm._cycle_focus(event.shift)
            return nil  -- consumed by WM
        end
    end

    return active_window
end

-- Alt+Tab: cycle through registered windows.
function wm._cycle_focus(reverse)
    local sorted = {}
    for _, w in pairs(windows) do
        if not w.minimized then
            sorted[#sorted + 1] = w
        end
    end
    if #sorted <= 1 then return end

    table.sort(sorted, function(a, b) return a.z < b.z end)

    -- Find current active in sorted list, move to next/prev
    local idx = 1
    for i, w in ipairs(sorted) do
        if w.surface == active_window then idx = i; break end
    end

    if reverse then
        idx = ((idx - 2) % #sorted) + 1
    else
        idx = (idx % #sorted) + 1
    end

    wm.focus(sorted[idx].surface)
end
```

### Input Dispatch — Event Queues

Apps run in their own scheduler tasks. The WM runs in the desktop shell task. To deliver events from the WM to app tasks, we use per-surface event queues stored in the WM:

```lua
-- Per-surface event queues. Apps poll these from their own task.
local event_queues = {}  -- event_queues[surface_handle] = { event1, event2, ... }

-- Push an event to a surface's queue.
function wm._push_event(surface, event)
    if not event_queues[surface] then
        event_queues[surface] = {}
    end
    local q = event_queues[surface]
    if #q < 256 then  -- overflow protection
        q[#q + 1] = event
    end
end

-- Apps call this instead of aios.input.poll() to receive WM-routed events.
-- This is the WM-aware input API.
function wm.poll_event(surface)
    local q = event_queues[surface]
    if not q or #q == 0 then return nil end
    return table.remove(q, 1)
end
```

**How apps use this:** the standard app lifecycle pattern changes slightly. Instead of calling `aios.input.poll()` directly (which reads raw hardware events), managed apps call `wm.poll_event(my_surface)` to receive WM-routed, coordinate-translated events. The UI toolkit's `core.lua` wraps this transparently — apps that use the toolkit don't need to know about the distinction.

---

## Cursor

The WM draws a hardware-style mouse cursor as a tiny surface at the highest z-order.

```lua
-- Cursor state
local cursor_surface = nil
local cursor_tex = nil       -- 16x16 or 24x24 cursor texture
local cursor_visible = true

function wm._init_cursor()
    cursor_surface = chaos_gl.surface_create(24, 24, false)
    chaos_gl.surface_set_zorder(cursor_surface, 999)
    chaos_gl.surface_set_visible(cursor_surface, true)

    -- Load cursor texture from filesystem
    cursor_tex = chaos_gl.load_texture("/system/icons/cursor_24.raw")

    -- Draw cursor onto its surface (once — only redraw on cursor shape change)
    chaos_gl.surface_bind(cursor_surface)
    chaos_gl.surface_clear(cursor_surface, 0x00FF00FF)  -- magenta = transparent
    chaos_gl.blit_keyed(0, 0, 24, 24, cursor_tex, 0x00FF00FF)
    chaos_gl.surface_present(cursor_surface)
end

function wm._update_cursor(mx, my)
    -- Clamp to screen bounds
    mx = math.max(0, math.min(1023, mx))
    my = math.max(0, math.min(767, my))
    chaos_gl.surface_set_position(cursor_surface, mx, my)
end
```

**Cursor shape changes:** for resize handles, the WM can swap the cursor texture (arrow → resize → text beam). The cursor surface is redrawn when the shape changes — not every frame.

---

## Taskbar

The taskbar is a 1024×32 surface at z=100. It is drawn by the desktop shell task as part of the WM.

### Layout

```
┌──────────────────────────────────────────────────────────────────┐
│ [≡]  [Files] [Settings] [Terminal]              12:34  ▲ ■ ●    │
│ menu  ──── running apps ────────              clock  tray icons  │
└──────────────────────────────────────────────────────────────────┘
  24px   variable width (per app)                     right-aligned
```

### Rendering

```lua
local taskbar_surface = nil

function wm._init_taskbar()
    taskbar_surface = chaos_gl.surface_create(1024, 32, false)
    chaos_gl.surface_set_position(taskbar_surface, 0, 768 - 32)
    chaos_gl.surface_set_zorder(taskbar_surface, 100)
    chaos_gl.surface_set_visible(taskbar_surface, true)
end

function wm._draw_taskbar()
    chaos_gl.surface_bind(taskbar_surface)
    chaos_gl.surface_clear(taskbar_surface, theme.taskbar_bg)

    -- Menu button (leftmost)
    local menu_hover = wm._taskbar_hover == "menu"
    local menu_bg = menu_hover and theme.button_hover or theme.taskbar_bg
    chaos_gl.rect(0, 0, 32, 32, menu_bg)
    chaos_gl.text(8, 8, "≡", theme.text_primary, 0, 0)

    -- App buttons
    local x = 36
    local wins = wm.get_windows()
    for _, w in ipairs(wins) do
        local btn_w = math.min(160, chaos_gl.text_width(w.title) + 32)
        local bg = w.active and theme.accent
                 or (wm._taskbar_hover == w.surface and theme.button_hover
                 or theme.taskbar_bg)

        chaos_gl.rect(x, 0, btn_w, 32, bg)

        -- Icon (24x24, vertically centered)
        if w.icon then
            chaos_gl.blit_keyed(x + 4, 4, 24, 24, w.icon, 0x00FF00FF)
        end

        -- Title (truncated to fit)
        local title_x = w.icon and (x + 32) or (x + 8)
        local max_title_w = btn_w - (w.icon and 36 or 16)
        local title = w.title
        while chaos_gl.text_width(title) > max_title_w and #title > 1 do
            title = title:sub(1, -2)
        end
        chaos_gl.text(title_x, 8, title, theme.text_primary, 0, 0)

        -- Active indicator (underline)
        if w.active then
            chaos_gl.rect(x, 28, btn_w, 3, theme.app_active_indicator)
        end

        -- Minimized indicator (dimmed text)
        if w.minimized then
            chaos_gl.rect(x, 0, btn_w, 32,
                          CHAOS_GL_RGB(0, 0, 0))  -- darken overlay
        end

        x = x + btn_w + 2
    end

    -- Clock (right-aligned)
    local uptime = aios.os.millis()
    local secs = math.floor(uptime / 1000)
    local mins = math.floor(secs / 60) % 60
    local hrs  = math.floor(secs / 3600) % 24
    local clock_str = string.format("%02d:%02d", hrs, mins)
    local clock_w = chaos_gl.text_width(clock_str)
    chaos_gl.text(1024 - clock_w - 8, 8, clock_str, theme.text_secondary, 0, 0)

    chaos_gl.surface_present(taskbar_surface)
end
```

### Taskbar Input

```lua
function wm._handle_taskbar_click(event)
    local x = event.mouse_x

    -- Menu button (0-32)
    if x < 32 then
        wm._toggle_app_menu()
        return
    end

    -- App buttons: find which button was clicked
    local bx = 36
    local wins = wm.get_windows()
    for _, w in ipairs(wins) do
        local btn_w = math.min(160, chaos_gl.text_width(w.title) + 32)
        if x >= bx and x < bx + btn_w then
            wm.toggle(w.surface)
            return
        end
        bx = bx + btn_w + 2
    end
end
```

---

## Desktop Shell Integration

The desktop shell (`/system/desktop.lua`, specified in Phase 10) is the first Lua task launched. It initialises the WM, creates the desktop and taskbar surfaces, and runs the main event loop that pumps the WM's input router:

```lua
-- Skeleton of the desktop shell's main loop (full spec in Phase 10)

-- Initialize WM
local wm = require "/system/wm"
wm.init()

-- Main event loop
while true do
    -- Draw desktop (wallpaper, icons)
    wm._draw_desktop()

    -- Draw taskbar
    wm._draw_taskbar()

    -- Draw cursor
    -- (cursor surface is repositioned in wm._update_cursor,
    --  which is called by wm.route_mouse)

    -- Process all pending input events
    local event = aios.input.poll()
    while event do
        if event.type >= EVENT_KEY_DOWN then
            -- Keyboard
            local target = wm.route_keyboard(event)
            if target then
                wm._push_event(target, event)
            end
        else
            -- Mouse
            local target = wm.route_mouse(event)
            if target == "taskbar" then
                if event.type == EVENT_MOUSE_DOWN then
                    wm._handle_taskbar_click(event)
                end
            elseif target then
                local translated = wm.translate_event(event, target)
                wm._push_event(target, translated)
            else
                -- Click on desktop background
                wm._handle_desktop_click(event)
            end
        end
        event = aios.input.poll()
    end

    aios.os.sleep(16)
end
```

---

## App Menu

The app menu is a popup launched from the taskbar's menu button. It lists installed applications with icons and names. Clicking an entry spawns the app.

```lua
local app_menu_visible = false
local app_menu_surface = nil

-- App registry: scanned from /apps/ at boot.
local installed_apps = {}
-- Each: { name, path, icon_path }

function wm._scan_apps()
    installed_apps = {}
    local entries = aios.io.listdir("/apps")
    for _, e in ipairs(entries) do
        if e.type == "file" and e.name:match("%.lua$") then
            -- Read first line for app metadata comment
            -- Format: -- @app name="File Browser" icon="/system/icons/files_32.raw"
            local meta = wm._parse_app_meta("/apps/" .. e.name)
            installed_apps[#installed_apps + 1] = {
                name = meta.name or e.name:gsub("%.lua$", ""),
                path = "/apps/" .. e.name,
                icon_path = meta.icon or nil,
            }
        end
    end
    table.sort(installed_apps, function(a, b) return a.name < b.name end)
end

function wm._toggle_app_menu()
    if app_menu_visible then
        wm._close_app_menu()
    else
        wm._open_app_menu()
    end
end

function wm._open_app_menu()
    local item_h = 32
    local menu_w = 200
    local menu_h = #installed_apps * item_h + 8
    local menu_y = 768 - 32 - menu_h

    app_menu_surface = chaos_gl.surface_create(menu_w, menu_h, false)
    chaos_gl.surface_set_position(app_menu_surface, 0, menu_y)
    chaos_gl.surface_set_zorder(app_menu_surface, 101)  -- above taskbar
    chaos_gl.surface_set_visible(app_menu_surface, true)

    -- Draw menu items
    chaos_gl.surface_bind(app_menu_surface)
    chaos_gl.surface_clear(app_menu_surface, theme.menu_bg)

    local y = 4
    for _, app in ipairs(installed_apps) do
        chaos_gl.text(40, y + 8, app.name, theme.text_primary, 0, 0)
        y = y + item_h
    end
    chaos_gl.surface_present(app_menu_surface)

    app_menu_visible = true
end

function wm._close_app_menu()
    if app_menu_surface then
        chaos_gl.surface_destroy(app_menu_surface)
        app_menu_surface = nil
    end
    app_menu_visible = false
end
```

---

## Window Constraints

### Screen Bounds

Windows cannot be dragged fully off-screen. The WM enforces that at least 32px of the titlebar remains visible:

```lua
function wm.clamp_position(surface, x, y)
    local sw, sh = chaos_gl.surface_get_size(surface)
    local min_visible = 32
    x = math.max(-sw + min_visible, math.min(1024 - min_visible, x))
    y = math.max(0, math.min(768 - 32 - min_visible, y))  -- 32 = taskbar height
    return x, y
end
```

### Maximise

Double-clicking the titlebar maximises the window to fill the screen minus the taskbar:

```lua
function wm.maximize(surface)
    local w = windows[surface]
    if not w then return end

    -- Store original geometry for restore
    local sx, sy = chaos_gl.surface_get_position(surface)
    local sw, sh = chaos_gl.surface_get_size(surface)
    w._restore_rect = { x = sx, y = sy, w = sw, h = sh }

    chaos_gl.surface_set_position(surface, 0, 0)
    chaos_gl.surface_resize(surface, 1024, 768 - 32)  -- full screen minus taskbar
    wm.focus(surface)
end

function wm.restore_size(surface)
    local w = windows[surface]
    if not w or not w._restore_rect then return end
    local r = w._restore_rect
    chaos_gl.surface_set_position(surface, r.x, r.y)
    chaos_gl.surface_resize(surface, r.w, r.h)
    w._restore_rect = nil
end
```

---

## Phase 9 Acceptance Tests

### Window Management

1. **Register/unregister:** register surface, verify `wm.get_windows()` includes it. Unregister, verify removed.
2. **Focus:** register 3 windows. Focus window C. Verify C has highest z-order. Verify `wm.get_active() == C`.
3. **Bring to front:** click on background window. Verify it moves to front (z-order updated). Previous front window loses active status.
4. **Z-order compaction:** register and focus 95 windows. Z-orders stay in 1–99 range. No overflow.
5. **Minimize:** minimize active window. Surface becomes invisible. Taskbar still shows it (dimmed). Focus moves to next window.
6. **Restore:** restore minimized window. Surface visible again. Window receives focus.
7. **Toggle:** click minimized window in taskbar → restores. Click active window in taskbar → minimizes.

### Input Routing

8. **Mouse hit-test:** two overlapping windows. Click on the top window — event goes to top window only. Click on exposed area of bottom window — event goes to bottom window.
9. **Coordinate translation:** click at screen position (200, 150) on a window at position (100, 100). App receives event with mouse_x=100, mouse_y=50.
10. **Keyboard to active:** type characters. Only the active window's event queue receives them.
11. **Alt+Tab:** 3 windows. Alt+Tab cycles focus A→B→C→A. Alt+Shift+Tab reverses.
12. **Alt+F4:** active window receives close request. No crash if window handles it.
13. **Modal window:** register modal window. All mouse clicks route to modal only. Keyboard goes to modal.
14. **Click-to-focus:** click on inactive window. Window gains focus AND receives the click event.
15. **Desktop click:** click on empty desktop area (no window). Returns nil — desktop handles it.

### Taskbar

16. **App buttons:** all registered windows show in taskbar with correct titles.
17. **Active indicator:** active window's taskbar button has underline.
18. **Taskbar click:** click app button → toggles window. Click menu button → opens app menu.
19. **Clock:** clock displays and updates. Format is HH:MM.
20. **Taskbar always on top:** open window at bottom of screen. Taskbar draws over it.

### Cursor

21. **Cursor follows mouse:** move mouse. Cursor surface position updates every frame.
22. **Cursor on top:** cursor visible over all windows including taskbar.
23. **Screen bounds:** move mouse to corners. Cursor doesn't leave 0..1023, 0..767.

### Integration

24. **Full lifecycle:** spawn app, register window, drag it, type into it, minimize, restore, close. No leak, no crash.
25. **Multi-window stress:** 8 apps registered. Alt+Tab through all. Minimize 4, restore 2. All state correct. Taskbar shows all 8.

---

# Phase 10 — Desktop & Core Applications

---

## Founding Premise

Phase 10 delivers the applications that make AIOS usable. Each app is a standalone Lua script (100–300 lines) following the UI toolkit's app lifecycle pattern: create surface, register with WM, build widget tree, run event loop, cleanup on exit. The apps are:

1. **Desktop Shell** (`/system/desktop.lua`) — the first task, hosts the WM, draws wallpaper and desktop icons.
2. **File Browser** (`/apps/files.lua`) — navigate ChaosFS, open files, create/delete/rename.
3. **Settings** (`/apps/settings.lua`) — theme switching, system info, display settings.
4. **Terminal** (`/apps/terminal.lua`) — Lua REPL with command history, runs Lua expressions live.
5. **Text Editor** (`/apps/edit.lua`) — view and edit text files on ChaosFS.

These five apps are the minimum viable desktop. They exercise every widget in the UI toolkit, every WM feature, and every `aios.*` API. If they all work, the platform is proven.

---

## App Metadata Convention

Every app has a metadata comment on its first line. The WM's app scanner reads this to populate the app menu:

```lua
-- @app name="File Browser" icon="/system/icons/files_32.raw"
```

Fields: `name` (display name), `icon` (path to 32×32 `.raw` icon for app menu and desktop). Both optional — defaults to filename and generic icon.

---

## 1. Desktop Shell (`/system/desktop.lua`)

The desktop shell is the first Lua task spawned by `kernel_main`. It is the host for the WM and the persistent background.

### Responsibilities

- Initialise the WM (`wm.init()`)
- Create the desktop surface (z=0, fullscreen)
- Draw wallpaper (solid colour or `.raw` texture from `/system/wallpaper.raw`)
- Draw desktop app icons (shortcut grid from `/system/desktop_icons.lua`)
- Run the main event loop (pumps raw input → WM router → app event queues)
- Run the compositor indirectly (the compositor is a C task, not Lua — it runs independently)

### Desktop Icons

Desktop icons are `AppIcon` widgets from the UI toolkit, laid out in a grid starting from the top-left corner. Double-clicking an icon launches the app.

```lua
-- /system/desktop_icons.lua — scanned at boot
return {
    { name = "Files",    app = "/apps/files.lua",    icon = "/system/icons/files_48.raw" },
    { name = "Terminal", app = "/apps/terminal.lua",  icon = "/system/icons/shell_48.raw" },
    { name = "Settings", app = "/apps/settings.lua",  icon = "/system/icons/settings_48.raw" },
    { name = "Editor",   app = "/apps/edit.lua",      icon = "/system/icons/text_48.raw" },
}
```

### Implementation Sketch

```lua
-- /system/desktop.lua
-- @app name="Desktop" icon="/system/icons/desktop_48.raw"

local wm = require "/system/wm"
local ui = require "/system/ui/core"
local AppIcon = require "/system/ui/appicon"

-- Initialize window manager (creates taskbar, cursor)
wm.init()

-- Create desktop surface (fullscreen, behind everything)
local desktop = chaos_gl.surface_create(1024, 768, false)
chaos_gl.surface_set_position(desktop, 0, 0)
chaos_gl.surface_set_zorder(desktop, 0)
chaos_gl.surface_set_visible(desktop, true)

-- Load wallpaper (or use solid colour)
local wallpaper_color = theme.desktop_bg or CHAOS_GL_RGB(40, 40, 60)
local wallpaper_tex = nil
if aios.io.exists("/system/wallpaper.raw") then
    wallpaper_tex = chaos_gl.load_texture("/system/wallpaper.raw")
end

-- Build desktop icon grid
local icons_config = dofile("/system/desktop_icons.lua")
local desktop_icons = {}
local grid_x, grid_y = 24, 24
for _, cfg in ipairs(icons_config) do
    local icon = AppIcon.new(cfg.name, {
        icon = chaos_gl.load_texture(cfg.icon),
        on_launch = function()
            aios.task.spawn(cfg.app)
        end,
    })
    desktop_icons[#desktop_icons + 1] = { widget = icon, x = grid_x, y = grid_y }
    grid_y = grid_y + theme.app_cell_size
    if grid_y > 768 - 32 - theme.app_cell_size then
        grid_y = 24
        grid_x = grid_x + theme.app_cell_size
    end
end

-- Main loop
while true do
    -- Draw desktop
    chaos_gl.surface_bind(desktop)
    if wallpaper_tex then
        chaos_gl.blit(0, 0, 1024, 768, wallpaper_tex)
    else
        chaos_gl.surface_clear(desktop, wallpaper_color)
    end

    for _, di in ipairs(desktop_icons) do
        di.widget:draw(di.x, di.y)
    end
    chaos_gl.surface_present(desktop)

    -- Draw taskbar
    wm._draw_taskbar()

    -- Process input
    local event = aios.input.poll()
    while event do
        if event.type >= EVENT_KEY_DOWN then
            local target = wm.route_keyboard(event)
            if target then
                wm._push_event(target, event)
            end
        else
            local target = wm.route_mouse(event)
            if target == "taskbar" then
                if event.type == EVENT_MOUSE_DOWN then
                    wm._handle_taskbar_click(event)
                end
            elseif target then
                local translated = wm.translate_event(event, target)
                wm._push_event(target, translated)
            else
                -- Desktop: check icon clicks
                for _, di in ipairs(desktop_icons) do
                    if di.widget:contains(event.mouse_x, event.mouse_y) then
                        di.widget:on_input(event)
                    end
                end
            end
        end
        event = aios.input.poll()
    end

    aios.os.sleep(16)
end
```

---

## 2. File Browser (`/apps/files.lua`)

### Features

- Navigate ChaosFS directory tree
- Grid view (32×32 icons) and list view (16×16 icons + filename)
- Double-click folder to navigate into it
- Double-click file to open in text editor (or appropriate viewer)
- Toolbar: Back, Up, New Folder, Delete, View toggle
- Path bar showing current directory
- File type icons via icon registry (auto-detected from extension)

### Implementation Sketch

```lua
-- /apps/files.lua
-- @app name="File Browser" icon="/system/icons/files_32.raw"

local wm = require "/system/wm"
local ui = require "/system/ui/core"
local Window = require "/system/ui/window"
local Button = require "/system/ui/button"
local Label = require "/system/ui/label"
local ScrollView = require "/system/ui/scrollview"
local FileItem = require "/system/ui/fileitem"
local flex = require "/system/layout/flex"

-- State
local current_path = "/"
local history = { "/" }
local history_idx = 1
local view_mode = "grid"  -- "grid" or "list"
local selected = nil

-- Create window
local surface = chaos_gl.surface_create(640, 480, false)
chaos_gl.surface_set_position(surface, 100, 80)
chaos_gl.surface_set_visible(surface, true)

local task_id = aios.task.self().id
wm.register(surface, {
    title = "File Browser",
    icon = chaos_gl.load_texture("/system/icons/files_32.raw"),
    task_id = task_id,
})

-- Navigation functions
local function navigate(path)
    current_path = path
    history_idx = history_idx + 1
    history[history_idx] = path
    -- Trim forward history
    for i = history_idx + 1, #history do history[i] = nil end
    selected = nil
end

local function go_back()
    if history_idx > 1 then
        history_idx = history_idx - 1
        current_path = history[history_idx]
        selected = nil
    end
end

local function go_up()
    local parent = current_path:match("^(.*)/[^/]+/?$") or "/"
    navigate(parent)
end

local function open_item(name, type)
    if type == "dir" then
        local new_path = current_path
        if new_path ~= "/" then new_path = new_path .. "/" end
        navigate(new_path .. name)
    else
        -- Open file in text editor
        local file_path = current_path
        if file_path ~= "/" then file_path = file_path .. "/" end
        aios.task.spawn("/apps/edit.lua", { file = file_path .. name })
    end
end

-- Build UI
local running = true

local win = Window.new("File Browser", {
    closable = true,
    resizable = true,
    on_close = function() running = false end,
})

-- Main loop
while running do
    chaos_gl.surface_bind(surface)
    chaos_gl.surface_clear(surface, theme.window_bg)

    -- Draw window chrome
    win:draw(0, 0)

    -- Toolbar (inside window body, below titlebar)
    local toolbar_y = theme.titlebar_height + 4
    -- [Back] [Up] [New Folder] [Delete] [Grid/List]
    -- (simplified — real impl uses Button widgets in a flex row)
    chaos_gl.text(8, toolbar_y, "← Back  ↑ Up  📁 New  🗑 Del  ☰ "
                  .. view_mode, theme.text_secondary, 0, 0)

    -- Path bar
    chaos_gl.text(8, toolbar_y + 24, current_path, theme.text_primary, 0, 0)

    -- File listing
    local entries = aios.io.listdir(current_path)
    local content_y = toolbar_y + 48
    local x, y = 8, content_y

    for _, entry in ipairs(entries) do
        if entry.name ~= "." and entry.name ~= ".." then
            local is_selected = (selected == entry.name)
            if is_selected then
                chaos_gl.rect(x - 2, y - 2,
                    view_mode == "grid" and 72 or 300, 
                    view_mode == "grid" and 72 or 20,
                    theme.file_selection)
            end

            chaos_gl.text(x, y, entry.name,
                entry.type == "dir" and theme.accent or theme.text_primary, 0, 0)

            if view_mode == "grid" then
                y = y + theme.file_grid_cell
                if y > 480 - 40 then y = content_y; x = x + theme.file_grid_cell end
            else
                y = y + 20
            end
        end
    end

    chaos_gl.surface_present(surface)

    -- Event handling
    local event = wm.poll_event(surface)
    while event do
        win:on_input(event)
        -- Handle file clicks, double-clicks, toolbar buttons...
        event = wm.poll_event(surface)
    end

    aios.os.sleep(16)
end

-- Cleanup
wm.unregister(surface)
chaos_gl.surface_destroy(surface)
```

---

## 3. Settings (`/apps/settings.lua`)

### Features

- Theme switching (dark / light) — instant via `ui.load_theme()`
- System information display (memory usage, CPU usage, filesystem stats, uptime)
- Display info (resolution, framebuffer address)
- KAOS module list (loaded modules, names, versions)

### Layout

```
┌─ Settings ──────────────────────────────────┐
│                                             │
│  [Appearance] [System] [Modules]  ← tabs    │
│                                             │
│  ┌─ Theme ─────────────────────┐            │
│  │  (●) Dark    ( ) Light      │            │
│  └─────────────────────────────┘            │
│                                             │
│  ┌─ System Info ───────────────┐            │
│  │  Heap: 2.1 MB / 8.0 MB     │            │
│  │  PMM:  28400 / 32768 pages  │            │
│  │  CPU:  12%                  │            │
│  │  Uptime: 0h 14m 32s        │            │
│  │  FS: 1240 / 8192 blocks    │            │
│  └─────────────────────────────┘            │
│                                             │
└─────────────────────────────────────────────┘
```

**Estimated size:** ~150 lines of Lua. TabView with 3 tabs, Label widgets for display, Checkbox or radio-style Buttons for theme selection, a timer that refreshes system stats every second.

---

## 4. Terminal (`/apps/terminal.lua`)

### Features

- Lua REPL: type Lua expressions, see results immediately
- Command history (up/down arrows, stored in memory)
- Scrollback buffer (last 500 lines)
- `clear` command to reset display
- `ls`, `cat`, `mkdir`, `rm` convenience commands (mapped to `aios.io.*`)
- Output displayed in Claude Mono 8×16 (ChaosGL text)
- Coloured prompt and error messages

### Architecture

The terminal is a single-surface app with a TextField at the bottom for input and a ScrollView-style region above for output. It does not spawn sub-processes — there is no shell or process model. Typed lines are evaluated as Lua expressions via `load()`:

```lua
local function eval_line(line)
    -- Try as expression first (prepend "return")
    local fn, err = load("return " .. line)
    if not fn then
        -- Try as statement
        fn, err = load(line)
    end
    if fn then
        local ok, result = pcall(fn)
        if ok then
            if result ~= nil then
                append_output(tostring(result), theme.text_primary)
            end
        else
            append_output("Error: " .. tostring(result), theme.text_danger)
        end
    else
        append_output("Syntax: " .. err, theme.text_danger)
    end
end
```

### Convenience Commands

```lua
local commands = {
    clear = function() output_lines = {} end,
    ls = function(args)
        local path = args[1] or current_dir
        local entries = aios.io.listdir(path)
        for _, e in ipairs(entries) do
            local prefix = e.type == "dir" and "[DIR] " or "      "
            append_output(prefix .. e.name, theme.text_primary)
        end
    end,
    cat = function(args)
        if not args[1] then append_output("Usage: cat <path>", theme.text_warning); return end
        local text = aios.io.readfile(args[1])
        if text then append_output(text, theme.text_primary)
        else append_output("File not found: " .. args[1], theme.text_danger) end
    end,
    cd = function(args)
        if not args[1] then current_dir = "/"; return end
        if aios.io.stat(args[1]) then current_dir = args[1]
        else append_output("Not a directory: " .. args[1], theme.text_danger) end
    end,
    mkdir = function(args)
        if args[1] then aios.io.mkdir(args[1]) end
    end,
    rm = function(args)
        if args[1] then aios.io.unlink(args[1]) end
    end,
    mem = function()
        local m = aios.os.meminfo()
        append_output(string.format("Heap: %d KB used / %d KB free",
            m.heap_used / 1024, m.heap_free / 1024), theme.text_primary)
        append_output(string.format("PMM:  %d / %d pages free",
            m.pmm_free_pages, m.pmm_total_pages), theme.text_primary)
    end,
}

local function process_input(line)
    append_output("> " .. line, theme.accent)
    -- Check convenience commands first
    local cmd, rest = line:match("^(%S+)%s*(.*)")
    if cmd and commands[cmd] then
        local args = {}
        for arg in rest:gmatch("%S+") do args[#args + 1] = arg end
        commands[cmd](args)
    else
        eval_line(line)
    end
end
```

**Estimated size:** ~200 lines of Lua.

---

## 5. Text Editor (`/apps/edit.lua`)

### Features

- Open and edit text files from ChaosFS
- TextArea widget for the editing surface (from UI toolkit)
- Save with Ctrl+S
- Line numbers in left gutter
- Status bar: filename, line count, modified indicator
- Syntax-agnostic (no highlighting in v1 — just monospace text editing)

### Launch

The text editor accepts a file path via the `arg` table (set by `aios.task.spawn`):

```lua
-- @app name="Text Editor" icon="/system/icons/text_32.raw"

local file_path = arg and arg.file or nil
local content = ""
local modified = false

if file_path then
    content = aios.io.readfile(file_path) or ""
end
```

### Save

```lua
local function save()
    if not file_path then
        -- TODO: save-as dialog
        return
    end
    aios.io.writefile(file_path, editor:get_text())
    modified = false
end
```

**Estimated size:** ~180 lines of Lua.

---

## Filesystem Layout (Phases 9–10)

```
/system/
├── desktop.lua              -- Desktop shell (Phase 10, ~200 lines)
├── wm.lua                   -- Window manager (Phase 9, ~400 lines)
├── desktop_icons.lua        -- Desktop shortcut configuration
├── wallpaper.raw            -- Desktop wallpaper (1024x768 BGRX, optional)
├── ui/                      -- UI toolkit (Phase 8, already specified)
├── icons/
│   ├── cursor_24.raw        -- Mouse cursor
│   ├── files_32.raw         -- File browser icon (taskbar)
│   ├── files_48.raw         -- File browser icon (desktop)
│   ├── shell_32.raw         -- Terminal icon (taskbar)
│   ├── shell_48.raw         -- Terminal icon (desktop)
│   ├── settings_32.raw      -- Settings icon (taskbar)
│   ├── settings_48.raw      -- Settings icon (desktop)
│   ├── text_32.raw          -- Text editor icon
│   ├── text_48.raw          -- Text editor icon (desktop)
│   └── ... (file type icons from UI toolkit spec)
├── layout/                  -- Layout engine (Phase 8)
└── themes/                  -- Dark/light themes (Phase 8)

/apps/
├── files.lua                -- File browser (~250 lines)
├── settings.lua             -- Settings panel (~150 lines)
├── terminal.lua             -- Lua terminal (~200 lines)
└── edit.lua                 -- Text editor (~180 lines)
```

**Total estimated new code:** ~400 lines (wm.lua) + ~200 lines (desktop.lua) + ~780 lines (4 apps) ≈ **1,380 lines of Lua**.

---

## Boot Sequence — From Kernel to Desktop

The complete boot flow from power-on to interactive desktop:

```
1. BIOS → Stage 1 (MBR) → Stage 2 (real mode)
2. Stage 2: E820, VBE 1024x768, ELF kernel load → kernel_main()
3. Phases 0-3: Memory, multitasking, drivers (VGA boot log)
4. Phase 4: ChaosFS mount
5. Phase 5: ChaosGL init (now in graphical mode)
6. Phase 6: KAOS init
7. Phase 7: Lua init
8. KAOS load_all (Mac OS 9 icon parade on boot screen)
9. lua_task_create("/system/desktop.lua") → desktop task starts
10. desktop.lua:
    a. require("/system/wm") → WM initialised
    b. wm.init() → taskbar surface, cursor surface created
    c. Desktop surface created (z=0), wallpaper drawn
    d. Desktop icons loaded from /system/desktop_icons.lua
    e. Main event loop begins — system is now interactive
11. User double-clicks "Files" icon → aios.task.spawn("/apps/files.lua")
    a. files.lua creates surface, registers with wm
    b. Window appears on screen, taskbar shows "File Browser"
12. User is now in a graphical desktop with windowed applications.
```

---

## Phase 10 Acceptance Tests

### Desktop Shell

1. **Boot to desktop:** system boots, Mac OS 9 icon parade shows, desktop appears with wallpaper and icons. Taskbar visible at bottom. Cursor visible and tracks mouse.
2. **Desktop icons:** all configured icons render at correct positions with labels. Double-click launches the app.
3. **Wallpaper:** if `/system/wallpaper.raw` exists, it displays. If not, solid colour. No crash either way.

### File Browser

4. **Launch:** double-click Files icon. Window appears. Current directory is `/`. Files and folders listed.
5. **Navigate:** double-click folder → navigates into it. Path bar updates. Back button returns.
6. **Up:** Up button navigates to parent directory. At `/`, Up is a no-op.
7. **View toggle:** switch between grid and list view. Items render correctly in both modes.
8. **Create folder:** New Folder button creates a directory. It appears in the listing.
9. **Delete:** select file, delete. File removed from listing. `chaos_fsck()` clean after.
10. **Open file:** double-click `.lua` or `.txt` file → text editor opens with that file.

### Settings

11. **Launch:** Settings window appears with tabs.
12. **Theme switch:** select Light theme. All windows (including Settings itself) redraw with light colours on next frame. Switch back to Dark — instant.
13. **System info:** memory, CPU, uptime display. Values update every second. Numbers are plausible.
14. **Module list:** shows loaded KAOS modules with names and versions.

### Terminal

15. **Launch:** Terminal window appears with prompt.
16. **Lua eval:** type `2 + 2`, press Enter. Output shows `4`.
17. **Lua error:** type `nil.x`, press Enter. Error message in red, no crash.
18. **ls command:** type `ls /`. Shows root directory contents.
19. **cat command:** type `cat /system/themes/dark.lua`. Shows file contents.
20. **cd command:** `cd /apps`, then `ls` shows app files.
21. **History:** type 3 commands. Up arrow recalls previous commands in order.
22. **Scrollback:** generate 100 lines of output. Scroll up to see earlier lines.
23. **clear command:** type `clear`. Output area empties.

### Text Editor

24. **Open from file browser:** double-click a `.txt` file in Files. Editor opens with content.
25. **Edit:** type characters, delete with backspace, move cursor with arrows. Modified indicator appears.
26. **Save:** Ctrl+S saves. Reopen file — content matches edits.
27. **New file:** open editor without file argument. Type content, save-as (or provide path).

### Integration

28. **Multi-app:** launch all 4 apps simultaneously. Alt+Tab between them. Each works independently.
29. **Window management:** drag windows, resize, minimize, restore, maximize. All operations smooth.
30. **Memory stability:** open and close all apps 10 times each. `aios.os.meminfo()` shows heap returning to baseline. No leak.
31. **Filesystem integrity:** after all operations, `chaos_fsck()` returns 0.
32. **Theme persistence:** switch theme in Settings. Launch new app — new app uses new theme immediately.

---

## Summary

| Property | Value |
|----------|-------|
| Language | Lua (entire WM and all apps) |
| WM model | Library loaded by desktop shell, not a separate task |
| Window registry | Lua table keyed by surface handle |
| Input routing | WM hit-tests mouse, routes keyboard to active window |
| Z-order range | 0 (desktop), 1-99 (apps), 100 (taskbar), 200+ (system) |
| Taskbar | 1024×32 surface at z=100, shows running apps + clock |
| Cursor | 24×24 surface at z=999, repositioned every frame |
| App menu | Popup from taskbar, scans `/apps/` for `.lua` files |
| Focus model | Click-to-focus + Alt+Tab cycling |
| Keyboard shortcuts | Alt+F4 (close), Alt+Tab (cycle), Ctrl+S (save in editor) |
| Screen bounds | Windows clamped so 32px titlebar always visible |
| Maximize | Double-click titlebar, fills screen minus taskbar |
| Core apps | Desktop shell, file browser, settings, terminal, text editor |
| Total new Lua code | ~1,380 lines across all files |
| Filesystem | `/system/wm.lua`, `/system/desktop.lua`, `/apps/*.lua` |
| Dependencies | Phase 7 (Lua), Phase 8 (UI toolkit), ChaosGL, ChaosFS, input subsystem |
| C code changes | None. Phases 9-10 are pure Lua. |

**Do not proceed to Phase 11 (Claude integration) until all Phase 9-10 acceptance tests pass.**
