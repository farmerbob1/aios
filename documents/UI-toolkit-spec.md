# AIOS UI Toolkit — Specification
## Lua Widget Library, Layout Engine, and Theming

---

## Founding Premise

Every AIOS application that draws a GUI uses the same widget library. No app implements its own button, its own text field, its own scrollbar. The widget library is the single source of truth for how UI elements look and behave. It sits between applications and ChaosGL:

```
Applications (shell.lua, files.lua, settings.lua, claude.lua)
        |
        v
UI Toolkit (/system/ui/)
  Widget library — Button, TextField, Label, Window, etc.
  Layout engine  — flex rows/columns, grid
  Theme table    — colours, radii, spacing, font sizes
        |
        v
ChaosGL Surface API
  chaos_gl.rect, chaos_gl.text, chaos_gl.surface_bind, etc.
        |
        v
C / Kernel
```

**What this gives us:**
- Consistent look across every app — same widget code, same theme
- Theme switching — swap the theme table, everything redraws instantly
- Short apps — a file browser is ~200 lines of Lua, not 2000
- Claude can read and modify UI code — it's plain Lua, not compiled C
- New widgets are just Lua files dropped in `/system/ui/`

**What this is NOT:**
- Not CSS. No selector matching, no specificity, no cascade, no parser. Styling is Lua tables — the language already has the data structure we need.
- Not HTML. No DOM, no markup language, no document model. Widgets are Lua objects with methods.
- Not a general-purpose GUI framework. It targets AIOS specifically — 1024x768, Claude Mono 8x16 font, BGRX pixel format, ChaosGL surfaces.

---

## Architecture

### Widget Base Class

Every widget shares a common interface. `core.lua` defines the base and provides focus management, event dispatch, and the widget tree traversal.

```lua
-- Widget interface contract (all widgets implement these)
widget:draw(x, y)           -- render self at position (x,y) on bound surface
widget:on_input(event)      -- handle input event, return true if consumed
widget:on_focus()           -- called when widget gains focus
widget:on_blur()            -- called when widget loses focus
widget:get_size()           -- return w, h (computed or fixed)
widget:set_size(w, h)       -- set fixed dimensions (optional — layout may override)
widget:contains(px, py)     -- hit test: is point (px,py) inside this widget?
```

**Draw coordinates are absolute, relative to the surface origin.** The layout engine computes final positions and passes them to `draw()`. Widgets do not store their own position — the parent layout owns positioning. This prevents stale position state and simplifies reflow.

**`on_input()` returns true if the event was consumed.** This enables event bubbling — if a child doesn't consume the event, the parent can handle it. Events propagate from leaf to root.

### Widget Tree

Widgets form a tree. Container widgets (Panel, ScrollView, Window) have children. Leaf widgets (Button, Label, Checkbox) do not. The tree is implicit in Lua table nesting — no separate tree data structure.

```lua
-- A window containing a column of widgets
local win = Window.new("Settings", {
    content = flex.col({
        Label.new("Volume"),
        Slider.new(0, 100, 50, on_volume),
        flex.row({
            Button.new("OK", on_ok),
            Button.new("Cancel", on_cancel),
        }, { spacing = 8, align = "right" }),
    }, { spacing = 12, padding = 16 }),
})
```

The tree is traversed top-down for drawing (parent draws before children) and bottom-up for input (children get events first).

---

## Widget Catalogue

### Core Widgets

| Widget | Description | Key Properties |
|--------|-------------|----------------|
| `Label` | Static text, single or multi-line | `text`, `color`, `wrap` |
| `Button` | Clickable button with text | `text`, `on_click`, `enabled` |
| `IconButton` | Button with icon (texture handle) | `icon`, `on_click`, `tooltip` |
| `TextField` | Single-line text input | `text`, `placeholder`, `on_change`, `on_submit` |
| `TextArea` | Multi-line text input with scroll | `text`, `on_change`, `max_lines` |
| `Checkbox` | Toggle with label | `checked`, `label`, `on_change` |
| `Slider` | Horizontal value slider | `min`, `max`, `value`, `on_change` |
| `ProgressBar` | Non-interactive fill bar | `value`, `max`, `color` |
| `FileItem` | File/folder in a file browser | `name`, `file_type`, `icon`, `on_click`, `on_double_click` |
| `AppIcon` | Launchable app on desktop/launcher | `name`, `icon`, `on_launch` |

### Container Widgets

| Widget | Description | Key Properties |
|--------|-------------|----------------|
| `Panel` | Generic container, optional border/bg | `children`, `bg_color`, `border` |
| `ScrollView` | Scrollable content area | `child`, `scroll_x`, `scroll_y` |
| `ListView` | Virtualised scrolling list | `items`, `item_height`, `on_select`, `render_item` |
| `TabView` | Tabbed panel switcher | `tabs[]` (label + content) |
| `Window` | Window chrome: titlebar, close, resize | `title`, `content`, `closable`, `resizable` |
| `Menu` | Dropdown / context menu | `items[]` (label + on_click + shortcut) |
| `Dialog` | Modal dialog with buttons | `title`, `message`, `buttons[]` |

### Widget Specifications

#### Label

```lua
local lbl = Label.new(text, opts)
-- opts (all optional):
--   color    = theme.text_primary     (BGRX)
--   wrap     = false                  (word-wrap within parent width)
--   align    = "left"                 ("left", "center", "right")
```

Draws text using `chaos_gl.text()` or `chaos_gl.text_wrapped()`. Size is computed from text content and font metrics (8px wide per character, 16px tall per line).

#### Button

```lua
local btn = Button.new(text, on_click, opts)
-- opts (all optional):
--   enabled  = true
--   style    = {}    (per-instance style overrides — see Styling)
```

**States:** normal, hovered, pressed, disabled. Each state maps to a theme colour. Transition is immediate — no animation.

**Rendering:** rounded rect background + centered text. Default height: 32px. Default width: text width + 24px horizontal padding. Overridable via `set_size()` or layout constraints.

```
draw():
  bg = disabled ? theme.button_disabled
     : pressed  ? theme.button_pressed
     : hovered  ? theme.button_hover
     : theme.button_normal
  chaos_gl.rect_rounded(x, y, w, h, theme.button_radius, bg)
  chaos_gl.text(centered, text, fg, 0, 0)  -- flags=0: transparent bg
```

**Input:** tracks hover via `contains()` on mouse move. Press on mouse-down while hovered. Click fires on mouse-up while pressed and still hovered. Disabled buttons ignore all input.

#### TextField

```lua
local tf = TextField.new(opts)
-- opts (all optional):
--   placeholder = ""
--   on_change   = nil     (called with new text on each edit)
--   on_submit   = nil     (called on Enter key)
--   max_length  = 256
--   password    = false   (show dots instead of characters)
```

**Rendering:** rect background with 1px border. Text drawn with cursor (blinking `|` at cursor position, toggled every 500ms via frame counter). Placeholder text drawn in `theme.text_placeholder` when empty and unfocused.

**Input:**
- Printable characters: insert at cursor position
- Backspace: delete character before cursor
- Delete: delete character at cursor
- Left/Right arrows: move cursor
- Home/End: cursor to start/end
- Enter: fire `on_submit`
- All edits fire `on_change`

**Selection (v2.1):** shift+arrow for selection, ctrl+A select all, ctrl+C/V copy/paste. Deferred — requires clipboard integration.

**Cursor position** stored as integer character index. Rendering computes pixel offset: `cursor_px = x + padding + cursor_pos * 8`.

**Scroll:** if text is wider than the field, the visible portion scrolls to keep the cursor in view. Scroll offset is tracked internally.

#### TextArea

Multi-line extension of TextField. Wraps text within width. Supports vertical scrolling via embedded ScrollView. Line count stored for height calculation.

#### Checkbox

```lua
local cb = Checkbox.new(label, on_change, opts)
-- opts: checked = false
```

**Rendering:** 16x16 box (rect outline or filled rect) + 8px gap + label text. When checked, the box is filled with `theme.accent` and a checkmark is drawn (two lines forming a tick, or a filled inner rect — whichever is simpler with ChaosGL primitives).

**Input:** toggle on click anywhere in the hit area (box + label).

#### Slider

```lua
local sl = Slider.new(min, max, value, on_change, opts)
-- opts: step = 1, show_value = true
```

**Rendering:** horizontal track (2px tall rect, `theme.slider_track`) with a circular thumb (`theme.accent`, radius 8). Optional value label to the right.

**Input:** drag thumb on mouse-down + mouse-move. Click on track jumps thumb to click position. Snaps to step increments.

**Value calculation:** `value = min + (thumb_x - track_x) / track_w * (max - min)`, rounded to nearest step.

#### ScrollView

```lua
local sv = ScrollView.new(child, opts)
-- opts: scroll_x = 0, scroll_y = 0, show_scrollbar = "auto"
```

**Rendering:** draws child offset by (-scroll_x, -scroll_y) within a clip rect. Scrollbar track and thumb drawn on right edge (vertical) and/or bottom edge (horizontal) when content exceeds viewport.

**Clip integration:** pushes a clip rect via `chaos_gl.push_clip()` before drawing child, pops after. This is the primary use of ChaosGL's clip stack from the widget layer.

**Input:** mouse wheel scrolls vertically (scroll_y += wheel_delta * theme.scroll_speed). Scrollbar thumb is draggable. Content is scrollable by drag if no child consumes the drag.

**Scrollbar visibility:** `"auto"` shows scrollbar only when content exceeds viewport. `"always"` always shows. `"never"` hides.

#### ListView

```lua
local lv = ListView.new(opts)
-- opts:
--   items        = {}          (array of item data)
--   item_height  = 24          (fixed height per row)
--   render_item  = function(item, index, selected, x, y, w, h) end
--   on_select    = nil         (called with item, index)
--   multi_select = false
```

**Virtualised rendering:** only draws items visible in the viewport. If the list has 1000 items but only 20 are visible, only 20 `render_item` calls happen per frame.

```
visible_start = math.floor(scroll_y / item_height)
visible_end   = math.min(#items, visible_start + math.ceil(viewport_h / item_height) + 1)
```

**Selection:** single-click selects. Selected item highlighted with `theme.list_selection`. Multi-select (if enabled): ctrl+click toggles, shift+click range-selects.

**Scroll:** inherits ScrollView behaviour. Only vertical scroll.

#### TabView

```lua
local tv = TabView.new(tabs)
-- tabs: array of { label = "Display", content = widget }
```

**Rendering:** horizontal tab bar at top (each tab is a clickable label with underline or background highlight for active tab). Content area below shows the active tab's content widget. Inactive tabs' content is not drawn.

**Input:** clicking a tab label switches the active tab. Keyboard: left/right arrows switch tabs when tab bar is focused.

#### Window

```lua
local win = Window.new(title, opts)
-- opts:
--   content    = nil       (child widget — the window body)
--   closable   = true      (show close button)
--   resizable  = true      (allow drag-resize)
--   min_w      = 200
--   min_h      = 150
```

**Window is a widget, not a surface.** The Window widget draws titlebar chrome and hosts content. The app task owns the surface and renders the Window widget into it. This keeps the surface lifecycle in the app's control.

**Rendering:**
- Titlebar: `theme.titlebar_height` px tall (default 28). Background `theme.titlebar_bg` (or `theme.titlebar_bg_inactive` when unfocused). Title text left-aligned with padding.
- Close button: right side of titlebar, 16x16, X icon drawn with two `chaos_gl.line()` calls. Hover highlight.
- Resize handle: 12x12 region at bottom-right corner. Three diagonal lines drawn as grip indicator.
- Content area: clipped to window body (below titlebar, inside borders).

**Input:**
- Titlebar drag: moves the surface position via `chaos_gl.surface_set_position()`.
- Close button: fires `on_close` callback.
- Resize handle drag: resizes surface via `chaos_gl.surface_resize()`, triggers layout reflow on content.
- All input within content area is forwarded to the content widget.

#### Menu

```lua
local menu = Menu.new(items)
-- items: array of { label, on_click, shortcut, separator }
-- separator = true renders a horizontal line instead of a label
```

**Rendering:** vertical list of items in a bordered panel. Hovered item highlighted with `theme.menu_hover`. Shortcut text right-aligned. Fixed width: max label width + shortcut width + padding.

**Rendering approach:** Menu creates its own surface (high z-order) so it draws on top of everything. Destroyed when dismissed.

**Input:** hover highlights, click fires `on_click` and dismisses. Click outside or Escape dismisses without firing.

#### Dialog

```lua
local dlg = Dialog.new(opts)
-- opts:
--   title    = "Dialog"
--   message  = ""
--   buttons  = { { label = "OK", on_click = fn, style = "primary" } }
--   modal    = true
```

**Modal behaviour:** when modal is true, a fullscreen overlay surface is created behind the dialog to intercept clicks and dim the background. The overlay surface has z-order just below the dialog surface and uses `chaos_gl.surface_set_alpha(overlay, 128)` for semi-transparency (ChaosGL per-surface alpha blending). The overlay is filled with black; the compositor blends it at 50% over the surfaces below. Clicking the overlay does nothing (or flashes the dialog border as feedback).

**Rendering:** centered window with message text (word-wrapped) and a button row at the bottom, right-aligned.

#### FileItem

```lua
local fi = FileItem.new(name, file_type, opts)
-- file_type: "folder", "file", "text", "image", "audio", "binary", "lua", "cobj", "raw"
-- opts (all optional):
--   icon          = nil         (override icon — texture handle or icon name)
--   on_click      = nil         (single click — typically selects)
--   on_double_click = nil       (double click — typically opens)
--   selected      = false
--   view          = "grid"      ("grid" or "list")
```

**Icon resolution:** each `file_type` maps to a default icon from the icon registry (see below). The `icon` opt overrides this — used for folders with custom icons (e.g. the home folder) or files with thumbnails.

**Grid view rendering (view = "grid"):**
- Icon centred at top: 32x32 icon blit
- Filename below icon: centred text, truncated with "..." if wider than cell
- Cell size: 72x72 (icon + label + padding)
- Selected: `theme.file_selection` background rect behind the item
- Hovered: `theme.file_hover` background

```
+------------------+
|                  |
|    [  icon  ]    |   32x32 icon
|                  |
|   filename.tx..  |   text, truncated
+------------------+
       72x72 cell
```

**List view rendering (view = "list"):**
- Row: 16x16 icon + 8px gap + filename + right-aligned metadata (size, date)
- Row height: `theme.list_item_height` (default 24)
- Selected: `theme.file_selection` row highlight
- Hovered: `theme.file_hover` row highlight

```
[icon] documents                          4 items
[icon] readme.txt                         2.1 KB
[icon] screenshot.raw                    48.0 KB
```

**Input:**
- Single click: sets `selected = true`, fires `on_click` with the item
- Double click: fires `on_double_click` (open folder / launch file)
- Double-click detection: two clicks within 400ms on the same item

**File type to icon mapping (defaults):**

| file_type | Icon | Description |
|-----------|------|-------------|
| `"folder"` | folder icon (yellow) | Directory |
| `"file"` | generic file icon | Unknown file type |
| `"text"` | text file icon (lines) | .txt, .md, .log |
| `"image"` | image icon (landscape) | .raw, .bmp |
| `"audio"` | audio icon (note) | .wav, .pcm |
| `"binary"` | binary icon (010) | .bin, .dat |
| `"lua"` | Lua icon (moon/gear) | .lua scripts |
| `"cobj"` | 3D model icon (cube) | .cobj models |
| `"raw"` | texture icon (grid) | .raw textures |

#### AppIcon

```lua
local app = AppIcon.new(name, icon, on_launch, opts)
-- name:      display name shown below icon
-- icon:      texture handle or icon name from registry
-- on_launch: callback when app is activated
-- opts (all optional):
--   tooltip   = nil          (shown on hover after delay)
--   badge     = nil          (notification count — small number drawn on icon corner)
--   pinned    = false        (pinned to taskbar)
```

**Used in:** desktop grid, taskbar, app launcher/start menu.

**Desktop rendering (large):**
- 48x48 icon centred
- App name below, centred, white text with subtle drop shadow for readability over wallpaper
- Cell size: 80x80
- Hover: subtle highlight behind icon
- Selected (clicked): brighter highlight

```
+--------------------+
|                    |
|     [  icon   ]    |   48x48 icon
|                    |
|     App Name       |   text with shadow
+--------------------+
        80x80 cell
```

**Taskbar rendering (small):**
- 24x24 icon only (no label — tooltip on hover)
- Badge: small circle at top-right corner of icon with count text (e.g. notification count)
- Active app: underline indicator below icon (`theme.accent`, 2px)

```
[icon] [icon] [icon]    24x24 each, horizontal row
  __                    underline on active app
```

**Input:**
- Double click (desktop): fires `on_launch`
- Single click (taskbar): fires `on_launch` or brings existing window to front
- Right click: opens context menu (if provided)

**Badge rendering:** when `badge ~= nil` and `badge > 0`, draw a small filled circle (radius 7, `theme.badge_bg`) at the top-right corner of the icon with the count as text (1-2 characters). Badge > 99 shows "99+".

### Icon Registry

Icons are small textures (16x16, 24x24, 32x32, 48x48) loaded from ChaosFS. The icon registry provides named access to icons so widgets don't hardcode texture paths.

```lua
-- /system/ui/icons.lua

local icons = {}

-- Load all standard icons at startup
function icons.init()
    icons.register("folder",      "/system/icons/folder_16.raw",
                                   "/system/icons/folder_32.raw",
                                   "/system/icons/folder_48.raw")
    icons.register("file",        "/system/icons/file_16.raw",
                                   "/system/icons/file_32.raw",
                                   "/system/icons/file_48.raw")
    icons.register("text",        "/system/icons/text_16.raw",
                                   "/system/icons/text_32.raw",
                                   "/system/icons/text_48.raw")
    icons.register("lua",         "/system/icons/lua_16.raw",
                                   "/system/icons/lua_32.raw",
                                   "/system/icons/lua_48.raw")
    icons.register("image",       "/system/icons/image_16.raw",
                                   "/system/icons/image_32.raw",
                                   "/system/icons/image_48.raw")
    -- ... etc for all file types
    icons.register("app_shell",   "/system/icons/shell_32.raw",
                                   "/system/icons/shell_48.raw")
    icons.register("app_files",   "/system/icons/files_32.raw",
                                   "/system/icons/files_48.raw")
    icons.register("app_settings","/system/icons/settings_32.raw",
                                   "/system/icons/settings_48.raw")
    icons.register("app_claude",  "/system/icons/claude_32.raw",
                                   "/system/icons/claude_48.raw")
    icons.register("close",       "/system/icons/close_16.raw")
    icons.register("minimize",    "/system/icons/minimize_16.raw")
    icons.register("maximize",    "/system/icons/maximize_16.raw")
end

-- Register an icon with multiple sizes.
-- Sizes are auto-detected from texture dimensions on load.
function icons.register(name, ...)
    local sizes = {}
    for _, path in ipairs({...}) do
        local tex = chaos_gl.load_texture(path)
        if tex >= 0 then
            local w, h = chaos_gl.texture_get_size(tex)
            sizes[w] = tex   -- keyed by width (16, 24, 32, 48)
        end
    end
    icons[name] = sizes
end

-- Get texture handle for an icon at the requested size.
-- Returns the exact size if available, otherwise the nearest larger size.
-- Falls back to a magenta "missing" icon if not found.
function icons.get(name, size)
    local entry = icons[name]
    if not entry then return icons._missing end
    return entry[size] or entry[32] or entry[16] or icons._missing
end
```

**Multi-size icons:** each icon is stored at multiple resolutions. Widgets request the size they need — `icons.get("folder", 32)` for file browser grid, `icons.get("folder", 16)` for list view. No runtime scaling — each size is a separate pre-rendered texture.

**Icon file format:** standard `.raw` textures (RAWT header + BGRX pixels), same as ChaosGL textures. Transparency via `chaos_gl.blit_keyed()` (magenta `0x00FF00FF` as key colour) or `chaos_gl.blit_alpha()` for smooth edges.

**Filesystem layout:**

```
/system/icons/
+-- folder_16.raw
+-- folder_32.raw
+-- folder_48.raw
+-- file_16.raw
+-- file_32.raw
+-- file_48.raw
+-- text_16.raw
+-- text_32.raw
+-- lua_16.raw
+-- lua_32.raw
+-- image_16.raw
+-- image_32.raw
+-- audio_16.raw
+-- audio_32.raw
+-- binary_16.raw
+-- binary_32.raw
+-- cobj_32.raw
+-- raw_32.raw
+-- shell_32.raw
+-- shell_48.raw
+-- files_32.raw
+-- files_48.raw
+-- settings_32.raw
+-- settings_48.raw
+-- claude_32.raw
+-- claude_48.raw
+-- close_16.raw
+-- minimize_16.raw
+-- maximize_16.raw
+-- missing_16.raw         -- magenta "?" fallback
+-- missing_32.raw
```

**File type auto-detection:** when a FileItem is created from a path without an explicit `file_type`, the extension is used:

```lua
local ext_to_type = {
    txt = "text", md = "text", log = "text",
    lua = "lua",
    raw = "raw",
    cobj = "cobj",
    wav = "audio", pcm = "audio",
    bin = "binary", dat = "binary",
}

function FileItem.type_from_path(path)
    local ext = path:match("%.(%w+)$")
    return ext and ext_to_type[ext:lower()] or "file"
end
```

Directories are always `"folder"`. No extension check needed — ChaosFS provides the is-directory flag.

---

## Layout Engine

### Design Decision: Not CSS

Styling is Lua tables. Layout is Lua function calls. There is no parser, no selector engine, no cascade. This is intentional:

- **Lua tables are stylesheets.** `theme.button_normal = 0x003A3A3A` is a CSS variable without needing a tokenizer.
- **Lua function calls are layout declarations.** `flex.row({...}, { spacing = 8 })` is `display: flex; flex-direction: row; gap: 8px` without needing a parser.
- **Per-widget style overrides are inline styles.** `Button.new("OK", fn, { bg = 0xFF0000 })` is `style="background: red"` without needing attribute parsing.

CSS exists because the web needs to separate untrusted content from styling across thousands of elements. AIOS has one developer, ten apps, and full control over every widget. The complexity of a CSS engine buys nothing here.

### Flex Layout

The primary layout primitive. Arranges children in a row or column with configurable spacing, padding, and alignment.

```lua
-- Horizontal row
local row = flex.row(children, opts)

-- Vertical column
local col = flex.col(children, opts)
```

**Options:**

```lua
{
    spacing  = 0,        -- pixels between children
    padding  = 0,        -- pixels inside container edge (uniform)
                         -- or { top, right, bottom, left } for per-side
    align    = "start",  -- cross-axis: "start", "center", "end", "stretch"
    justify  = "start",  -- main-axis: "start", "center", "end", "space_between"
    wrap     = false,    -- wrap to next row/column if children exceed width/height
}
```

**Layout algorithm (flex.row):**

```
1. Query each child's size via get_size()
2. Sum child widths + (n-1) * spacing + padding.left + padding.right = total_w
3. Available width = parent_w (passed in during layout)
4. If justify == "start":     start_x = padding.left
   If justify == "center":    start_x = (available - total_content) / 2
   If justify == "end":       start_x = available - total_content - padding.right
   If justify == "space_between": distribute remaining space equally between children
5. For each child:
     child_x = current_x
     child_y = based on align:
       "start":   padding.top
       "center":  (container_h - child_h) / 2
       "end":     container_h - child_h - padding.bottom
       "stretch": padding.top, child_h = container_h - padding.top - padding.bottom
     Advance current_x by child_w + spacing
6. Call child:draw(child_x, child_y) for each child
```

`flex.col` is the same algorithm with axes swapped.

**Flex children can have flex weight:** a child with `flex = 1` expands to fill remaining space. Two children with `flex = 1` each get half the remaining space. This enables common patterns like a fixed sidebar + flexible content area.

```lua
flex.row({
    Panel.new(sidebar, { w = 200 }),          -- fixed 200px
    Panel.new(content, { flex = 1 }),          -- fills remaining width
}, { spacing = 0 })
```

### Grid Layout

For tabular arrangements. Less common than flex but useful for settings panels, icon grids, and dashboards.

```lua
local g = grid.new(opts)
-- opts:
--   cols       = 3          (number of columns)
--   row_height = 32         (fixed row height, or "auto" for tallest child)
--   col_width  = "equal"    ("equal" divides evenly, or array of widths)
--   spacing    = { x = 8, y = 8 }
--   padding    = 8
```

**Layout:** children fill cells left-to-right, top-to-bottom. Cell position is deterministic from index:

```
col = (index - 1) % cols
row = math.floor((index - 1) / cols)
cell_x = padding + col * (col_width + spacing.x)
cell_y = padding + row * (row_height + spacing.y)
```

### Layout Reflow

Layout is recomputed when:
- The surface is resized (window resize)
- A widget's content changes in a way that affects size (text change in label, items added to list)
- A widget calls `self:invalidate()` which marks the layout tree as dirty

**Reflow is not per-frame.** Layout is computed once when the tree is built or invalidated. The computed positions are cached and reused every frame until the next invalidation. This matters because layout computation walks the entire widget tree — doing it 60 times per second is wasteful when nothing changed.

```lua
-- Widget base provides:
function Widget:invalidate()
    self._layout_dirty = true
    if self.parent then self.parent:invalidate() end
end

-- Layout containers check dirty flag in draw:
function FlexRow:draw(x, y)
    if self._layout_dirty then
        self:reflow(x, y, self.w, self.h)
        self._layout_dirty = false
    end
    for _, child in ipairs(self.children) do
        child:draw(child._layout_x, child._layout_y)
    end
end
```

---

## Theming

### Theme Table

A theme is a Lua table loaded from disk. It contains every colour, dimension, and visual parameter the widget library uses. Widgets never hardcode colours — they always read from the global `theme` table.

```lua
-- /system/themes/dark.lua
return {
    -- Window
    window_bg              = 0x002D2D2D,
    window_border          = 0x00555555,
    titlebar_bg            = 0x003C3C3C,
    titlebar_bg_inactive   = 0x00333333,
    titlebar_text          = 0x00FFFFFF,
    titlebar_height        = 28,

    -- General text
    text_primary           = 0x00FFFFFF,
    text_secondary         = 0x00AAAAAA,
    text_placeholder       = 0x00666666,
    text_disabled          = 0x00555555,

    -- Accent (selection, focus, active elements)
    accent                 = 0x00FF8800,  -- BGRX: orange
    accent_hover           = 0x00FFA040,
    accent_text            = 0x00FFFFFF,

    -- Button
    button_normal          = 0x003A3A3A,
    button_hover           = 0x004A4A4A,
    button_pressed         = 0x002A2A2A,
    button_disabled        = 0x00333333,
    button_text            = 0x00FFFFFF,
    button_text_disabled   = 0x00666666,
    button_radius          = 4,
    button_height          = 32,
    button_padding_h       = 12,

    -- TextField
    field_bg               = 0x00222222,
    field_border           = 0x00555555,
    field_border_focus     = 0x00FF8800,  -- accent on focus
    field_text             = 0x00FFFFFF,
    field_cursor           = 0x00FFFFFF,
    field_selection        = 0x00FF8800,
    field_height           = 28,
    field_padding          = 6,

    -- Checkbox
    checkbox_size          = 16,
    checkbox_border        = 0x00666666,
    checkbox_checked_bg    = 0x00FF8800,
    checkbox_check_color   = 0x00FFFFFF,

    -- Slider
    slider_track           = 0x00444444,
    slider_track_height    = 4,
    slider_thumb_radius    = 8,
    slider_thumb_color     = 0x00FF8800,

    -- ScrollView / scrollbar
    scrollbar_width        = 10,
    scrollbar_track        = 0x00333333,
    scrollbar_thumb        = 0x00555555,
    scrollbar_thumb_hover  = 0x00777777,
    scroll_speed           = 48,           -- pixels per wheel tick

    -- ListView
    list_item_height       = 24,
    list_selection         = 0x00FF8800,
    list_hover             = 0x003A3A3A,
    list_stripe            = 0x00323232,   -- alternating row bg (0 = none)

    -- TabView
    tab_height             = 32,
    tab_bg                 = 0x00333333,
    tab_active_bg          = 0x002D2D2D,   -- matches window_bg for seamless look
    tab_text               = 0x00AAAAAA,
    tab_text_active        = 0x00FFFFFF,
    tab_indicator          = 0x00FF8800,   -- active tab underline
    tab_indicator_height   = 2,

    -- Menu
    menu_bg                = 0x003A3A3A,
    menu_border            = 0x00555555,
    menu_hover             = 0x00FF8800,
    menu_text              = 0x00FFFFFF,
    menu_shortcut_text     = 0x00888888,
    menu_separator         = 0x00444444,

    -- Dialog
    dialog_overlay_alpha   = 128,           -- surface alpha for modal overlay (0-255)
    dialog_bg              = 0x003A3A3A,

    -- FileItem
    file_selection         = 0x00FF8800,   -- selected file highlight
    file_hover             = 0x003A3A3A,   -- hovered file highlight
    file_grid_cell         = 72,           -- grid cell size
    file_text_shadow       = 0x00000000,   -- text shadow for desktop icons

    -- AppIcon
    app_cell_size          = 80,           -- desktop icon cell size
    app_taskbar_size       = 24,           -- taskbar icon size
    app_active_indicator   = 0x00FF8800,   -- underline for active taskbar app
    badge_bg               = 0x000000FF,   -- notification badge background (red)
    badge_text             = 0x00FFFFFF,   -- notification badge text

    -- General
    focus_outline          = 0x00FF8800,
    focus_outline_width    = 2,
    border_radius          = 4,
    spacing_sm             = 4,
    spacing_md             = 8,
    spacing_lg             = 16,
    padding_sm             = 4,
    padding_md             = 8,
    padding_lg             = 16,
}
```

### Per-Widget Style Overrides

Any widget accepts an optional `style` table at construction that overrides specific theme values for that instance only. This is the equivalent of inline CSS — used sparingly for one-off styling.

```lua
-- A red "Delete" button that overrides just the background colours
Button.new("Delete", on_delete, {
    style = {
        button_normal  = CHAOS_GL_RGB(180, 40, 40),
        button_hover   = CHAOS_GL_RGB(200, 60, 60),
        button_pressed = CHAOS_GL_RGB(140, 30, 30),
    }
})
```

**Resolution order:** widget checks `self.style[key]` first, falls back to `theme[key]`. No deeper cascade. Two levels is enough.

```lua
-- In any widget:
function Widget:get_style(key)
    if self.style and self.style[key] ~= nil then
        return self.style[key]
    end
    return theme[key]
end
```

### Theme Loading and Switching

```lua
-- /system/ui/core.lua provides:

-- Global theme table — all widgets read from this
theme = {}

-- Load a theme file and apply it. Triggers full redraw.
function ui.load_theme(path)
    theme = dofile(path)
end

-- Default: load dark theme at startup
ui.load_theme("/system/themes/dark.lua")
```

Switching themes at runtime is instant — `theme` is a global table reference. The next frame draws everything with new colours. No per-widget notification needed.

---

## Focus System

### Focus Chain

One widget at a time has focus. Focus determines which widget receives keyboard input. The focus chain is a flat ordered list of focusable widgets, built by depth-first traversal of the widget tree.

```lua
-- core.lua manages focus:
local focus_chain = {}   -- ordered list of focusable widgets
local focus_index = 0    -- current index in chain (0 = none)

function ui.build_focus_chain(root)
    focus_chain = {}
    root:collect_focusable(focus_chain)
end

function ui.focus_next()
    if #focus_chain == 0 then return end
    if focus_index > 0 then focus_chain[focus_index]:on_blur() end
    focus_index = (focus_index % #focus_chain) + 1
    focus_chain[focus_index]:on_focus()
end

function ui.focus_prev()
    if #focus_chain == 0 then return end
    if focus_index > 0 then focus_chain[focus_index]:on_blur() end
    focus_index = ((focus_index - 2) % #focus_chain) + 1
    focus_chain[focus_index]:on_focus()
end

function ui.set_focus(widget)
    -- find widget in chain, blur old, focus new
end
```

**Tab** advances focus. **Shift+Tab** reverses. **Clicking** a focusable widget sets focus directly.

**Focusable widgets:** TextField, TextArea, Button, Checkbox, Slider, TabView (tab bar). Non-focusable: Label, Panel, ProgressBar, ScrollView (scroll by mouse only).

**Focus indicator:** focused widget draws a `theme.focus_outline` border (2px, accent colour). Each widget is responsible for drawing its own focus indicator in its `draw()` method when `self.focused == true`.

---

## Input Events

### Event Structure

Input events are passed from the app's event loop to the widget tree. The event format matches what the AIOS input subsystem provides:

```lua
-- Event types
EVENT_MOUSE_MOVE    = 1
EVENT_MOUSE_DOWN    = 2
EVENT_MOUSE_UP      = 3
EVENT_MOUSE_WHEEL   = 4
EVENT_KEY_DOWN      = 5
EVENT_KEY_UP        = 6
EVENT_KEY_CHAR      = 7   -- printable character (after keymap translation)

-- Event table
event = {
    type    = EVENT_KEY_DOWN,
    key     = KEY_TAB,       -- scancode (for KEY_DOWN/KEY_UP)
    char    = nil,           -- character string (for KEY_CHAR)
    mouse_x = 0,             -- absolute mouse position on surface
    mouse_y = 0,
    button  = 0,             -- mouse button (1=left, 2=right, 3=middle)
    wheel   = 0,             -- scroll delta (positive = up)
    shift   = false,
    ctrl    = false,
    alt     = false,
}
```

### Event Dispatch

```lua
-- App event loop (standard pattern):
while running do
    chaos_gl.surface_clear(surface, theme.window_bg)
    root_widget:draw(0, 0)
    chaos_gl.surface_present(surface)

    local event = input_poll()
    if event then
        -- Keyboard events go to focused widget
        if event.type >= EVENT_KEY_DOWN then
            -- Tab/Shift+Tab handled by focus system
            if event.type == EVENT_KEY_DOWN and event.key == KEY_TAB then
                if event.shift then ui.focus_prev() else ui.focus_next() end
            else
                local focused = focus_chain[focus_index]
                if focused then focused:on_input(event) end
            end
        else
            -- Mouse events: hit-test from root, dispatch to deepest containing widget
            root_widget:on_input(event)
        end
    end

    task_sleep(16)
end
```

**Mouse event hit-testing:** container widgets forward mouse events to the child whose bounds contain the mouse position. This naturally traverses the tree to the deepest widget under the cursor.

---

## Window Manager Integration

### Surface Ownership

Each app task creates and owns its ChaosGL surface. The Window widget draws chrome (titlebar, borders, close button) but does not create or manage the surface. This separation keeps surface lifecycle in the app's control:

```lua
-- App creates surface
local surface = chaos_gl.surface_create(600, 400, false)
chaos_gl.surface_set_position(surface, 200, 150)
chaos_gl.surface_set_zorder(surface, 10)
chaos_gl.surface_set_visible(surface, true)

-- App creates Window widget for chrome
local win = Window.new("My App", {
    content = build_content(),
    on_close = function() running = false end,
    on_resize = function(w, h)
        chaos_gl.surface_resize(surface, w, h)
    end,
})

-- App render loop
while running do
    chaos_gl.surface_bind(surface)
    chaos_gl.surface_clear(surface, theme.window_bg)
    win:draw(0, 0)
    chaos_gl.surface_present(surface)
    -- ... event handling ...
    task_sleep(16)
end

-- App cleanup
chaos_gl.surface_destroy(surface)
```

### Window Dragging

When the user drags the titlebar, the Window widget calls `chaos_gl.surface_set_position()` to move the surface on screen. The drag delta is computed from mouse events:

```lua
function Window:on_titlebar_drag(event)
    local dx = event.mouse_x - self._drag_start_x
    local dy = event.mouse_y - self._drag_start_y
    local sx, sy = chaos_gl.surface_get_position(self.surface)
    chaos_gl.surface_set_position(self.surface, sx + dx, sy + dy)
end
```

**Constraint:** windows cannot be dragged fully off-screen. The WM (or Window widget) clamps position so at least the titlebar remains grabbable.

### Window Resizing

Drag from the resize handle adjusts surface dimensions. The Window widget calls `chaos_gl.surface_resize()` and triggers layout reflow on its content:

```lua
function Window:on_resize_drag(event)
    local new_w = math.max(self.min_w, self._drag_base_w + dx)
    local new_h = math.max(self.min_h, self._drag_base_h + dy)
    chaos_gl.surface_resize(self.surface, new_w, new_h)
    self.w = new_w
    self.h = new_h
    self:invalidate()  -- triggers layout reflow
end
```

### Z-Order and Focus

When a window is clicked, the WM brings it to the front by adjusting z-order. This is handled by a WM module (not the widget library itself):

```lua
-- /system/wm.lua (window manager)
function wm.bring_to_front(surface)
    -- reassign z-orders so this surface is highest among normal windows
    -- z_order 1-99 range for normal apps
end
```

---

## App Lifecycle Pattern

Every GUI app follows the same structure:

```lua
-- 1. Require UI modules
local ui = require "/system/ui/core"
local Button = require "/system/ui/button"
local Label = require "/system/ui/label"
-- ... etc

-- 2. Create surface
local surface = chaos_gl.surface_create(width, height, false)
chaos_gl.surface_set_position(surface, x, y)
chaos_gl.surface_set_zorder(surface, z)
chaos_gl.surface_set_visible(surface, true)

-- 3. Build widget tree
local root = Window.new("App Title", {
    content = flex.col({ ... }),
    on_close = function() running = false end,
})

-- 4. Build focus chain
ui.build_focus_chain(root)

-- 5. Render loop
local running = true
while running do
    chaos_gl.surface_bind(surface)
    chaos_gl.surface_clear(surface, theme.window_bg)
    root:draw(0, 0)
    chaos_gl.surface_present(surface)

    local event = input_poll()
    if event then
        ui.dispatch_event(root, event)
    end

    task_sleep(16)
end

-- 6. Cleanup
chaos_gl.surface_destroy(surface)
```

This is the same pattern for every app — shell, file browser, settings, Claude overlay. The only differences are the widget tree and the event handling logic.

---

## Filesystem Layout

```
/system/
+-- ui/
|   +-- core.lua          -- Widget base, focus management, event dispatch, ui.load_theme
|   +-- button.lua
|   +-- textfield.lua
|   +-- textarea.lua
|   +-- label.lua
|   +-- checkbox.lua
|   +-- slider.lua
|   +-- progressbar.lua
|   +-- scrollview.lua
|   +-- listview.lua
|   +-- window.lua        -- Window chrome: titlebar, close, resize handle
|   +-- panel.lua         -- Container with optional border/background
|   +-- menu.lua          -- Dropdown / context menus
|   +-- dialog.lua        -- Modal dialogs
|   +-- tabview.lua       -- Tabbed panels
|   +-- iconbutton.lua    -- Button with icon
|   +-- fileitem.lua      -- File/folder item (grid and list view)
|   +-- appicon.lua       -- App launcher icon (desktop and taskbar)
|   +-- icons.lua         -- Icon registry: named multi-size icon lookup
+-- icons/
|   +-- folder_16.raw     -- File type icons at 16/32/48px
|   +-- folder_32.raw
|   +-- folder_48.raw
|   +-- file_16.raw
|   +-- file_32.raw
|   +-- ... (see Icon Registry section for full list)
+-- layout/
|   +-- flex.lua          -- flex.row(), flex.col()
|   +-- grid.lua          -- grid.new()
+-- themes/
|   +-- dark.lua          -- Default theme
|   +-- light.lua         -- Light theme
+-- wm.lua                -- Window manager: z-order, focus, taskbar
```

**Total estimated size:** ~1700 lines of Lua for the widget library, ~200 lines for the icon registry, ~300 lines for layout, ~100 lines per theme. Small enough to fit comfortably in ChaosFS.

---

## Dependencies

| Dependency | Used For |
|------------|----------|
| ChaosGL surface API | `surface_create`, `surface_bind`, `surface_clear`, `surface_present`, `surface_set_position`, `surface_get_position`, `surface_set_zorder`, `surface_get_zorder`, `surface_set_alpha`, `surface_resize` |
| ChaosGL 2D API | `rect`, `rect_rounded`, `rect_outline`, `circle`, `line`, `text`, `text_wrapped`, `text_width`, `push_clip`, `pop_clip`, `blit` |
| ChaosGL textures | Icon loading for `IconButton`, image blits |
| Input subsystem | `input_poll()` for keyboard and mouse events |
| ChaosFS | Loading theme files, widget source files via `require` / `dofile` |
| Lua runtime | Everything — the entire toolkit is Lua |

---

## Acceptance Tests

### Widget Tests

1. **Button states:** create Button, simulate hover/press/release. Verify correct theme colour used for each state. Click fires callback exactly once.
2. **Button disabled:** disabled button does not respond to hover/press. Uses `theme.button_disabled` colour.
3. **Label:** Label renders text at correct position. Wrapped label wraps at specified width. `get_size()` returns correct dimensions.
4. **TextField input:** type characters, verify text updates. Backspace deletes. Cursor moves with arrows. Home/End jump to start/end. `on_change` fires on each edit. `on_submit` fires on Enter.
5. **TextField scroll:** type text wider than field. Cursor remains visible. Text scrolls horizontally.
6. **Checkbox toggle:** click toggles `checked` state. `on_change` fires with new state. Visual matches state.
7. **Slider drag:** drag thumb, verify `value` updates. Click track, verify thumb jumps. Value clamps to min/max. Step snapping works.
8. **ProgressBar:** set value to 0, 50, 100. Bar fills proportionally. Non-interactive — ignores input.

### FileItem and AppIcon Tests

9. **FileItem grid view:** create FileItem with type "folder". Renders 32x32 folder icon centred, filename below. Correct cell size.
10. **FileItem list view:** create FileItem with view="list". Renders 16x16 icon + filename in a row. Correct row height.
11. **FileItem selection:** click selects item (highlight shown). Double click fires `on_double_click`. Second single click on different item deselects first.
12. **FileItem type detection:** create FileItem from path "test.lua". Auto-detects type "lua", shows Lua icon. Unknown extension shows generic file icon.
13. **AppIcon desktop:** create AppIcon with 48x48 icon. Renders icon + name below with shadow. Double click fires `on_launch`.
14. **AppIcon taskbar:** create AppIcon in taskbar mode (24x24). Icon only, no label. Active app shows underline indicator.
15. **AppIcon badge:** set badge=5 on AppIcon. Small red circle with "5" appears at top-right of icon. Badge=0 hides badge.
16. **Icon registry:** register "folder" icon at 16 and 32px. `icons.get("folder", 32)` returns 32px handle. `icons.get("folder", 16)` returns 16px handle. `icons.get("nonexistent", 32)` returns missing icon fallback.

### Container Tests

17. **Panel:** panel with children draws all children. Optional border and background render.
18. **ScrollView clipping:** content larger than viewport. Only visible portion draws. Scrollbar appears. Mouse wheel scrolls. Content beyond clip boundary not visible.
19. **ListView virtualisation:** list with 1000 items. Only ~20 `render_item` calls per frame. Scroll to bottom — correct items render. Selection works.
20. **TabView switching:** click tab labels to switch. Correct content shown. Inactive tab content not drawn.
21. **Window chrome:** titlebar renders with title text and close button. Close button fires callback. Content draws below titlebar, clipped to window body.
22. **Menu:** open menu, hover items, click fires callback and dismisses. Click outside dismisses. Menu draws on top of everything (high z-order surface).
23. **Dialog modal:** dialog with overlay. Clicking overlay does not dismiss. Clicking dialog button fires callback and dismisses.

### Layout Tests

24. **Flex row:** 3 buttons in a row with spacing=8. Buttons positioned correctly with 8px gaps. Total width matches.
25. **Flex col:** 3 labels in a column with spacing=4. Labels stacked vertically with 4px gaps.
26. **Flex justify:** `justify = "center"` centres children. `"end"` right-aligns. `"space_between"` distributes evenly.
27. **Flex align:** `align = "center"` vertically centres children in a row. `"stretch"` makes children fill container height.
28. **Flex weight:** two panels with `flex = 1` each get half the remaining space. One fixed + one flex fills correctly.
29. **Grid layout:** 6 items in 3-column grid. Items appear in correct cells. Spacing applied.
30. **Layout reflow:** resize surface. Layout recomputes. Widgets reposition correctly. Flex children adjust to new width.

### Theme Tests

31. **Theme load:** load dark theme. All widget colours match theme table values.
32. **Theme switch:** load light theme at runtime. Next frame draws all widgets with new colours. No stale colours.
33. **Style override:** Button with style override uses override colour. Other buttons use theme colour. Override is per-instance, not global.

### Focus Tests

34. **Tab navigation:** Tab cycles focus through all focusable widgets in order. Shift+Tab reverses. Focus indicator visible on focused widget.
35. **Click focus:** clicking a TextField focuses it. Previously focused widget blurs. Keyboard input goes to clicked widget.
36. **Focus chain rebuild:** add/remove widgets. Rebuild focus chain. Tab order reflects new tree structure.

### Integration Tests

37. **Full app pattern:** create surface, build widget tree with Window + flex layout + buttons + text field. Render loop runs. Input works. Close button destroys surface. No crash, no leak.
38. **Multi-window:** two app tasks with separate surfaces and widget trees. Each handles input independently. Neither corrupts the other's state.
39. **Window drag:** drag titlebar. Surface position updates. Content moves with window. Release stops drag.
40. **Window resize:** drag resize handle. Surface resizes. Layout reflows. Content adjusts to new dimensions. Minimum size enforced.

---

## Summary

| Property | Value |
|----------|-------|
| Language | Lua (entire toolkit) |
| Styling | Lua theme tables — no CSS, no parser |
| Layout | Flex row/col + grid — Lua function calls |
| Widget interface | `draw()`, `on_input()`, `on_focus()`, `on_blur()`, `get_size()` |
| Theme switching | Instant — swap global `theme` table, next frame uses new colours |
| Style override | Per-widget `style` table, 2-level lookup (instance then theme) |
| Focus | Tab/Shift+Tab chain, click-to-focus, one focused widget at a time |
| Event dispatch | Keyboard to focused widget, mouse hit-tested through tree |
| Surface ownership | App task creates/owns surface, Window widget draws chrome |
| Layout reflow | On invalidation only, not per-frame |
| Font | Claude Mono 8x16 (ChaosGL) |
| Resolution | 1024x768 target |
| Pixel format | BGRX 32bpp (ChaosGL) |
| Widget count | 17 widgets (10 core + 7 container) |
| Icon system | Multi-size icon registry, auto file-type detection |
| Estimated code size | ~2200 lines Lua total |
| Filesystem | `/system/ui/`, `/system/icons/`, `/system/layout/`, `/system/themes/` |
