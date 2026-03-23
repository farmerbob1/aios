local M = {}

local RENDER_W = 320
local RENDER_H = 200
local MAX_PORTAL_DEPTH = 16

local function project_point(wx, wy, wz, cam)
    -- Transform to view space
    local dx = wx - cam.eye_x
    local dy = wy - cam.eye_y
    local dz = wz - cam.eye_z
    -- View-space: right=X, up=Y, forward=-Z
    local vx = dx * cam.right_x + dz * cam.right_z
    local vy = dy
    local vz = -(dx * cam.forward_x + dz * cam.forward_z)

    if vz < cam.z_near then return nil, nil, false end

    local sx = RENDER_W / 2 + (vx / vz) * cam.focal_x
    local sy = RENDER_H / 2 - (vy / vz) * cam.focal_y
    return sx, sy, true
end

local function project_portal_to_screen(x0, z0, x1, z1, floor_y, ceil_y, cam)
    local corners = {
        {x0, floor_y, z0},
        {x1, floor_y, z1},
        {x1, ceil_y,  z1},
        {x0, ceil_y,  z0},
    }

    local min_sx, min_sy = RENDER_W, RENDER_H
    local max_sx, max_sy = 0, 0
    local any_in_front = false

    for _, c in ipairs(corners) do
        local sx, sy, in_front = project_point(c[1], c[2], c[3], cam)
        if in_front then
            any_in_front = true
            if sx < min_sx then min_sx = sx end
            if sy < min_sy then min_sy = sy end
            if sx > max_sx then max_sx = sx end
            if sy > max_sy then max_sy = sy end
        else
            -- Behind camera: portal extends off screen
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

local function clip_rect(a, b)
    local x0 = math.max(a.x0, b.x0)
    local y0 = math.max(a.y0, b.y0)
    local x1 = math.min(a.x1, b.x1)
    local y1 = math.min(a.y1, b.y1)
    if x0 > x1 or y0 > y1 then return nil end
    return { x0 = x0, y0 = y0, x1 = x1, y1 = y1 }
end

function M.cull(level, player_sector_id, camera)
    local visible = {}

    local function flood(sector_id, screen_window, depth)
        if depth > MAX_PORTAL_DEPTH then return end
        if visible[sector_id] then return end

        visible[sector_id] = true

        local sector = level.sectors[sector_id]
        if not sector then return end

        for i, edge in ipairs(sector.edges) do
            if edge.type == "portal" then
                local target_id = edge.target_sector
                local target = level.sectors[target_id]
                if target then
                    local j = (i % #sector.vertices) + 1
                    local v0 = sector.vertices[i]
                    local v1 = sector.vertices[j]

                    local p_floor = edge.portal_floor
                        or math.max(sector.floor_h, target.floor_h)
                    local p_ceil = edge.portal_ceil
                        or math.min(sector.ceil_h, target.ceil_h)
                    if p_ceil > p_floor then
                        local portal_rect = project_portal_to_screen(
                            v0.x, v0.z, v1.x, v1.z, p_floor, p_ceil, camera
                        )
                        if portal_rect then
                            local clipped = clip_rect(portal_rect, screen_window)
                            if clipped then
                                flood(target_id, clipped, depth + 1)
                            end
                        end
                    end
                end
            end
        end
    end

    local full_screen = { x0 = 0, y0 = 0, x1 = RENDER_W - 1, y1 = RENDER_H - 1 }
    flood(player_sector_id, full_screen, 0)
    return visible
end

return M
