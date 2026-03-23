local M = {}
local mu = require("lib/math_util")
local Collision = require("lib/collision")
local Entities = require("lib/entities")

M.SHOTGUN = {
    damage = 10, pellets = 7, spread = 0.08,
    range = 30.0, fire_time = 200, cooldown_time = 600, ammo_cost = 1,
}

local function ray_blocked(player, dx, dz, max_dist, level)
    local current_sector = player.sector_id
    local steps = math.ceil(max_dist * 4)
    local px, pz = player.x, player.z

    for i = 1, steps do
        local t = (i / steps) * max_dist
        local sx = player.x + dx * t
        local sz = player.z + dz * t
        local sector = level.sectors[current_sector]
        if not sector then return true end

        if not Collision.point_in_sector(sector, sx, sz) then
            local found = false
            for ei, edge in ipairs(sector.edges) do
                if edge.type == "portal" then
                    local ej = (ei % #sector.vertices) + 1
                    if mu.line_segments_intersect(px, pz, sx, sz,
                        sector.vertices[ei].x, sector.vertices[ei].z,
                        sector.vertices[ej].x, sector.vertices[ej].z) then
                        current_sector = edge.target_sector
                        found = true
                        break
                    end
                end
            end
            if not found then return true end
        end
        px, pz = sx, sz
    end
    return false
end

function M.fire(player, weapon, entities, level)
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
                local d = mu.ray_circle_intersect(
                    player.x, player.z, dx, dz, ent.x, ent.z, ent.radius)
                if d and d < hit_dist then
                    if not ray_blocked(player, dx, dz, d, level) then
                        hit_ent = ent
                        hit_dist = d
                    end
                end
            end
        end

        if hit_ent then Entities.damage(hit_ent, weapon.damage) end
    end
end

function M.update(player, dt)
    if player.weapon_state == "firing" then
        player.weapon_timer = player.weapon_timer - dt
        if player.weapon_timer <= 0 then
            player.weapon_state = "cooldown"
            player.weapon_timer = M.SHOTGUN.cooldown_time
        end
    elseif player.weapon_state == "cooldown" then
        player.weapon_timer = player.weapon_timer - dt
        if player.weapon_timer <= 0 then
            player.weapon_state = "idle"
            player.weapon_timer = 0
        end
    end
end

return M
