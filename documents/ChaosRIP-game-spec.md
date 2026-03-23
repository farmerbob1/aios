# ChaosRIP — FPS Game Specification
## AIOS v2 Application — Proof of Concept

---

## Founding Premise

ChaosRIP is a Doom/Quake-hybrid first-person shooter running natively on AIOS. It is not a tech demo that renders a spinning cube. It is a game — a player navigates a 3D environment, shoots enemies, collects items, and finds the exit. It exists to prove that the entire AIOS stack (ChaosGL, ChaosFS, Lua runtime, input subsystem, scheduler) works under sustained real-time load.

**The hybrid:** true 3D polygon world geometry (floors, walls, ceilings at arbitrary heights and slopes, bridges, rooms over rooms) rendered through ChaosGL's triangle pipeline. Enemies, items, and decorations are 2D sprites rendered as camera-facing billboarded quads — the Doom approach. No 3D character models. The aesthetic sits between Doom (1993) and Quake (1996): the architecture is Quake, the characters are Doom.

**The scope:** one level, multiple zones. One weapon. One enemy type. One pickup type (health). A locked door, a key, and an exit trigger. This is a proof of concept — it must be complete and polished within its scope, not sprawling and broken.

**The constraint:** everything goes through ChaosGL. The game has no special access to VRAM, no custom rasterizer, no bypass. It uses the public ChaosGL API exclusively. If ChaosGL can't do it, the game doesn't do it.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                     game.lua (main loop)                      │
│                                                              │
│   ┌─────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐  │
│   │  Input   │  │  Player  │  │  World   │  │  Renderer  │  │
│   │ Handler  │→ │  Update  │→ │  Tick    │→ │ Draw Frame │  │
│   └─────────┘  └──────────┘  └──────────┘  └────────────┘  │
│       ↑                           │               │          │
│       │                           ▼               ▼          │
│   aios.wm             entity_update()   chaos_gl.*           │
│   .poll_event()        collision()       draw calls          │
│                        ai_think()        aios.audio.*        │
│                        portal_cull()                         │
└──────────────────────────────────────────────────────────────┘

App directory: /apps/rip/
  manifest.lua              — app metadata (name, icon, entry, version)
  main.lua                  — entry point, main loop
  lib/                      — private modules (loaded via app-relative require)
    level.lua               — level loader, sector/portal format
    sector.lua              — sector geometry generation
    portal.lua              — portal culling (visibility flood)
    player.lua              — player state, movement, sector traversal
    entities.lua            — entity system (enemies, items, doors)
    ai.lua                  — enemy AI state machine
    render.lua              — 3D scene + sprite + HUD rendering
    collision.lua           — sector-based collision detection
    billboard.lua           — sprite billboard quad generation
    weapons.lua             — weapon logic and hitscan
    hud.lua                 — HUD overlay (health, ammo, weapon sprite)
    assets.lua              — texture/model/sprite loading and caching
    math_util.lua           — angle helpers, lerp, 2D geometry
  data/                     — game assets (textures, levels, sounds)
    textures/               — wall, floor, sprite textures (.png)
    levels/                 — level definition files (.lua)
    sounds/                 — sound effects (.wav)
```

**No KAOS module.** The entire game is Lua. ChaosGL's C rasterizer does the heavy lifting. Lua orchestrates: loads the level, runs portal culling, moves entities, picks sprite frames, submits draw calls. This is the correct split — the inner loops (triangle rasterization, z-buffer, pixel fill) are C with SSE2. The outer loops (game logic, AI, visibility, event handling) are Lua at 30–60 fps.

---

## App Packaging

ChaosRIP follows the AIOS directory-based app packaging convention. It lives on ChaosFS as a self-contained directory with a manifest, entry script, private modules, and bundled assets.

### Manifest

```lua
-- /apps/rip/manifest.lua
return {
    name        = "ChaosRIP",
    version     = "0.1",
    author      = "AIOS",
    icon        = "/apps/rip/data/icon_32.png",
    entry       = "main.lua",
    description = "Doom/Quake-style FPS — portal-based sectors, sprite enemies",
}
```

The game appears in the desktop dock alongside the other AIOS apps. Launching it from the dock spawns a new Lua task running `main.lua`.

### Module Loading

Game modules live in `/apps/rip/lib/`. The Lua runtime's app-relative `require()` resolves these — `require("lib/level")` finds `/apps/rip/lib/level.lua`. No path manipulation needed. Game modules are fully private and don't pollute the system module namespace.

### CPK Distribution

The game can be packaged as a `.cpk` archive (LZ4-compressed) for distribution:

```bash
# Build tool creates a compressed archive from the game directory
python3 tools/cpk_pack.py harddrive/apps/rip/ build/chaosrip.cpk
```

Installed on target via `aios.cpk.install("/path/to/chaosrip.cpk", "/apps/rip")`.

---

## Audio

AIOS now has an audio subsystem (AC97 driver as a KAOS module) with WAV, MP3, and MIDI support. ChaosRIP uses this for sound effects and ambient atmosphere.

### Sound Effects

```lua
-- assets.lua: sound paths (no pre-loading — aios.audio.play() streams from disk)
sounds = {
    shotgun_fire = "/apps/rip/data/sounds/shotgun.wav",
    imp_sight    = "/apps/rip/data/sounds/imp_sight.wav",
    imp_pain     = "/apps/rip/data/sounds/imp_pain.wav",
    imp_death    = "/apps/rip/data/sounds/imp_death.wav",
    pickup       = "/apps/rip/data/sounds/pickup.wav",
    door_open    = "/apps/rip/data/sounds/door.wav",
    player_pain  = "/apps/rip/data/sounds/pain.wav",
}

-- Play a sound effect (fire-and-forget, pass path directly)
aios.audio.play(sounds.shotgun_fire)
```

Sound triggers are placed at the relevant gameplay events: weapon fire, enemy state transitions (sight/pain/death), pickup collection, door activation, and player damage. All sounds are short WAV files — no streaming needed.

**If the AC97 module is not loaded** (no audio hardware in QEMU without `-soundhw ac97`), all `aios.audio.*` calls are no-ops. The game runs identically without sound.

---

## Render Strategy

### Surface Setup

```lua
-- Render at 320x200 (classic Doom resolution).
-- ChaosGL surface is 320x200. Compositor presents it at position on the
-- 1024x768 screen. The pixel art aesthetic is a feature.
local RENDER_W = 320
local RENDER_H = 200

local surface = chaos_gl.surface_create(RENDER_W, RENDER_H, true)  -- has_depth = true
chaos_gl.surface_set_position(surface, 352, 284)   -- centered on 1024x768
chaos_gl.surface_set_zorder(surface, 50)
chaos_gl.surface_set_visible(surface, true)
```

**Why 320×200:** a 320×200 3D surface with z-buffer costs ~380KB (front + back + zbuf). The rasterizer fills 64,000 pixels per frame — roughly 12× fewer than 1024×768. On a software renderer, pixel fill rate is the bottleneck. 320×200 gets us well above 30fps with a full scene. The nearest-neighbour scaling is the retro aesthetic we want.

**Texture format:** ChaosGL now supports PNG and JPEG loading via stb_image, in addition to the original `.raw` format. Game textures use **PNG** — smaller on disk, standard tooling for creation, and the LZ4 compression in CPK archives stacks on top. ChaosGL decodes PNGs to BGRX at load time, so runtime sampling cost is identical to `.raw`.

**Fullscreen presentation:** the surface is 320×200, centered at (352, 284) on the 1024×768 screen. Black border around it. Authentic retro. If software upscaling is desired later, render to 320×200 and blit-scale into a larger surface — purely cosmetic, zero gameplay impact.

### Frame Structure

Each frame:

```
1. chaos_gl.surface_bind(surface)
2. chaos_gl.surface_clear(surface, sky_color)       -- clear + reset z-buffer
3. Draw sky (2D blit — drawn first, behind everything)
4. Run portal culling → visible_sectors list
5. For each visible sector (any order — z-buffer handles overlap):
     Draw sector geometry (walls, floor, ceiling) via draw_model()
6. Draw sprites (billboarded quads for all visible entities, z-tested)
7. Draw weapon sprite (2D blit at bottom-center — no z-test, always on top)
8. Draw HUD (2D text + rects — health, ammo, keys)
9. chaos_gl.surface_present(surface)
10. aios.os.sleep(16)
```

Steps 3, 7–8 are 2D draws (bypass z-buffer). Steps 5–6 are 3D draws (z-tested). ChaosGL supports intermixing 2D and 3D on a depth-enabled surface. The z-buffer means we can draw visible sectors in any order — no need for back-to-front sorting.

---

## Sector / Portal Level Architecture

### Why Portals

ChaosGL has a z-buffer, so we don't need BSP for rendering correctness. But without any spatial culling, we'd submit every triangle in the level every frame — all of them pass through vertex transformation, clipping, and backface culling even when the player is in a corridor staring at a wall.

Portal-based visibility is the natural fit for ChaosGL:

1. Divide the level into **sectors** — convex (or near-convex) rooms, each with their own floor/ceiling heights.
2. Connect sectors through **portals** — openings in walls (doorways, windows, gaps).
3. At runtime, **flood from the player's sector through visible portals**, clipping a screen-space window at each portal. Only sectors reachable through an unclipped portal chain get drawn.
4. The **z-buffer handles draw order** within visible sectors. No BSP sort needed.

This gives us aggressive culling (a corridor level might only draw 4–8 sectors out of 50) while keeping the rendering code simple — each visible sector is just a `draw_model()` call.

### What Portals Enable

Unlike a grid, portal-based sectors give us true arbitrary 3D:

- **Rooms over rooms.** Two sectors can overlap in the XZ plane — a bridge sector at Y=4.0 over a river sector at Y=0.0. The portal system tracks which sector the player is in by sector, not by XZ position.
- **Non-orthogonal walls.** Sector polygons can be any convex shape — angled walls, triangular rooms, hexagonal arenas.
- **Varying heights.** Each sector has its own floor and ceiling height (can be flat or sloped). A staircase is a sequence of sectors with increasing floor height.
- **Windows and ledges.** A portal doesn't have to be floor-to-ceiling. A portal with a restricted vertical range creates a window or a ledge you can look through but not walk through.
- **Overlapping geometry.** A spiral staircase wrapping around a central pillar, with the upper level visible through gaps — all handled by the sector graph and z-buffer.

---

## Sector Definition

### Sector Structure

A sector is a convex polygon extruded vertically between a floor height and a ceiling height. All world geometry is derived from sectors and their connections.

```lua
sector = {
    id          = 1,              -- unique sector ID

    -- Floor plan: ordered list of 2D vertices (XZ plane) defining the
    -- convex polygon, counter-clockwise when viewed from above.
    -- Minimum 3 vertices.
    vertices = {
        { x = 0.0, z = 0.0 },
        { x = 4.0, z = 0.0 },
        { x = 4.0, z = 6.0 },
        { x = 0.0, z = 6.0 },
    },

    -- Height
    floor_h     = 0.0,            -- floor height (Y coordinate)
    ceil_h      = 3.0,            -- ceiling height (Y coordinate)
    floor_slope = nil,            -- optional: { nx, ny, nz, d } plane equation
    ceil_slope  = nil,            -- optional: same (nil = flat)

    -- Textures
    floor_tex   = "floor1",       -- texture name (resolved from asset table)
    ceil_tex    = "ceil1",
    wall_tex    = "brick1",       -- default wall texture for solid edges

    -- Lighting
    light       = 0.8,            -- ambient light level 0.0–1.0

    -- Gameplay
    zone        = "hub",          -- zone name (see Zones section)
    is_door     = false,          -- if true, this sector animates as a door
    trigger_id  = nil,            -- event trigger when player enters
    is_exit     = false,          -- level exit trigger

    -- Edges: one per polygon edge, indexed to match vertex pairs.
    -- edges[i] connects vertices[i] to vertices[i+1] (wrapping).
    -- Each edge is either a solid wall or a portal to another sector.
    edges = {
        { type = "wall",   tex = "brick1" },                    -- solid wall
        { type = "portal", target_sector = 2, tex = nil },      -- open to sector 2
        { type = "wall",   tex = "brick2" },                    -- solid wall
        { type = "portal", target_sector = 5, tex = nil,        -- window
          portal_floor = 1.5, portal_ceil = 2.5 },              -- restricted range
    },

    -- Door state (only if is_door = true)
    door_open       = false,
    door_target_h   = 3.0,        -- ceiling height when fully open
    door_timer      = 0,
    key_required    = nil,         -- "red", "blue", or nil

    -- Generated at load time
    _models         = nil,         -- list of { model, texture, light } per texture group
    _tri_count      = 0,
}
```

### Edge Types

**Wall:** solid edge. Generates a vertical quad (2 triangles) from floor to ceiling, textured, facing inward.

**Portal:** open edge connecting to an adjacent sector.

- **Full portal** (no `portal_floor`/`portal_ceil`): opening spans the full floor-to-ceiling overlap between the two sectors.
- **Restricted portal** (`portal_floor`/`portal_ceil` set): narrower opening. Creates a window or ledge.
- **Step walls:** automatically generated where adjacent sectors have different floor or ceiling heights.

Portal geometry rules — when generating a sector's mesh for a portal edge:

1. **Portal opening:** no wall geometry (this is the hole).
2. **Floor step:** if `target.floor_h > this.floor_h`, vertical wall from `this.floor_h` to `target.floor_h`.
3. **Ceiling step:** if `target.ceil_h < this.ceil_h`, vertical wall from `target.ceil_h` to `this.ceil_h`.
4. **Below-portal wall:** if `portal_floor` is set and higher than the floor overlap, wall from floor to `portal_floor`.
5. **Above-portal wall:** if `portal_ceil` is set and lower than the ceiling overlap, wall from `portal_ceil` to ceiling.

```
Sector A (floor=0, ceil=3)          Sector B (floor=1, ceil=4)

  ceil=3 ────────┐                    ┌──────── ceil=4
                 │    portal         │
                 │  ← opening →      │    ← ceiling step wall (3 to 4)
                 │   (1 to 3)        │
  floor=0 ───────┘                    └──────── floor=1
                     ← floor step wall (0 to 1)
```

---

## Portal Culling

### The Algorithm

Runtime portal-based visibility determination. Runs once per frame before rendering. Produces a set of sector IDs to draw.

Start from the player's current sector (always visible). For each portal edge, project the portal opening onto screen space. If the projected rectangle is non-empty, the target sector is visible — add it and recursively check its portals, clipped to the screen-space rectangle of the portal we came through. This is a flood-fill through the portal graph that progressively narrows the visible "window."

```lua
-- portal.lua

local MAX_PORTAL_DEPTH = 16

function portal_cull(level, player_sector_id, camera)
    local visible = {}

    local function flood(sector_id, screen_window, depth)
        if depth > MAX_PORTAL_DEPTH then return end
        if visible[sector_id] then return end

        visible[sector_id] = true

        local sector = level.sectors[sector_id]
        for i, edge in ipairs(sector.edges) do
            if edge.type == "portal" then
                local target_id = edge.target_sector
                local j = (i % #sector.vertices) + 1
                local v0 = sector.vertices[i]
                local v1 = sector.vertices[j]
                local target = level.sectors[target_id]

                -- Portal vertical range
                local p_floor = edge.portal_floor
                    or math.max(sector.floor_h, target.floor_h)
                local p_ceil = edge.portal_ceil
                    or math.min(sector.ceil_h, target.ceil_h)
                if p_ceil <= p_floor then goto continue end

                -- Project portal quad to screen-space bounding rect
                local portal_rect = project_portal_to_screen(
                    v0.x, v0.z, v1.x, v1.z, p_floor, p_ceil, camera
                )
                if not portal_rect then goto continue end

                -- Clip against the incoming window
                local clipped = clip_rect(portal_rect, screen_window)
                if not clipped then goto continue end

                -- Target sector is visible — recurse with narrowed window
                flood(target_id, clipped, depth + 1)

                ::continue::
            end
        end
    end

    local full_screen = { x0 = 0, y0 = 0, x1 = RENDER_W - 1, y1 = RENDER_H - 1 }
    flood(player_sector_id, full_screen, 0)
    return visible
end
```

### Portal Projection

Project the four corners of a portal quad into screen space and return the bounding rectangle. Corners behind the near plane expand the rect to the screen edge (the portal extends off-screen in that direction).

```lua
function project_portal_to_screen(x0, z0, x1, z1, floor_y, ceil_y, camera)
    local corners = {
        { x = x0, y = floor_y, z = z0 },
        { x = x1, y = floor_y, z = z1 },
        { x = x1, y = ceil_y,  z = z1 },
        { x = x0, y = ceil_y,  z = z0 },
    }

    local min_sx, min_sy = RENDER_W, RENDER_H
    local max_sx, max_sy = 0, 0
    local any_in_front = false

    for _, c in ipairs(corners) do
        local vx, vy, vz = transform_point(c.x, c.y, c.z, camera.view_matrix)

        if vz < -camera.z_near then
            any_in_front = true
            local sx, sy = project_to_screen(vx, vy, vz, camera)
            min_sx = math.min(min_sx, sx)
            min_sy = math.min(min_sy, sy)
            max_sx = math.max(max_sx, sx)
            max_sy = math.max(max_sy, sy)
        else
            -- Behind camera — portal extends off screen in this direction
            any_in_front = true
            min_sx = 0; min_sy = 0
            max_sx = RENDER_W - 1; max_sy = RENDER_H - 1
        end
    end

    if not any_in_front then return nil end

    return {
        x0 = math.max(0, math.floor(min_sx)),
        y0 = math.max(0, math.floor(min_sy)),
        x1 = math.min(RENDER_W - 1, math.ceil(max_sx)),
        y1 = math.min(RENDER_H - 1, math.ceil(max_sy)),
    }
end

function clip_rect(a, b)
    local x0 = math.max(a.x0, b.x0)
    local y0 = math.max(a.y0, b.y0)
    local x1 = math.min(a.x1, b.x1)
    local y1 = math.min(a.y1, b.y1)
    if x0 > x1 or y0 > y1 then return nil end
    return { x0 = x0, y0 = y0, x1 = x1, y1 = y1 }
end
```

### Why This Works with ChaosGL

The portal culler produces a set of visible sector IDs. The renderer calls `chaos_gl.draw_model()` for each visible sector's pre-built geometry. ChaosGL's z-buffer ensures correct pixel overlap — a bridge sector above a ground sector both draw correctly. No sorting needed. The culling just determines WHICH sectors to submit, not the order.

For a corridor-heavy level, portal culling typically eliminates 80–90% of sectors per frame.

---

## Zones

Zones are named groups of sectors within a single level. They serve gameplay and atmosphere — not rendering. Portal culling operates on sectors regardless of zone boundaries.

```lua
-- Defined in the level file
zones = {
    hub = {
        name      = "Central Hub",
        sectors   = { 1, 2, 3, 4, 5 },
        light     = 0.8,
    },
    armory = {
        name      = "The Armory",
        sectors   = { 10, 11, 12, 13 },
        light     = 0.5,
    },
    bridge = {
        name      = "The Crossing",
        sectors   = { 20, 21, 22 },
        light     = 1.0,
    },
    exit_corridor = {
        name      = "Exit",
        sectors   = { 30, 31, 32 },
        light     = 0.3,
    },
}
```

### What Zones Do

**Ambient lighting:** sectors inherit their zone's light level unless overridden. Light entire areas at once, tweak individuals.

**Zone entry triggers:** when the player transitions between zones, fire events — display the zone name on the HUD ("Entering: The Armory"), trigger enemy spawns, change music (future).

**Gameplay grouping:** "kill all enemies in this zone to unlock the door" or "find the switch in the armory" — zone IDs make these queries simple.

**HUD indicator:** current zone name displayed briefly on entering.

```lua
function check_zone_transition(player, level)
    local sector = level.sectors[player.sector_id]
    local new_zone = sector.zone

    if new_zone ~= player.current_zone then
        player.current_zone = new_zone
        hud_show_zone_name(level.zones[new_zone].name, 3000)
    end
end
```

---

## Geometry Generation

### Per-Sector Model Building

At level load, each sector's geometry is converted into ChaosGL models. Runs once per sector (and again if a door changes state).

**Floor polygon:** fan triangulation from vertex 0. For N vertices: N-2 triangles. Normals up. UVs world-space.

**Ceiling polygon:** same, winding reversed. Normals down.

**Walls:** solid edges → vertical quad (2 triangles) from floor to ceiling. Portal edges → step walls and restricted-range walls per the portal geometry rules.

**Texture grouping:** triangles grouped by texture. Each group becomes a separate `draw_model()` call.

```lua
function generate_sector_geometry(sector, level)
    local groups = {}  -- groups[texture_name] = { verts, normals, uvs, faces }

    local verts = sector.vertices
    local nv = #verts

    -- Floor (fan from vertex 0)
    local fg = get_group(groups, sector.floor_tex)
    for i = 2, nv - 1 do
        add_floor_tri(fg, verts[1], verts[i], verts[i + 1], sector.floor_h)
    end

    -- Ceiling (reversed winding)
    local cg = get_group(groups, sector.ceil_tex)
    for i = 2, nv - 1 do
        add_ceil_tri(cg, verts[1], verts[i + 1], verts[i], sector.ceil_h)
    end

    -- Edges
    for i = 1, nv do
        local j = (i % nv) + 1
        local v0, v1 = verts[i], verts[j]
        local edge = sector.edges[i]

        if edge.type == "wall" then
            local wg = get_group(groups, edge.tex or sector.wall_tex)
            add_wall_quad(wg, v0, v1, sector.floor_h, sector.ceil_h)

        elseif edge.type == "portal" then
            local target = level.sectors[edge.target_sector]
            local overlap_floor = math.max(sector.floor_h, target.floor_h)
            local overlap_ceil  = math.min(sector.ceil_h,  target.ceil_h)
            local p_floor = edge.portal_floor or overlap_floor
            local p_ceil  = edge.portal_ceil  or overlap_ceil
            local tex = edge.step_tex or sector.wall_tex

            if p_floor > sector.floor_h then
                add_wall_quad(get_group(groups, tex), v0, v1,
                              sector.floor_h, p_floor)
            end
            if p_ceil < sector.ceil_h then
                add_wall_quad(get_group(groups, tex), v0, v1,
                              p_ceil, sector.ceil_h)
            end
        end
    end

    -- Build ChaosGL models
    local models = {}
    for tex_name, group in pairs(groups) do
        local model = chaos_gl.model_create(#group.verts, #group.faces)
        -- set vertices, normals, uvs, faces ...
        models[#models + 1] = {
            model   = model,
            texture = assets.get_texture(tex_name),
            light   = sector.light,
        }
    end
    return models
end
```

### Triangle Budget

Typical level with 50 sectors:

| Component | Triangles |
|-----------|-----------|
| Floors | ~200 |
| Ceilings | ~200 |
| Walls | ~600 |
| Portal trim | ~200 |
| **Total level** | **~1200** |

With portal culling showing 6–10 sectors, visible triangles per frame: **150–300 world** + sprites. After backface culling: **~100–200 rasterized**. Very comfortable at 320×200.

---

## Sector-Based Collision

### Player Sector Tracking

The player is always "in" exactly one sector. Movement checks portal crossings to transition between sectors. The player's sector — not their XZ position — determines floor height and valid movement.

```lua
function point_in_sector(sector, px, pz)
    local verts = sector.vertices
    local n = #verts
    local sign = nil

    for i = 1, n do
        local j = (i % n) + 1
        local ex = verts[j].x - verts[i].x
        local ez = verts[j].z - verts[i].z
        local cross = ex * (pz - verts[i].z) - ez * (px - verts[i].x)

        if sign == nil then
            sign = (cross >= 0)
        elseif (cross >= 0) ~= sign then
            return false
        end
    end
    return true
end
```

### Movement and Portal Crossing

When the player moves outside their current sector, find which edge was crossed. If it's a portal and the player fits through, transition to the target sector. If it's a wall or impassable portal, slide along it.

```lua
function move_player(player, new_x, new_z, level)
    local sector = level.sectors[player.sector_id]

    -- Still inside current sector
    if point_in_sector(sector, new_x, new_z) then
        player.x = new_x
        player.z = new_z
        player.y = get_floor_at(sector, new_x, new_z)
        return
    end

    -- Find the crossed edge
    for i, edge in ipairs(sector.edges) do
        local j = (i % #sector.vertices) + 1
        local v0 = sector.vertices[i]
        local v1 = sector.vertices[j]

        if line_segments_intersect(player.x, player.z, new_x, new_z,
                                   v0.x, v0.z, v1.x, v1.z) then
            if edge.type == "portal" then
                local target = level.sectors[edge.target_sector]
                local p_floor = edge.portal_floor
                    or math.max(sector.floor_h, target.floor_h)
                local p_ceil = edge.portal_ceil
                    or math.min(sector.ceil_h, target.ceil_h)

                local step_up = p_floor - player.y
                if step_up > 0.5 then
                    return slide_along_edge(player, new_x, new_z, v0, v1)
                end

                local clearance = p_ceil - math.max(player.y, p_floor)
                if clearance < player.height then
                    return slide_along_edge(player, new_x, new_z, v0, v1)
                end

                if target.is_door and not target.door_open then
                    if target.key_required and
                       not player["has_key_" .. target.key_required] then
                        return slide_along_edge(player, new_x, new_z, v0, v1)
                    end
                end

                -- Transition
                player.sector_id = edge.target_sector
                player.x = new_x
                player.z = new_z
                player.y = get_floor_at(target, new_x, new_z)
                return
            else
                return slide_along_edge(player, new_x, new_z, v0, v1)
            end
        end
    end
end
```

### Wall Sliding

Project remaining movement onto the wall direction for smooth sliding.

```lua
function slide_along_edge(player, new_x, new_z, v0, v1)
    local wx = v1.x - v0.x
    local wz = v1.z - v0.z
    local wlen = math.sqrt(wx * wx + wz * wz)
    if wlen < 0.001 then return end
    wx, wz = wx / wlen, wz / wlen

    local mx = new_x - player.x
    local mz = new_z - player.z
    local dot = mx * wx + mz * wz

    local slide_x = player.x + wx * dot
    local slide_z = player.z + wz * dot

    -- Push slightly away from wall
    slide_x = slide_x + (-wz) * player.radius * 0.1
    slide_z = slide_z + ( wx) * player.radius * 0.1

    local sector = level.sectors[player.sector_id]
    if point_in_sector(sector, slide_x, slide_z) then
        player.x = slide_x
        player.z = slide_z
    end
end
```

### Floor Height with Slopes

```lua
function get_floor_at(sector, x, z)
    if sector.floor_slope then
        local s = sector.floor_slope
        return (s.d - s.nx * x - s.nz * z) / s.ny
    end
    return sector.floor_h
end
```

---

## Sprites and Billboards

Every sprite (enemies, items, decorations) is a camera-facing textured quad — two triangles through the 3D pipeline with the sprite shader. The z-buffer handles occlusion against world geometry.

### Billboard Generation

```lua
function make_billboard(pos_x, pos_y, pos_z, w, h, camera)
    local rx, rz = camera.right_x, camera.right_z
    local half_w = w * 0.5

    local lx = pos_x - rx * half_w
    local lz = pos_z - rz * half_w
    local rx2 = pos_x + rx * half_w
    local rz2 = pos_z + rz * half_w

    return {
        { pos = {lx,  pos_y,     lz},  uv = {0, 1} },
        { pos = {rx2, pos_y,     rz2}, uv = {1, 1} },
        { pos = {rx2, pos_y + h, rz2}, uv = {1, 0} },
        { pos = {lx,  pos_y,     lz},  uv = {0, 1} },
        { pos = {rx2, pos_y + h, rz2}, uv = {1, 0} },
        { pos = {lx,  pos_y + h, lz},  uv = {0, 0} },
    }
end
```

Y-axis billboard only — sprites stay upright, rotate horizontally to face camera.

### Sprite Rotation Frames

8 directions per enemy (Doom-style). Frame selected by angle between camera direction and entity facing:

```lua
function get_sprite_frame(entity_angle, cam_x, cam_z, ent_x, ent_z)
    local dx = cam_x - ent_x
    local dz = cam_z - ent_z
    local to_cam = math.atan(dx, dz)
    local rel = (to_cam - entity_angle) % (2 * math.pi)
    return math.floor((rel + math.pi / 8) / (math.pi / 4)) % 8
end
```

### Sprite Sheet: 512×512

```
8 columns (rotation 0–7) × 8 rows (animation states)
Each cell: 64×64 pixels

Row 0: idle       Row 4: attack 2
Row 1: walk 1     Row 5: pain
Row 2: walk 2     Row 6: death 1
Row 3: attack 1   Row 7: death 2
```

### Sprite Shader (C-side)

Custom fragment shader: sample texture, discard magenta (0x00FF00FF) pixels, apply flat lighting. Vertex shader remaps UVs to sprite sheet sub-rect.

```c
/* renderer/shaders.c — ~40 lines */

typedef struct {
    chaos_gl_texture_t* texture;
    uint32_t key_color;
    float    light;
    float    u0, v0, u1, v1;   /* UV sub-rect in sprite sheet */
} sprite_uniforms_t;

static gl_frag_out_t frag_sprite(gl_fragment_in_t in, void* uniforms) {
    sprite_uniforms_t* u = (sprite_uniforms_t*)uniforms;
    uint32_t texel = chaos_gl_tex_sample(u->texture, in.uv.x, in.uv.y);

    if (texel == u->key_color) return GL_DISCARD;

    uint8_t b = (texel >>  0) & 0xFF;
    uint8_t g = (texel >>  8) & 0xFF;
    uint8_t r = (texel >> 16) & 0xFF;
    float l = u->light;

    return GL_COLOR(
        ((uint32_t)(b * l)) |
        ((uint32_t)(g * l) << 8) |
        ((uint32_t)(r * l) << 16)
    );
}
```

---

## Entity System

```lua
entity = {
    type        = "imp",          -- "imp", "health", "ammo", "key_red", "key_blue", "barrel"
    sector_id   = 5,              -- current sector
    x = 0, z = 0, y = 0,         -- position (y = floor height)
    angle       = 0.0,            -- facing direction
    alive       = true,
    health      = 30,
    active      = false,          -- woken up

    ai_state    = "idle",         -- idle/chase/attack/pain/dying/dead
    ai_timer    = 0,
    attack_cooldown = 0,
    target_x = 0, target_z = 0,

    anim_state  = "idle",
    anim_frame  = 0,
    anim_timer  = 0,

    radius      = 0.4,
    height      = 1.5,
    sprite_w    = 1.2,
    sprite_h    = 1.5,
}
```

Pickups: player touches entity (radius overlap) → collect, entity removed. Health +25, ammo +10, keys set flag.

---

## Player

```lua
player = {
    x = 0, y = 0, z = 0,
    angle       = 0.0,
    sector_id   = 1,
    current_zone = "hub",

    health = 100, max_health = 100,
    ammo = 50, max_ammo = 99,
    has_key_red = false, has_key_blue = false,
    alive = true,

    weapon_state = "idle",
    weapon_timer = 0,

    eye_height = 1.6,
    move_speed = 4.0,
    turn_speed = 3.0,
    mouse_sens = 0.003,
    radius     = 0.3,
    height     = 1.8,
}
```

### Movement

```lua
function player_update(p, dt, input, level)
    if not p.alive then return end

    -- Turning
    if input.turn_left  then p.angle = p.angle + p.turn_speed * dt / 1000 end
    if input.turn_right then p.angle = p.angle - p.turn_speed * dt / 1000 end
    p.angle = p.angle + input.mouse_dx * p.mouse_sens
    p.angle = p.angle % (2 * math.pi)

    -- Direction vectors
    local fx = math.sin(p.angle)
    local fz = math.cos(p.angle)
    local rx, rz = fz, -fx

    -- Accumulate movement
    local mx, mz = 0, 0
    if input.forward  then mx = mx + fx; mz = mz + fz end
    if input.backward then mx = mx - fx; mz = mz - fz end
    if input.strafe_l then mx = mx - rx; mz = mz - rz end
    if input.strafe_r then mx = mx + rx; mz = mz + rz end

    local len = math.sqrt(mx * mx + mz * mz)
    if len > 0 then
        local speed = p.move_speed * dt / 1000
        mx = mx / len * speed
        mz = mz / len * speed
        move_player(p, p.x + mx, p.z + mz, level)
    end
end
```

### Camera

```lua
function set_camera_from_player(player)
    local eye_y = player.y + player.eye_height
    local dx = math.sin(player.angle)
    local dz = math.cos(player.angle)

    chaos_gl.set_camera(
        player.x, eye_y, player.z,
        player.x + dx, eye_y, player.z + dz,
        0, 1, 0
    )
    chaos_gl.set_perspective(60, 0, 0.1, 50.0)

    camera = {
        eye_x = player.x, eye_y = eye_y, eye_z = player.z,
        right_x = dz, right_z = -dx,
        forward_x = dx, forward_z = dz,
        z_near = 0.1,
        -- view_matrix computed internally by ChaosGL; we replicate for portal projection
    }
end
```

---

## Enemy AI

Six states: **idle → chase → attack → pain → dying → dead**.

```
IDLE:   stand still. If player enters line of sight → CHASE.
CHASE:  move toward last known player position. If LOS, update target.
        If close enough and LOS → ATTACK. If lost LOS for 3s → IDLE.
ATTACK: face player, fire hitscan (random hit chance based on distance).
        After animation → CHASE with 1–1.5s cooldown.
PAIN:   200ms stun (50% chance on hit). Then → CHASE.
DYING:  death animation playing. When done → DEAD.
DEAD:   corpse sprite on floor. Permanent.
```

### Line of Sight

Sector-based raycasting: step along the line from entity to player, tracking which sector each sample is in. If a step exits the current sector and no portal exists on that edge, LOS is blocked.

```lua
function has_line_of_sight(ent, player, level)
    local dist = math.sqrt((ent.x - player.x)^2 + (ent.z - player.z)^2)
    if dist > 20.0 then return false end

    local dx = player.x - ent.x
    local dz = player.z - ent.z
    local current_sector = ent.sector_id
    local steps = math.ceil(dist * 4)
    local px, pz = ent.x, ent.z

    for i = 1, steps do
        local t = i / steps
        local sx = ent.x + dx * t
        local sz = ent.z + dz * t
        local sector = level.sectors[current_sector]

        if not point_in_sector(sector, sx, sz) then
            local found = false
            for ei, edge in ipairs(sector.edges) do
                if edge.type == "portal" then
                    local ej = (ei % #sector.vertices) + 1
                    if line_segments_intersect(px, pz, sx, sz,
                        sector.vertices[ei].x, sector.vertices[ei].z,
                        sector.vertices[ej].x, sector.vertices[ej].z) then
                        -- Check vertical clearance
                        local target = level.sectors[edge.target_sector]
                        local p_floor = edge.portal_floor
                            or math.max(sector.floor_h, target.floor_h)
                        local p_ceil = edge.portal_ceil
                            or math.min(sector.ceil_h, target.ceil_h)
                        local eye_h = math.max(ent.y, player.y) + 1.6
                        if eye_h > p_floor and eye_h < p_ceil then
                            current_sector = edge.target_sector
                            found = true
                            break
                        end
                    end
                end
            end
            if not found then return false end
        end
        px, pz = sx, sz
    end

    return (current_sector == player.sector_id)
end
```

---

## Weapons

### Shotgun (Single Weapon)

Hitscan, 7-pellet spread. Each pellet: random angle within ±0.08 radians, ray tested against entity hitboxes, sector-aware wall blocking.

```lua
WEAPON_SHOTGUN = {
    damage = 10, pellets = 7, spread = 0.08,
    range = 30.0, fire_time = 200, cooldown_time = 600, ammo_cost = 1,
}

function weapon_fire(player, weapon, entities, level)
    if player.ammo < weapon.ammo_cost then return end
    player.ammo = player.ammo - weapon.ammo_cost
    player.weapon_state = "firing"
    player.weapon_timer = weapon.fire_time

    for i = 1, weapon.pellets do
        local angle = player.angle + (math.random() - 0.5) * weapon.spread * 2
        local dx = math.sin(angle)
        local dz = math.cos(angle)

        local hit_ent, hit_dist = nil, weapon.range
        for _, ent in ipairs(entities) do
            if ent.alive and ent.type == "imp" then
                local d = ray_circle_intersect(
                    player.x, player.z, dx, dz, ent.x, ent.z, ent.radius)
                if d and d < hit_dist then
                    if not ray_blocked(player, dx, dz, d, level) then
                        hit_ent = ent
                        hit_dist = d
                    end
                end
            end
        end

        if hit_ent then entity_damage(hit_ent, weapon.damage) end
    end
end
```

`ray_blocked()` uses the same sector-walk approach as LOS — step along the ray through portals, return true if a solid wall is hit before reaching `max_dist`.

---

## Doors

A door is a sector with `is_door = true`. When closed, `ceil_h = floor_h` (zero clearance — portals impassable). When open, `ceil_h = door_target_h`. Animates over 500ms.

```lua
function door_update(level, dt)
    for id, sector in pairs(level.sectors) do
        if sector.is_door then
            if sector.door_open and sector.door_timer < 500 then
                sector.door_timer = math.min(sector.door_timer + dt, 500)
                local t = sector.door_timer / 500
                sector.ceil_h = sector.floor_h +
                    (sector.door_target_h - sector.floor_h) * t
                level.geometry_dirty[id] = true
            elseif not sector.door_open and sector.door_timer > 0 then
                sector.door_timer = math.max(sector.door_timer - dt, 0)
                local t = sector.door_timer / 500
                sector.ceil_h = sector.floor_h +
                    (sector.door_target_h - sector.floor_h) * t
                level.geometry_dirty[id] = true
            end
        end
    end
end

function door_activate(player, level)
    local sector = level.sectors[player.sector_id]
    for _, edge in ipairs(sector.edges) do
        if edge.type == "portal" then
            local target = level.sectors[edge.target_sector]
            if target.is_door then
                if target.key_required and
                   not player["has_key_" .. target.key_required] then
                    return
                end
                target.door_open = not target.door_open
                return
            end
        end
    end
end
```

When `geometry_dirty[id]` is set, only that sector's model is rebuilt — not the whole level.

---

## HUD

Bottom 40 pixels of the 320×200 surface. 3D viewport = top 160 pixels.

ChaosGL now has TrueType font rendering (stb_truetype, Inter font) alongside the original Claude Mono 8×16 bitmap. The HUD uses **TrueType at small sizes** for a cleaner look — `chaos_gl.text_ttf()` for the HUD labels, or falls back to bitmap `chaos_gl.text()` if TTF performance is too costly at 320×200 (profile both). At this resolution, the bitmap font's pixel-perfect rendering may actually look better.

```lua
local HUD_Y = 160

function hud_draw(player, dt)
    chaos_gl.rect(0, HUD_Y, 320, 40, CHAOS_GL_RGB(32, 32, 32))

    -- Health
    local hc = player.health > 50
        and CHAOS_GL_RGB(50, 200, 50) or CHAOS_GL_RGB(200, 50, 50)
    chaos_gl.text(8, HUD_Y + 4,  "HEALTH", CHAOS_GL_RGB(150, 150, 150), 0, 0)
    chaos_gl.text(8, HUD_Y + 20, tostring(player.health), hc, 0, 0)

    -- Ammo
    chaos_gl.text(250, HUD_Y + 4,  "AMMO", CHAOS_GL_RGB(150, 150, 150), 0, 0)
    chaos_gl.text(250, HUD_Y + 20, tostring(player.ammo),
                  CHAOS_GL_RGB(200, 200, 50), 0, 0)

    -- Keys
    if player.has_key_red then
        chaos_gl.rect(120, HUD_Y + 8, 12, 12, CHAOS_GL_RGB(255, 0, 0))
    end
    if player.has_key_blue then
        chaos_gl.rect(136, HUD_Y + 8, 12, 12, CHAOS_GL_RGB(0, 0, 255))
    end

    -- Weapon sprite
    local wtex = player.weapon_state == "firing"
        and assets.shotgun_fire or assets.shotgun_idle
    chaos_gl.blit_keyed(120, HUD_Y - 40, 80, 80, wtex, 0x00FF00FF)

    -- Zone name (fading)
    if zone_name_timer > 0 then
        zone_name_timer = zone_name_timer - dt
        local tw = chaos_gl.text_width(zone_name_text)
        chaos_gl.text((320 - tw) / 2, 70, zone_name_text,
                      CHAOS_GL_RGB(255, 200, 50), 0, 0)
    end
end
```

---

## Input Mapping

AIOS apps normally receive input through the C-level WM registry (`aios.wm.poll_event(surface)`), which handles hit-testing, coordinate translation, and focus management. ChaosRIP registers its surface with the WM so it appears in the dock and responds to Alt+Tab, but polls events through the WM's per-window event queue:

```lua
-- Register with WM (appears in dock, receives routed events)
aios.wm.register(surface, "ChaosRIP", "/apps/rip/data/icon_32.png")

-- In-game input loop: poll WM-routed events
local event = aios.wm.poll_event(surface)
```

When the game window has focus, keyboard and mouse events are routed to it by the WM. Mouse deltas for FPS-style turning require raw mode.

```lua
-- Scancodes (PS/2 set 1, E0-prefixed keys at 128+)
local KEY_W = 0x11;  local KEY_A = 0x1E;  local KEY_S = 0x1F;  local KEY_D = 0x20
local KEY_E = 0x12;  local KEY_SPACE = 0x39;  local KEY_ESC = 0x01
local KEY_LEFT = 128 + 0x4B;  local KEY_RIGHT = 128 + 0x4D

-- WASD = move, arrows = turn, mouse = turn, space/LMB = fire, E = use, ESC = quit
```

Mouse uses raw mode deltas for FPS-style turning.

---

## Main Loop

```lua
-- /apps/rip/main.lua (entry point)

local Level     = require("lib/level")
local Player    = require("lib/player")
local Entities  = require("lib/entities")
local Portal    = require("lib/portal")
local Render    = require("lib/render")
local HUD       = require("lib/hud")
local Assets    = require("lib/assets")
local Weapons   = require("lib/weapons")

local RENDER_W, RENDER_H = 320, 200

local surface = chaos_gl.surface_create(RENDER_W, RENDER_H, true)
chaos_gl.surface_set_position(surface, 352, 284)
chaos_gl.surface_set_zorder(surface, 50)
chaos_gl.surface_set_visible(surface, true)

-- Register with WM (dock icon, Alt+Tab, focus management)
aios.wm.register(surface, "ChaosRIP", "/apps/rip/data/icon_32.png")

Assets.load_all()
local level    = Level.load("/apps/rip/data/levels/e1m1.lua")
local player   = Player.new(level)
local entities = Entities.spawn_all(level)
local game_state = "playing"
local last_time  = aios.os.millis()

local running = true
while running do
    local now = aios.os.millis()
    local dt  = math.min(now - last_time, 50)
    last_time = now

    running = handle_input()

    if game_state == "playing" then
        if input.use_pressed then door_activate(player, level) end
        if input.fire_pressed and player.weapon_state == "idle" then
            Weapons.fire(player, WEAPON_SHOTGUN, entities, level)
            aios.audio.play(Assets.sounds.shotgun_fire)
        end

        Player.update(player, dt, input, level)
        Weapons.update(player, dt)
        Entities.update(entities, player, level, dt)
        Level.update_doors(level, dt)
        check_zone_transition(player, level)

        for id in pairs(level.geometry_dirty) do
            Render.rebuild_sector(level, id)
        end
        level.geometry_dirty = {}

        local sec = level.sectors[player.sector_id]
        if sec.is_exit then game_state = "won" end
        if not player.alive then game_state = "dead" end
    end

    set_camera_from_player(player)
    local visible = Portal.cull(level, player.sector_id, camera)

    chaos_gl.surface_bind(surface)
    chaos_gl.surface_clear(surface, CHAOS_GL_RGB(40, 40, 60))
    Render.draw_sky()
    Render.draw_sectors(level, visible)
    Render.draw_entities(entities, visible, player, camera)
    HUD.draw(player, dt)

    if game_state == "dead" then
        chaos_gl.text(100, 80, "YOU DIED", CHAOS_GL_RGB(255, 0, 0), 0, 0)
    elseif game_state == "won" then
        chaos_gl.text(80, 80, "LEVEL COMPLETE", CHAOS_GL_RGB(0, 255, 0), 0, 0)
    end

    chaos_gl.surface_present(surface)
    aios.os.sleep(16)
end

Assets.free_all()
aios.wm.unregister(surface)
chaos_gl.surface_destroy(surface)
```

---

## Level File Format

Levels are Lua tables on ChaosFS defining sectors, edges, zones, entity spawns, and textures. See the Sector Definition section for the full sector schema.

```lua
-- /apps/rip/data/levels/e1m1.lua
return {
    name = "Facility",
    spawn_sector = 1, spawn_x = 2.0, spawn_z = 3.0, spawn_angle = 0,

    zones = {
        hub            = { name = "Central Hub",   sectors = {1,2,3,4,5},   light = 0.8 },
        armory         = { name = "The Armory",    sectors = {10,11,12,13}, light = 0.5 },
        bridge         = { name = "The Crossing",  sectors = {20,21,22},    light = 1.0 },
        exit_corridor  = { name = "Exit",          sectors = {30,31,32},    light = 0.3 },
    },

    sectors = {
        [1] = {
            vertices  = { {x=0,z=0}, {x=6,z=0}, {x=6,z=8}, {x=0,z=8} },
            floor_h = 0.0, ceil_h = 3.5,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick1",
            zone = "hub",
            edges = {
                { type = "wall", tex = "brick1" },
                { type = "portal", target_sector = 2 },
                { type = "wall", tex = "brick2" },
                { type = "portal", target_sector = 5 },
            },
        },
        -- Bridge (overlaps ground in XZ — rooms over rooms)
        [20] = {
            vertices  = { {x=12,z=4}, {x=20,z=4}, {x=20,z=6}, {x=12,z=6} },
            floor_h = 4.0, ceil_h = 6.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            zone = "bridge",
            edges = {
                { type = "wall" },
                { type = "portal", target_sector = 21 },
                { type = "wall" },
                { type = "portal", target_sector = 10, portal_floor = 4.0, portal_ceil = 6.0 },
            },
        },
        -- Door sector
        [15] = {
            vertices  = { {x=14,z=9}, {x=16,z=9}, {x=16,z=10}, {x=14,z=10} },
            floor_h = 0.0, ceil_h = 0.0, door_target_h = 3.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "door1",
            zone = "armory", is_door = true, key_required = "red",
            edges = {
                { type = "portal", target_sector = 14 },
                { type = "wall" },
                { type = "portal", target_sector = 16 },
                { type = "wall" },
            },
        },
        -- ... remaining sectors ...
    },

    entities = {
        { type = "imp",     sector = 10, x = 11, z = 5, angle = 180 },
        { type = "imp",     sector = 11, x = 15, z = 7, angle = 90  },
        { type = "health",  sector = 3,  x = 8,  z = 6 },
        { type = "ammo",    sector = 5,  x = 3,  z = 12 },
        { type = "key_red", sector = 22, x = 19, z = 5 },
    },

    textures = {
        brick1 = "/apps/rip/data/textures/brick1.png",
        brick2 = "/apps/rip/data/textures/brick2.png",
        metal1 = "/apps/rip/data/textures/metal1.png",
        floor1 = "/apps/rip/data/textures/floor1.png",
        ceil1  = "/apps/rip/data/textures/ceil1.png",
        door1  = "/apps/rip/data/textures/door1.png",
    },
}
```

---

## Asset Manifest

### Textures

| Path | Size | Usage |
|------|------|-------|
| `/apps/rip/data/textures/brick1.png` | 128×128 | Primary wall |
| `/apps/rip/data/textures/brick2.png` | 128×128 | Alternate wall |
| `/apps/rip/data/textures/metal1.png` | 128×128 | Metal (bridge, tech) |
| `/apps/rip/data/textures/floor1.png` | 128×128 | Stone floor |
| `/apps/rip/data/textures/ceil1.png` | 128×128 | Ceiling |
| `/apps/rip/data/textures/door1.png` | 128×128 | Door |
| `/apps/rip/data/textures/sky1.png` | 320×160 | Sky backdrop |
| `/apps/rip/data/textures/imp.png` | 512×512 | Imp sprite sheet |
| `/apps/rip/data/textures/health.png` | 64×64 | Health pickup |
| `/apps/rip/data/textures/ammo.png` | 64×64 | Ammo pickup |
| `/apps/rip/data/textures/key_red.png` | 64×64 | Red key |
| `/apps/rip/data/textures/key_blue.png` | 64×64 | Blue key |
| `/apps/rip/data/textures/shotgun_idle.png` | 128×128 | Weapon idle |
| `/apps/rip/data/textures/shotgun_fire.png` | 128×128 | Weapon fire |
| `/apps/rip/data/textures/barrel.png` | 64×64 | Barrel decoration |

PNG on disk is significantly smaller than the old `.raw` format (~4× compression for typical game art). Runtime memory is identical — ChaosGL decodes to BGRX at load time. Total decoded texture memory: ~1.7 MB.

### Sounds

| Path | Format | Usage |
|------|--------|-------|
| `/apps/rip/data/sounds/shotgun.wav` | WAV | Shotgun fire |
| `/apps/rip/data/sounds/imp_sight.wav` | WAV | Imp spots player |
| `/apps/rip/data/sounds/imp_pain.wav` | WAV | Imp takes damage |
| `/apps/rip/data/sounds/imp_death.wav` | WAV | Imp dies |
| `/apps/rip/data/sounds/pickup.wav` | WAV | Item collected |
| `/apps/rip/data/sounds/door.wav` | WAV | Door open/close |
| `/apps/rip/data/sounds/pain.wav` | WAV | Player takes damage |

Short clips (~0.5–2s each). Total sound data on disk: ~500 KB.

### App Metadata

| Path | Contents |
|------|----------|
| `/apps/rip/manifest.lua` | App name, version, icon, entry point |
| `/apps/rip/data/icon_32.png` | Dock icon (32×32 PNG) |
| `/apps/rip/data/levels/e1m1.lua` | Level 1 definition |

Total texture memory: ~1.7 MB. Total disk footprint (PNG + WAV + Lua): ~3 MB (vs ~8 MB with `.raw` textures).

---

## Memory Budget

| Region | Size |
|--------|------|
| Render surface (320×200, front+back+zbuf) | ~380 KB |
| Textures (all loaded, decoded to BGRX) | ~1.7 MB |
| Sector geometry models (all sectors) | ~200 KB |
| Sound effects (WAV decoded) | ~500 KB |
| Entity state + portal culling | ~50 KB |
| Lua VM baseline + game code | ~280 KB |
| **Total game** | **~3.1 MB** |

System total with OS kernel (~10 MB) + ChaosGL (~3 MB) + desktop apps: ~20 MB. 256 MB RAM — plenty.

---

## New C Code Required

| Addition | Lines | Location |
|----------|-------|----------|
| Sprite shader (frag discard + UV sub-rect + flat light) | ~40 | `renderer/shaders.c` + `shaders.h` |
| Runtime model construction API (create, set_vertex, etc.) | ~60 | `renderer/model.c` + `model.h` |
| Model API + sprite shader Lua bindings | ~80 | `kernel/lua/lua_chaosgl.c` |
| Mouse raw mode Lua binding | ~15 | `kernel/lua/lua_aios_input.c` |
| **Total** | **~195** | |

**Note:** `blit_keyed` already accepts texture handles — no new blit function needed. HUD weapon sprites use existing `chaos_gl.blit_keyed(x, y, w, h, tex_handle, key_color)`.

## Asset Generation

All game textures and sounds are generated procedurally in `tools/populate_fs.py` during build. This includes:
- 16 PNG textures (walls, floors, sky, sprite sheets, pickups, weapons, icon)
- 7 WAV sound effects (synthesized tones and noise)

Assets can be replaced with hand-crafted art later without code changes.

---

## Summary

| Property | Value |
|----------|-------|
| Name | ChaosRIP |
| Type | First-person shooter (Doom/Quake hybrid) |
| Scope | One level, multiple zones, proof of concept |
| Packaging | Directory-based app (`/apps/rip/`) with `manifest.lua`, distributable as `.cpk` |
| Language | Lua (game logic) + C (sprite shader, model API) |
| Render resolution | 320×200, centered on 1024×768 |
| Level architecture | Portal-based sectors (Build/Quake style) |
| Visibility culling | Runtime portal flood with screen-space window clipping |
| World geometry | Convex sector polygons, arbitrary heights, slopes, overlapping rooms |
| Sprite system | Y-axis billboarded quads, 8 rotation frames, key-colour discard shader |
| Collision | Sector-based: point-in-polygon, portal crossing, wall sliding |
| Zones | Named sector groups for lighting, triggers, HUD display |
| Enemy AI | 6-state: idle → chase → attack → pain → dying → dead |
| Weapon | Shotgun (hitscan, 7-pellet spread, sector-aware ray blocking) |
| HUD | Health, ammo, keys, weapon sprite, zone name (TTF or bitmap font) |
| Audio | WAV sound effects via AC97 KAOS module (graceful no-op if unavailable) |
| Texture format | PNG (stb_image decode at load, BGRX in memory) |
| Font | Inter TTF (stb_truetype) for HUD, Claude Mono bitmap fallback |
| Input | WM-routed events via `aios.wm.poll_event()`, mouse raw mode for FPS turning |
| Target FPS | 30+ at 320×200 |
| Typical visible tris | 150–300 (portal culled from ~1200 total) |
| Texture memory | ~1.7 MB (decoded) |
| Total game memory | ~3.1 MB |
| Disk footprint | ~3 MB (PNG + WAV + Lua) |
| New C code | ~170 lines |

**Do not add scope.** One level. One weapon. One enemy type. One key colour. Get it running, get it solid, get it at 30fps. Everything else is iteration.
