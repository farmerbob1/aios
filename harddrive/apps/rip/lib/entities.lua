local M = {}
local AI = require("lib/ai")
local Assets = require("lib/assets")
local Collision = require("lib/collision")
local mu = require("lib/math_util")

function M.spawn_all(level)
    local entities = {}
    for _, def in ipairs(level.entities or {}) do
        local ent = {
            type        = def.type,
            sector_id   = def.sector,
            x = def.x, z = def.z, y = 0,
            angle       = (def.angle or 0) * math.pi / 180,
            alive       = true,
            health      = 30,
            active      = false,
            ai_state    = "idle",
            ai_timer    = 0,
            attack_cooldown = 0,
            anim_state  = "idle",
            anim_frame  = 0,
            anim_timer  = 0,
            radius      = 0.4,
            height      = 1.5,
            sprite_w    = 1.8,
            sprite_h    = 1.8,
        }

        -- Set floor height
        local sector = level.sectors[ent.sector_id]
        if sector then
            ent.y = Collision.get_floor_at(sector, ent.x, ent.z)
        end

        -- Pickup-specific settings
        if def.type ~= "imp" then
            ent.health = nil
            ent.ai_state = nil
            ent.sprite_w = 1.5
            ent.sprite_h = 1.5
            ent.radius = 0.5
        end

        entities[#entities + 1] = ent
    end
    return entities
end

function M.update(entities, player, level, dt)
    for i = #entities, 1, -1 do
        local ent = entities[i]

        -- AI for enemies
        if ent.type == "imp" and ent.alive then
            AI.think(ent, player, level, dt)
            -- Update animation state from AI state
            ent.anim_state = ent.ai_state
            ent.anim_timer = ent.anim_timer + dt
            if ent.anim_timer > 200 then
                ent.anim_timer = 0
                ent.anim_frame = (ent.anim_frame + 1) % 2
            end
        end

        -- Pickup collection
        if ent.type ~= "imp" and ent.alive and player.alive then
            local dist = mu.distance_2d(ent.x, ent.z, player.x, player.z)
            if dist < ent.radius + player.radius then
                if ent.type == "health" then
                    if player.health < player.max_health then
                        player.health = math.min(player.health + 25, player.max_health)
                        ent.alive = false
                        Assets.play_sound("pickup")
                    end
                elseif ent.type == "ammo" then
                    if player.ammo < player.max_ammo then
                        player.ammo = math.min(player.ammo + 10, player.max_ammo)
                        ent.alive = false
                        Assets.play_sound("pickup")
                    end
                elseif ent.type == "key_red" then
                    player.has_key_red = true
                    ent.alive = false
                    Assets.play_sound("pickup")
                elseif ent.type == "key_blue" then
                    player.has_key_blue = true
                    ent.alive = false
                    Assets.play_sound("pickup")
                end
            end
        end
    end
end

function M.damage(ent, amount)
    if not ent.alive or ent.type ~= "imp" then return end
    ent.health = ent.health - amount
    if ent.health <= 0 then
        ent.health = 0
        ent.alive = false
        ent.ai_state = "dying"
        ent.ai_timer = 0
        Assets.play_sound("imp_death")
    else
        -- 50% chance of pain stun
        if math.random() < 0.5 then
            ent.ai_state = "pain"
            ent.ai_timer = 0
            Assets.play_sound("imp_pain")
        end
    end
end

return M
