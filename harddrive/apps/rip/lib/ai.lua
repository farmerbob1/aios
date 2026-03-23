local M = {}
local mu = require("lib/math_util")
local Collision = require("lib/collision")

function M.has_line_of_sight(ent, player, level)
    local dist = mu.distance_2d(ent.x, ent.z, player.x, player.z)
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
        if not sector then return false end

        if not Collision.point_in_sector(sector, sx, sz) then
            local found = false
            for ei, edge in ipairs(sector.edges) do
                if edge.type == "portal" then
                    local ej = (ei % #sector.vertices) + 1
                    if mu.line_segments_intersect(px, pz, sx, sz,
                        sector.vertices[ei].x, sector.vertices[ei].z,
                        sector.vertices[ej].x, sector.vertices[ej].z) then
                        local target = level.sectors[edge.target_sector]
                        if target then
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
            end
            if not found then return false end
        end
        px, pz = sx, sz
    end

    return (current_sector == player.sector_id)
end

function M.think(ent, player, level, dt)
    if not ent.alive then return end

    local state = ent.ai_state

    if state == "idle" then
        if player.alive and M.has_line_of_sight(ent, player, level) then
            ent.ai_state = "chase"
            ent.active = true
            ent.ai_timer = 0
        end

    elseif state == "chase" then
        -- Move toward player
        local dx = player.x - ent.x
        local dz = player.z - ent.z
        local dist = math.sqrt(dx * dx + dz * dz)

        if dist > 0.1 then
            ent.angle = math.atan(dx, dz)
            local speed = 2.0 * dt / 1000
            local nx = dx / dist * speed
            local nz = dz / dist * speed
            local new_x = ent.x + nx
            local new_z = ent.z + nz
            local sector = level.sectors[ent.sector_id]
            if sector and Collision.point_in_sector(sector, new_x, new_z) then
                ent.x = new_x
                ent.z = new_z
            end
        end

        -- Check for attack range
        if dist < 8 and player.alive and M.has_line_of_sight(ent, player, level) then
            if ent.attack_cooldown <= 0 then
                ent.ai_state = "attack"
                ent.ai_timer = 0
            end
        end

        -- Lost sight timeout
        if not M.has_line_of_sight(ent, player, level) then
            ent.ai_timer = ent.ai_timer + dt
            if ent.ai_timer > 3000 then
                ent.ai_state = "idle"
                ent.active = false
            end
        else
            ent.ai_timer = 0
        end

    elseif state == "attack" then
        ent.ai_timer = ent.ai_timer + dt
        if ent.ai_timer > 400 then
            -- Fire at player (random hit based on distance)
            local dist = mu.distance_2d(ent.x, ent.z, player.x, player.z)
            local hit_chance = mu.clamp(1.0 - dist / 15.0, 0.1, 0.8)
            if math.random() < hit_chance then
                local damage = 5 + math.random(10)
                require("lib/player").damage(player, damage)
            end
            ent.ai_state = "chase"
            ent.attack_cooldown = 1000 + math.random(500)
            ent.ai_timer = 0
        end

    elseif state == "pain" then
        ent.ai_timer = ent.ai_timer + dt
        if ent.ai_timer > 200 then
            ent.ai_state = "chase"
            ent.ai_timer = 0
        end

    elseif state == "dying" then
        ent.ai_timer = ent.ai_timer + dt
        if ent.ai_timer > 500 then
            ent.ai_state = "dead"
        end
    end

    -- Cooldown timer
    if ent.attack_cooldown > 0 then
        ent.attack_cooldown = ent.attack_cooldown - dt
    end
end

return M
