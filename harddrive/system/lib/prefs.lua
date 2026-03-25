-- AIOS System Preferences
-- Persistent key-value store backed by /system/preferences.lua
--
-- Usage:
--   local prefs = require("lib/prefs")
--   prefs.set("volume", 80)
--   local vol = prefs.get("volume", 100)  -- 100 is default if not set
--   prefs.save()  -- writes to disk

local M = {}
local PREFS_PATH = "/system/preferences.lua"
local data = {}
local _version = 0  -- bumped on every save, used for cross-task sync
local _last_check = 0
local _known_version = 0

-- Load prefs from disk
local function load()
    local fn, err = loadfile(PREFS_PATH)
    if fn then
        local ok, tbl = pcall(fn)
        if ok and type(tbl) == "table" then
            data = tbl
            _version = data._version or 0
            _known_version = _version
        end
    end
end

-- Save prefs to disk
function M.save()
    _version = _version + 1
    data._version = _version
    _known_version = _version

    local parts = {"return {\n"}
    -- Sort keys for stable output
    local keys = {}
    for k in pairs(data) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do
        local v = data[k]
        local vstr
        if type(v) == "string" then
            vstr = string.format("%q", v)
        elseif type(v) == "number" then
            vstr = tostring(v)
        elseif type(v) == "boolean" then
            vstr = v and "true" or "false"
        else
            goto skip
        end
        parts[#parts + 1] = string.format("    %s = %s,\n", k, vstr)
        ::skip::
    end
    parts[#parts + 1] = "}\n"

    local fd = aios.io.open(PREFS_PATH, "w")
    if fd then
        aios.io.write(fd, table.concat(parts))
        aios.io.close(fd)
    end
end

function M.get(key, default)
    local v = data[key]
    if v == nil then return default end
    return v
end

function M.set(key, value)
    data[key] = value
end

-- Set + save in one call
function M.put(key, value)
    data[key] = value
    M.save()
end

-- Check if prefs changed on disk (call periodically from event loops)
function M.poll()
    local now = aios.os.millis()
    if now - _last_check < 500 then return false end
    _last_check = now

    local fn = loadfile(PREFS_PATH)
    if fn then
        local ok, tbl = pcall(fn)
        if ok and type(tbl) == "table" then
            local disk_ver = tbl._version or 0
            if disk_ver ~= _known_version then
                data = tbl
                _version = disk_ver
                _known_version = disk_ver
                return true  -- changed
            end
        end
    end
    return false
end

-- Get raw table (read-only view)
function M.all()
    return data
end

-- Initial load
load()

return M
