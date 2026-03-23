local M = {}
local mu = require("lib/math_util")
local Assets = require("lib/assets")

local HUD_Y = 160
local zone_name_text = ""
local zone_name_timer = 0

function M.show_zone_name(name, duration)
    zone_name_text = name
    zone_name_timer = duration or 3000
end

function M.draw(player, dt)
    -- HUD background bar
    chaos_gl.rect(0, HUD_Y, 320, 40, mu.CHAOS_GL_RGB(32, 32, 32))
    chaos_gl.rect(0, HUD_Y, 320, 1, mu.CHAOS_GL_RGB(64, 64, 64))

    -- Health
    local hc
    if player.health > 50 then
        hc = mu.CHAOS_GL_RGB(50, 200, 50)
    else
        hc = mu.CHAOS_GL_RGB(200, 50, 50)
    end
    chaos_gl.text(8, HUD_Y + 4,  "HEALTH", mu.CHAOS_GL_RGB(150, 150, 150), 0, 0)
    chaos_gl.text(8, HUD_Y + 20, tostring(player.health), hc, 0, 0)

    -- Ammo
    chaos_gl.text(250, HUD_Y + 4,  "AMMO", mu.CHAOS_GL_RGB(150, 150, 150), 0, 0)
    chaos_gl.text(250, HUD_Y + 20, tostring(player.ammo),
                  mu.CHAOS_GL_RGB(200, 200, 50), 0, 0)

    -- Keys
    if player.has_key_red then
        chaos_gl.rect(120, HUD_Y + 8, 12, 12, mu.CHAOS_GL_RGB(255, 0, 0))
    end
    if player.has_key_blue then
        chaos_gl.rect(136, HUD_Y + 8, 12, 12, mu.CHAOS_GL_RGB(0, 0, 255))
    end

    -- Weapon sprite
    local wtex
    if player.weapon_state == "firing" then
        wtex = Assets.get_texture("shotgun_fire")
    else
        wtex = Assets.get_texture("shotgun_idle")
    end
    if wtex then
        chaos_gl.blit_keyed(120, HUD_Y - 40, 80, 80, wtex, 0xFFFF00FF)
    end

    -- Zone name (fading)
    if zone_name_timer > 0 then
        zone_name_timer = zone_name_timer - dt
        local tw = chaos_gl.text_width(zone_name_text)
        chaos_gl.text(math.floor((320 - tw) / 2), 70, zone_name_text,
                      mu.CHAOS_GL_RGB(255, 200, 50), 0, 0)
    end
end

function M.check_zone_transition(player, level)
    local sector = level.sectors[player.sector_id]
    if not sector then return end
    local new_zone = sector.zone

    if new_zone and new_zone ~= player.current_zone then
        player.current_zone = new_zone
        local zone_info = level.zones and level.zones[new_zone]
        if zone_info then
            M.show_zone_name(zone_info.name, 3000)
        end
    end
end

return M
