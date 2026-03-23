local M = {}
local Sector = require("lib/sector")
local Assets = require("lib/assets")

function M.load(path)
    -- loadfile + pcall to capture errors without crashing
    local chunk, err = loadfile(path)
    if not chunk then
        aios.debug.print("[rip] level loadfile error: " .. tostring(err) .. "\n")
        return nil
    end
    local ok, data = pcall(chunk)
    if not ok then
        aios.debug.print("[rip] level exec error: " .. tostring(data) .. "\n")
        return nil
    end
    if type(data) ~= "table" then
        aios.debug.print("[rip] level returned " .. type(data) .. ", not table\n")
        return nil
    end
    if not data.sectors then
        aios.debug.print("[rip] level has no sectors field\n")
        return nil
    end

    -- Resolve zone light inheritance
    if data.zones then
        for zone_name, zone in pairs(data.zones) do
            if zone.sectors then
                for _, sid in ipairs(zone.sectors) do
                    local sector = data.sectors[sid]
                    if sector and not sector.light then
                        sector.light = zone.light or 0.8
                    end
                    if sector then
                        sector.zone = zone_name
                    end
                end
            end
        end
    end

    -- Initialize door sectors
    for _, sector in pairs(data.sectors) do
        if sector.is_door then
            sector.door_open = false
            sector.door_timer = 0
        end
    end

    data.geometry_dirty = {}

    -- Load textures
    Assets.load_all(data)

    -- Generate all sector geometry
    Sector.generate_all(data)

    return data
end

function M.update_doors(level, dt)
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

function M.door_activate(player, level)
    local sector = level.sectors[player.sector_id]
    if not sector then return end

    for _, edge in ipairs(sector.edges) do
        if edge.type == "portal" then
            local target = level.sectors[edge.target_sector]
            if target and target.is_door then
                if target.key_required and
                   not player["has_key_" .. target.key_required] then
                    return
                end
                target.door_open = not target.door_open
                Assets.play_sound("door_open")
                return
            end
        end
    end
end

function M.free(level)
    if not level then return end
    for _, sector in pairs(level.sectors) do
        Sector.free_sector_models(sector)
    end
end

return M
