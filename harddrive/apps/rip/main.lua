-- ChaosRIP — Doom/Quake-style FPS for AIOS
-- Entry point

local Level    = require("lib/level")
local Player   = require("lib/player")
local Entities = require("lib/entities")
local Portal   = require("lib/portal")
local Render   = require("lib/render")
local HUD      = require("lib/hud")
local Assets   = require("lib/assets")
local Weapons  = require("lib/weapons")
local mu       = require("lib/math_util")

local RENDER_W, RENDER_H = 320, 200

-- Scancodes
local KEY_W = 0x11;  local KEY_A = 0x1E;  local KEY_S = 0x1F;  local KEY_D = 0x20
local KEY_E = 0x12;  local KEY_SPACE = 0x39;  local KEY_ESC = 0x01
local KEY_LEFT = 128 + 0x4B;  local KEY_RIGHT = 128 + 0x4D

-- Create render surface
local surface = chaos_gl.surface_create(RENDER_W, RENDER_H, true)
chaos_gl.surface_set_position(surface, 352, 284)
chaos_gl.surface_set_zorder(surface, 50)
chaos_gl.surface_set_visible(surface, true)

-- Register with WM
aios.wm.register(surface, "ChaosRIP", "/apps/rip/data/icon_32.png")

-- Mouse turning disabled (QEMU grab required); using A/D keyboard turning
-- aios.input.set_raw_mode(true)

-- Load game
aios.debug.print("[rip] loading level...\n")
local level    = Level.load("/apps/rip/data/levels/e1m1.lua")
if not level then
    aios.debug.print("[rip] FATAL: level failed to load\n")
    aios.wm.unregister(surface)
    chaos_gl.surface_destroy(surface)
    aios.input.set_raw_mode(false)
    return
end
aios.debug.print("[rip] level loaded, sectors: " .. tostring(level.sectors ~= nil) .. "\n")
local player   = Player.new(level)
local entities = Entities.spawn_all(level)
local game_state = "playing"
local last_time  = aios.os.millis()

-- Input state
local keys_down = {}

local running = true
while running do
    local now = aios.os.millis()
    local dt  = math.min(now - last_time, 50)
    last_time = now

    -- Poll input
    local input = {
        forward = false, backward = false,
        strafe_l = false, strafe_r = false,
        turn_left = false, turn_right = false,
        fire_pressed = false, use_pressed = false,
        mouse_dx = 0,
    }

    input.mouse_dx = 0

    -- Poll WM events
    while true do
        local event = aios.wm.poll_event(surface)
        if not event then break end

        if event.type == EVENT_KEY_DOWN then
            keys_down[event.key] = true
            if event.key == KEY_ESC then running = false end
            if event.key == KEY_SPACE then input.fire_pressed = true end
            if event.key == KEY_E then input.use_pressed = true end
        elseif event.type == EVENT_KEY_UP then
            keys_down[event.key] = false
        elseif event.type == EVENT_MOUSE_DOWN then
            if event.button == 1 then input.fire_pressed = true end
        end
    end

    -- Continuous key state
    input.forward  = keys_down[KEY_W] or false
    input.backward = keys_down[KEY_S] or false
    input.turn_left  = keys_down[KEY_A] or keys_down[KEY_LEFT] or false
    input.turn_right = keys_down[KEY_D] or keys_down[KEY_RIGHT] or false

    -- Game logic
    if game_state == "playing" then
        if input.use_pressed then Level.door_activate(player, level) end
        if input.fire_pressed and player.weapon_state == "idle" then
            Weapons.fire(player, Weapons.SHOTGUN, entities, level)
            Assets.play_sound("shotgun_fire")
        end

        Player.update(player, dt, input, level)
        if input.turn_left or input.turn_right then
            aios.debug.print(string.format("[rip] a=%.4f sin=%.4f cos=%.4f dt=%d\n",
                player.angle, math.sin(player.angle), math.cos(player.angle), dt))
        end
        Weapons.update(player, dt)
        Entities.update(entities, player, level, dt)
        Level.update_doors(level, dt)
        HUD.check_zone_transition(player, level)

        -- Rebuild dirty sector geometry
        for id in pairs(level.geometry_dirty) do
            Render.rebuild_sector(level, id)
        end
        level.geometry_dirty = {}

        -- Check win/lose
        local sec = level.sectors[player.sector_id]
        if sec and sec.is_exit then game_state = "won" end
        if not player.alive then game_state = "dead" end
    end

    -- Render: bind surface FIRST so set_camera targets the right surface
    chaos_gl.surface_bind(surface)
    -- Debug: encode angle in sky color (smooth = angle is fine, jump = angle bug)
    local _sky_r = math.floor(math.abs(math.sin(player.angle)) * 60)
    local _sky_b = math.floor(math.abs(math.cos(player.angle)) * 60)
    chaos_gl.surface_clear(surface, mu.CHAOS_GL_RGB(_sky_r, 40, _sky_b + 30))
    local camera = Player.set_camera(player)
    local visible = Portal.cull(level, player.sector_id, camera)
    Render.draw_sky()
    Render.draw_sectors(level, visible)
    Render.draw_entities(entities, visible, player, camera, level)
    HUD.draw(player, dt)

    if game_state == "dead" then
        chaos_gl.text(100, 80, "YOU DIED", mu.CHAOS_GL_RGB(255, 0, 0), 0, 0)
    elseif game_state == "won" then
        chaos_gl.text(80, 80, "LEVEL COMPLETE", mu.CHAOS_GL_RGB(0, 255, 0), 0, 0)
    end

    chaos_gl.surface_present(surface)
    aios.os.sleep(16)
end

-- Cleanup
Render.cleanup()
Level.free(level)
Assets.free_all()
aios.wm.unregister(surface)
chaos_gl.surface_destroy(surface)
