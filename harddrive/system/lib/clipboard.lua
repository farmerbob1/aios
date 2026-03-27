-- AIOS Clipboard System
-- Copy/cut/paste files with cross-task sync via /system/.clipboard

local M = {}
local CLIP_PATH = "/system/.clipboard"
local _version = 0
local _known_version = 0

-- Load clipboard state from disk
local function load()
    local fn = loadfile(CLIP_PATH)
    if fn then
        local ok, tbl = pcall(fn)
        if ok and type(tbl) == "table" then
            _version = tbl._version or 0
            _known_version = _version
            return tbl
        end
    end
    return nil
end

-- Save clipboard state to disk
local function save(data)
    _version = _version + 1
    _known_version = _version
    data._version = _version

    local parts = {"return {\n"}
    parts[#parts + 1] = string.format("    _version = %d,\n", data._version)
    parts[#parts + 1] = string.format("    mode = %q,\n", data.mode)
    parts[#parts + 1] = "    paths = {\n"
    for _, p in ipairs(data.paths or {}) do
        parts[#parts + 1] = string.format("        %q,\n", p)
    end
    parts[#parts + 1] = "    },\n"
    parts[#parts + 1] = "}\n"
    aios.io.writefile(CLIP_PATH, table.concat(parts))
end

-- Copy file paths to clipboard
function M.copy(paths)
    save({ mode = "copy", paths = paths })
end

-- Cut file paths to clipboard
function M.cut(paths)
    save({ mode = "cut", paths = paths })
end

-- Get clipboard contents
function M.get()
    return load()
end

-- Check if clipboard has content
function M.has_content()
    local data = load()
    return data ~= nil and data.paths ~= nil and #data.paths > 0
end

-- Clear clipboard
function M.clear()
    _version = _version + 1
    _known_version = _version
    aios.io.writefile(CLIP_PATH, "return {}\n")
end

-- Recursive copy helper
local function copy_recursive(src, dst)
    local stat = aios.io.stat(src)
    if not stat then return false end

    if stat.is_dir then
        aios.io.mkdir(dst)
        local list = aios.io.listdir(src)
        if list then
            for _, e in ipairs(list) do
                if e.name ~= "." and e.name ~= ".." then
                    copy_recursive(src .. "/" .. e.name, dst .. "/" .. e.name)
                end
            end
        end
        return true
    else
        return aios.io.copyfile(src, dst)
    end
end

-- Paste clipboard contents to destination directory
function M.paste(dest_dir)
    local data = load()
    if not data or not data.paths or #data.paths == 0 then return false end

    for _, path in ipairs(data.paths) do
        local name = path:match("([^/]+)$")
        if not name then goto continue end

        local dest = dest_dir .. "/" .. name
        -- Avoid overwriting: append suffix
        if aios.io.exists(dest) then
            local base, ext = name:match("^(.+)(%.[^.]+)$")
            if not base then base = name; ext = "" end
            local n = 2
            while aios.io.exists(dest) do
                dest = dest_dir .. "/" .. base .. "_" .. n .. ext
                n = n + 1
            end
        end

        if data.mode == "copy" then
            copy_recursive(path, dest)
        elseif data.mode == "cut" then
            local ok = aios.io.rename(path, dest)
            if not ok then
                -- Fallback: copy + delete
                copy_recursive(path, dest)
                -- Delete source (recursive for dirs)
                local stat = aios.io.stat(path)
                if stat and stat.is_dir then
                    -- Recursive delete
                    local function del_r(p)
                        local list = aios.io.listdir(p)
                        if list then
                            for _, e in ipairs(list) do
                                if e.name ~= "." and e.name ~= ".." then
                                    if e.is_dir then del_r(p .. "/" .. e.name)
                                    else aios.io.unlink(p .. "/" .. e.name) end
                                end
                            end
                        end
                        aios.io.rmdir(p)
                    end
                    del_r(path)
                else
                    aios.io.unlink(path)
                end
            end
        end
        ::continue::
    end

    -- Clear clipboard after cut
    if data.mode == "cut" then M.clear() end
    return true
end

-- Poll for changes (cross-task sync)
function M.poll()
    local fn = loadfile(CLIP_PATH)
    if fn then
        local ok, tbl = pcall(fn)
        if ok and type(tbl) == "table" then
            local disk_ver = tbl._version or 0
            if disk_ver ~= _known_version then
                _known_version = disk_ver
                return true
            end
        end
    end
    return false
end

return M
