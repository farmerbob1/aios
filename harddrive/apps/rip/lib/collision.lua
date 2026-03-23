local M = {}
local mu = require("lib/math_util")

function M.point_in_sector(sector, px, pz)
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

function M.get_floor_at(sector, x, z)
    if sector.floor_slope then
        local s = sector.floor_slope
        return (s.d - s.nx * x - s.nz * z) / s.ny
    end
    return sector.floor_h
end

function M.slide_along_edge(player, new_x, new_z, v0, v1, level)
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
    if M.point_in_sector(sector, slide_x, slide_z) then
        player.x = slide_x
        player.z = slide_z
    end
end

function M.move_player(player, new_x, new_z, level)
    local sector = level.sectors[player.sector_id]

    -- Still inside current sector
    if M.point_in_sector(sector, new_x, new_z) then
        player.x = new_x
        player.z = new_z
        player.y = M.get_floor_at(sector, new_x, new_z)
        return
    end

    -- Find the crossed edge
    for i, edge in ipairs(sector.edges) do
        local j = (i % #sector.vertices) + 1
        local v0 = sector.vertices[i]
        local v1 = sector.vertices[j]

        if mu.line_segments_intersect(player.x, player.z, new_x, new_z,
                                       v0.x, v0.z, v1.x, v1.z) then
            if edge.type == "portal" then
                local target = level.sectors[edge.target_sector]
                if not target then
                    M.slide_along_edge(player, new_x, new_z, v0, v1, level)
                    return
                end

                local p_floor = edge.portal_floor
                    or math.max(sector.floor_h, target.floor_h)
                local p_ceil = edge.portal_ceil
                    or math.min(sector.ceil_h, target.ceil_h)

                local step_up = p_floor - player.y
                if step_up > 0.5 then
                    M.slide_along_edge(player, new_x, new_z, v0, v1, level)
                    return
                end

                local clearance = p_ceil - math.max(player.y, p_floor)
                if clearance < player.height then
                    M.slide_along_edge(player, new_x, new_z, v0, v1, level)
                    return
                end

                if target.is_door and not target.door_open then
                    if target.key_required and
                       not player["has_key_" .. target.key_required] then
                        M.slide_along_edge(player, new_x, new_z, v0, v1, level)
                        return
                    end
                end

                -- Transition
                player.sector_id = edge.target_sector
                player.x = new_x
                player.z = new_z
                player.y = M.get_floor_at(target, new_x, new_z)
                return
            else
                M.slide_along_edge(player, new_x, new_z, v0, v1, level)
                return
            end
        end
    end
end

return M
