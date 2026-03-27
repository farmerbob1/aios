-- AIOS Trash System
-- Moves files to /system/trash/ with restore support.

local M = {}
local TRASH_DIR = "/system/trash"
local INFO_PATH = "/system/trash/.trashinfo"

-- Load trash info from disk
local function load_info()
    local fn = loadfile(INFO_PATH)
    if fn then
        local ok, tbl = pcall(fn)
        if ok and type(tbl) == "table" then return tbl end
    end
    return {}
end

-- Save trash info to disk
local function save_info(info)
    local parts = {"return {\n"}
    for _, entry in ipairs(info) do
        parts[#parts + 1] = string.format(
            "    { name = %q, original = %q, time = %d },\n",
            entry.name, entry.original, entry.time or 0
        )
    end
    parts[#parts + 1] = "}\n"
    aios.io.writefile(INFO_PATH, table.concat(parts))
end

-- Generate a unique name in trash (append _2, _3, etc.)
local function unique_trash_name(name)
    if not aios.io.exists(TRASH_DIR .. "/" .. name) then
        return name
    end
    local base, ext = name:match("^(.+)(%.[^.]+)$")
    if not base then base = name; ext = "" end
    local n = 2
    while true do
        local candidate = base .. "_" .. n .. ext
        if not aios.io.exists(TRASH_DIR .. "/" .. candidate) then
            return candidate
        end
        n = n + 1
    end
end

-- Recursive delete helper
local function delete_recursive(path)
    local stat = aios.io.stat(path)
    if not stat then return end
    if stat.is_dir then
        local list = aios.io.listdir(path)
        if list then
            for _, e in ipairs(list) do
                if e.name ~= "." and e.name ~= ".." then
                    delete_recursive(path .. "/" .. e.name)
                end
            end
        end
        aios.io.rmdir(path)
    else
        aios.io.unlink(path)
    end
end

-- Move a file or directory to trash
function M.move(path)
    aios.io.mkdir(TRASH_DIR)

    -- Extract filename from path
    local name = path:match("([^/]+)$")
    if not name then return false end

    local trash_name = unique_trash_name(name)
    local ok = aios.io.rename(path, TRASH_DIR .. "/" .. trash_name)
    if not ok then return false end

    -- Record in .trashinfo
    local info = load_info()
    info[#info + 1] = {
        name = trash_name,
        original = path,
        time = aios.os.millis() // 1000,
    }
    save_info(info)
    return true
end

-- Restore a trashed item to its original location
function M.restore(trash_name)
    local info = load_info()
    for i, entry in ipairs(info) do
        if entry.name == trash_name then
            -- Ensure parent directory exists
            local parent = entry.original:match("^(.+)/[^/]+$")
            if parent and parent ~= "" then
                aios.io.mkdir(parent)
            end
            local ok = aios.io.rename(TRASH_DIR .. "/" .. trash_name, entry.original)
            if ok then
                table.remove(info, i)
                save_info(info)
                return true, entry.original
            end
            return false
        end
    end
    return false
end

-- Get the original path for a trashed item
function M.get_original(trash_name)
    local info = load_info()
    for _, entry in ipairs(info) do
        if entry.name == trash_name then
            return entry.original
        end
    end
    return nil
end

-- Empty the trash completely
function M.empty()
    local list = aios.io.listdir(TRASH_DIR)
    if list then
        for _, e in ipairs(list) do
            if e.name ~= "." and e.name ~= ".." and e.name:sub(1,1) ~= "." then
                delete_recursive(TRASH_DIR .. "/" .. e.name)
            end
        end
    end
    -- Clear .trashinfo
    save_info({})
end

-- List trash contents with original paths
function M.list()
    return load_info()
end

-- Count items in trash
function M.count()
    local list = aios.io.listdir(TRASH_DIR)
    if not list then return 0 end
    local n = 0
    for _, e in ipairs(list) do
        if e.name:sub(1,1) ~= "." then n = n + 1 end
    end
    return n
end

return M
