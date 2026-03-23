-- AIOS v2 — Chaos Package Manager (CPM) Core Library
-- Usage: local cpm = require("cpm")

local http = require("http")

local cpm = {}

local CPM_DIR = "/system/cpm"
local SOURCES_PATH = CPM_DIR .. "/sources.lua"
local INSTALLED_PATH = CPM_DIR .. "/installed.lua"
local CACHE_DIR = CPM_DIR .. "/cache"

local _index_cache = nil  -- last refresh()'d package index

-- ── Helpers ──────────────────────────────────────────

local function ensure_dir(path)
    if not aios.io.exists(path) then
        pcall(aios.io.mkdir, path)
    end
end

local function ensure_dirs()
    ensure_dir(CPM_DIR)
    ensure_dir(CACHE_DIR)
end

local function load_lua_file(path)
    if not aios.io.exists(path) then return nil end
    local data = aios.io.readfile(path)
    if not data then return nil end
    local fn, err = load("return " .. data)
    if not fn then
        fn, err = load(data)
    end
    if not fn then return nil, err end
    local ok, result = pcall(fn)
    if not ok then return nil, result end
    return result
end

local function serialize_value(val, indent)
    indent = indent or ""
    local t = type(val)
    if t == "string" then
        return string.format("%q", val)
    elseif t == "number" then
        if val == math.floor(val) and val >= 0 and val <= 0xFFFFFFFF then
            if val > 0xFFFF then
                return string.format("0x%08X", val)
            end
        end
        return tostring(val)
    elseif t == "boolean" then
        return tostring(val)
    elseif t == "table" then
        local parts = {}
        local next_indent = indent .. "    "
        -- Check if array-like
        local is_array = true
        local n = 0
        for k, _ in pairs(val) do
            n = n + 1
            if type(k) ~= "number" or k ~= n then
                is_array = false
            end
        end
        if is_array and n > 0 then
            for i = 1, n do
                parts[#parts + 1] = next_indent .. serialize_value(val[i], next_indent)
            end
        else
            local keys = {}
            for k, _ in pairs(val) do keys[#keys + 1] = k end
            table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
            for _, k in ipairs(keys) do
                local ks = type(k) == "string" and k:match("^[%a_][%w_]*$") and k
                           or ("[" .. serialize_value(k) .. "]")
                parts[#parts + 1] = next_indent .. ks .. " = " .. serialize_value(val[k], next_indent)
            end
        end
        return "{\n" .. table.concat(parts, ",\n") .. ",\n" .. indent .. "}"
    end
    return "nil"
end

local function serialize_table(tbl)
    return "return " .. serialize_value(tbl) .. "\n"
end

local function load_sources()
    local sources = load_lua_file(SOURCES_PATH)
    if sources then return sources end
    return {
        { name = "AIOS Official", url = "https://farmerbob1.github.io/chaos-repo", enabled = true },
    }
end

local function load_installed()
    local db = load_lua_file(INSTALLED_PATH)
    return db or {}
end

local function save_installed(db)
    ensure_dirs()
    local data = serialize_table(db)
    local tmp = INSTALLED_PATH .. ".tmp"
    aios.io.writefile(tmp, data)
    aios.io.rename(tmp, INSTALLED_PATH)
end

-- ── Version Comparison ───────────────────────────────

function cpm._compare_versions(a, b)
    local a1, a2, a3 = (a or "0.0.0"):match("^(%d+)%.(%d+)%.(%d+)")
    local b1, b2, b3 = (b or "0.0.0"):match("^(%d+)%.(%d+)%.(%d+)")
    a1, a2, a3 = tonumber(a1) or 0, tonumber(a2) or 0, tonumber(a3) or 0
    b1, b2, b3 = tonumber(b1) or 0, tonumber(b2) or 0, tonumber(b3) or 0
    if a1 ~= b1 then return a1 < b1 and -1 or 1 end
    if a2 ~= b2 then return a2 < b2 and -1 or 1 end
    if a3 ~= b3 then return a3 < b3 and -1 or 1 end
    return 0
end

-- ── Path Safety ──────────────────────────────────────

local function is_path_safe(package_name, entry_path)
    if entry_path:sub(1, 1) == "/" then return false end
    if entry_path:find("%.%.") then return false end
    local target = "/apps/" .. package_name .. "/" .. entry_path
    local prefix = "/apps/" .. package_name .. "/"
    return target:sub(1, #prefix) == prefix
end

-- ── Dependency Resolution ────────────────────────────

local function resolve_deps(name, index, installed, visiting, order)
    visiting = visiting or {}
    order = order or {}

    if visiting[name] then
        return nil, "dependency cycle: " .. name
    end
    if installed[name] then return order end

    -- Find package in index
    local pkg = nil
    for _, p in ipairs(index) do
        if p.name == name then pkg = p; break end
    end
    if not pkg then
        return nil, "package not found: " .. name
    end

    visiting[name] = true

    -- Resolve dependencies first
    if pkg.depends then
        for _, dep in ipairs(pkg.depends) do
            if not installed[dep] then
                local _, err = resolve_deps(dep, index, installed, visiting, order)
                if err then return nil, err end
            end
        end
    end

    visiting[name] = nil
    order[#order + 1] = pkg
    return order
end

-- ── Public API ───────────────────────────────────────

function cpm.refresh(progress_cb)
    ensure_dirs()
    local sources = load_sources()
    local all_packages = {}
    local errors = {}

    for _, src in ipairs(sources) do
        if src.enabled ~= false then
            if progress_cb then progress_cb("refreshing", { source = src.name }) end

            local url = src.url .. "/repo.lua"
            local body, status, headers = http.get(url, 15000)

            if not body or status ~= 200 then
                errors[#errors + 1] = {
                    source = src.name,
                    error = string.format("HTTP %s from %s", tostring(status), url),
                }
            else
                local fn, err = load("return " .. body)
                if not fn then
                    fn, err = load(body)
                end
                if fn then
                    local ok, repo = pcall(fn)
                    if ok and type(repo) == "table" and repo.packages then
                        for _, pkg in ipairs(repo.packages) do
                            pkg._source = src.name
                            pkg._base_url = src.url
                            all_packages[#all_packages + 1] = pkg
                        end
                    else
                        errors[#errors + 1] = { source = src.name, error = "invalid repo.lua format" }
                    end
                else
                    errors[#errors + 1] = { source = src.name, error = "parse error: " .. tostring(err) }
                end
            end
        end
    end

    _index_cache = all_packages
    return { packages = all_packages, errors = errors }
end

function cpm.installed()
    return load_installed()
end

function cpm.check_updates()
    local installed = load_installed()
    local updates = {}

    if not _index_cache then return updates end

    for _, pkg in ipairs(_index_cache) do
        local inst = installed[pkg.name]
        if inst and cpm._compare_versions(inst.version, pkg.version) < 0 then
            updates[#updates + 1] = {
                name = pkg.name,
                current_version = inst.version,
                available_version = pkg.version,
                size = pkg.size,
            }
        end
    end

    return updates
end

function cpm.install(name, progress_cb)
    if not _index_cache then
        return nil, "run cpm.refresh() first"
    end

    local installed = load_installed()

    -- Resolve dependencies
    if progress_cb then progress_cb("resolving", { name = name }) end
    local install_queue, err = resolve_deps(name, _index_cache, installed)
    if not install_queue then return nil, err end

    if #install_queue == 0 then
        return true  -- already installed
    end

    -- Check min_aios for all packages
    local aios_ver = aios.os.version()
    for _, pkg in ipairs(install_queue) do
        if pkg.min_aios and cpm._compare_versions(aios_ver, pkg.min_aios) < 0 then
            return nil, string.format("incompatible: %s requires AIOS %s, running %s",
                                      pkg.name, pkg.min_aios, aios_ver)
        end
    end

    -- Check disk space (conservative: 4x .cpk size)
    local fs = aios.os.fsinfo()
    local free_bytes = (fs.free_blocks or 0) * 4096
    local total_needed = 0
    for _, pkg in ipairs(install_queue) do
        total_needed = total_needed + (pkg.size or 0) * 4
    end
    if free_bytes < total_needed then
        return nil, string.format("disk full: need ~%d bytes, only %d free",
                                  total_needed, free_bytes)
    end

    -- Install each package in queue
    for _, pkg in ipairs(install_queue) do
        -- Skip if already installed at this version
        if installed[pkg.name] and installed[pkg.name].version == pkg.version then
            goto continue_pkg
        end

        -- Download
        if progress_cb then progress_cb("downloading", { name = pkg.name, size = pkg.size }) end
        local cache_file = CACHE_DIR .. "/" .. pkg.name .. "-" .. pkg.version .. ".cpk"

        if not aios.io.exists(cache_file) then
            local url = pkg._base_url .. "/" .. pkg.url
            local body, status = http.get(url, 30000)
            if not body or status ~= 200 then
                return nil, string.format("download failed: HTTP %s for %s", tostring(status), pkg.url)
            end
            aios.io.writefile(cache_file, body)
        end

        -- Get file list before extraction (for uninstall tracking)
        if progress_cb then progress_cb("extracting", { name = pkg.name }) end

        local h = aios.cpk.open(cache_file)
        if not h then
            return nil, "failed to open " .. cache_file
        end

        local entries = aios.cpk.list(h)
        aios.cpk.close(h)

        if not entries or #entries == 0 then
            return nil, "empty or corrupt package: " .. pkg.name
        end

        -- Path safety check
        for _, entry in ipairs(entries) do
            if not is_path_safe(pkg.name, entry.path) then
                return nil, string.format("path escape rejected: entry '%s' in %s",
                                          entry.path, pkg.name)
            end
        end

        -- Extract
        local install_dir = "/apps/" .. pkg.name
        local count, extract_err = aios.cpk.install(cache_file, install_dir)
        if not count then
            return nil, "extraction failed for " .. pkg.name .. ": " .. tostring(extract_err)
        end

        -- Validate manifest
        if progress_cb then progress_cb("validating", { name = pkg.name }) end
        if not aios.io.exists(install_dir .. "/manifest.lua") then
            -- Clean up
            pcall(function()
                for _, entry in ipairs(entries) do
                    pcall(aios.io.unlink, install_dir .. "/" .. entry.path)
                end
                pcall(aios.io.rmdir, install_dir)
            end)
            return nil, "invalid package: no manifest.lua in " .. pkg.name
        end

        -- Build file list for installed database
        local file_list = {}
        for _, entry in ipairs(entries) do
            file_list[#file_list + 1] = install_dir .. "/" .. entry.path
        end

        -- Register
        if progress_cb then progress_cb("finalizing", { name = pkg.name }) end
        installed[pkg.name] = {
            version = pkg.version,
            installed_at = aios.os.ticks(),
            source = pkg._source or "unknown",
            checksum = pkg.checksum or 0,
            files = file_list,
        }
        save_installed(installed)

        ::continue_pkg::
    end

    -- Refresh app menu
    pcall(function()
        local wm = require("wm")
        if wm._scan_apps then wm._scan_apps() end
    end)

    return true
end

function cpm.uninstall(name)
    local installed = load_installed()

    if not installed[name] then
        return nil, "package not installed: " .. name
    end

    -- Check dependents
    for pkg_name, pkg_info in pairs(installed) do
        if pkg_name ~= name then
            -- Check if this package depends on the one being removed
            -- We need to check the repo index for dependency info
            if _index_cache then
                for _, repo_pkg in ipairs(_index_cache) do
                    if repo_pkg.name == pkg_name and repo_pkg.depends then
                        for _, dep in ipairs(repo_pkg.depends) do
                            if dep == name then
                                return nil, string.format("cannot uninstall: %s depends on %s",
                                                          pkg_name, name)
                            end
                        end
                    end
                end
            end
        end
    end

    -- Delete files
    local info = installed[name]
    if info.files then
        -- Sort by length descending so deeper paths are deleted first
        local sorted = {}
        for _, f in ipairs(info.files) do sorted[#sorted + 1] = f end
        table.sort(sorted, function(a, b) return #a > #b end)

        for _, filepath in ipairs(sorted) do
            pcall(aios.io.unlink, filepath)
        end
    end

    -- Remove app directory (try to clean up empty subdirs)
    local app_dir = "/apps/" .. name
    local function rmdir_recursive(dir)
        local ok, entries = pcall(aios.io.listdir, dir)
        if ok and entries then
            for _, e in ipairs(entries) do
                if e.name ~= "." and e.name ~= ".." then
                    if e.is_dir then
                        rmdir_recursive(dir .. "/" .. e.name)
                    else
                        pcall(aios.io.unlink, dir .. "/" .. e.name)
                    end
                end
            end
        end
        pcall(aios.io.rmdir, dir)
    end
    rmdir_recursive(app_dir)

    -- Unregister
    installed[name] = nil
    save_installed(installed)

    -- Refresh app menu
    pcall(function()
        local wm = require("wm")
        if wm._scan_apps then wm._scan_apps() end
    end)

    return true
end

function cpm.update(name, progress_cb)
    local installed = load_installed()
    if not installed[name] then
        return nil, "package not installed: " .. name
    end

    local app_dir = "/apps/" .. name

    -- Preserve data/ directory
    local has_data = aios.io.exists(app_dir .. "/data")
    local data_backup = {}
    if has_data then
        if progress_cb then progress_cb("preserving", { name = name }) end
        local ok, entries = pcall(aios.io.listdir, app_dir .. "/data")
        if ok and entries then
            for _, e in ipairs(entries) do
                if e.name ~= "." and e.name ~= ".." and not e.is_dir then
                    local content = aios.io.readfile(app_dir .. "/data/" .. e.name)
                    if content then
                        data_backup[e.name] = content
                    end
                end
            end
        end
    end

    -- Uninstall then reinstall
    local ok, err = cpm.uninstall(name)
    if not ok then return nil, "update uninstall failed: " .. tostring(err) end

    ok, err = cpm.install(name, progress_cb)
    if not ok then return nil, "update install failed: " .. tostring(err) end

    -- Restore data/
    if has_data and next(data_backup) then
        ensure_dir(app_dir .. "/data")
        for fname, content in pairs(data_backup) do
            aios.io.writefile(app_dir .. "/data/" .. fname, content)
        end
    end

    return true
end

function cpm.update_all(progress_cb)
    local updates = cpm.check_updates()
    local updated = {}
    local failed = {}

    for _, u in ipairs(updates) do
        local ok, err = cpm.update(u.name, progress_cb)
        if ok then
            updated[#updated + 1] = u.name
        else
            failed[#failed + 1] = { name = u.name, error = err }
        end
    end

    return { updated = updated, failed = failed }
end

function cpm.search(query)
    if not _index_cache then return {} end
    query = query:lower()
    local results = {}
    for _, pkg in ipairs(_index_cache) do
        if pkg.name:lower():find(query, 1, true) or
           (pkg.description and pkg.description:lower():find(query, 1, true)) then
            results[#results + 1] = pkg
        end
    end
    return results
end

function cpm.info(name)
    local installed = load_installed()
    local repo_info = nil
    if _index_cache then
        for _, pkg in ipairs(_index_cache) do
            if pkg.name == name then repo_info = pkg; break end
        end
    end

    if not repo_info and not installed[name] then return nil end

    local result = {}
    if repo_info then
        for k, v in pairs(repo_info) do result[k] = v end
    end
    if installed[name] then
        result.installed = true
        result.installed_version = installed[name].version
        result.installed_at = installed[name].installed_at
    else
        result.installed = false
    end
    return result
end

function cpm.clear_cache()
    local ok, entries = pcall(aios.io.listdir, CACHE_DIR)
    if ok and entries then
        for _, e in ipairs(entries) do
            if e.name ~= "." and e.name ~= ".." and not e.is_dir then
                pcall(aios.io.unlink, CACHE_DIR .. "/" .. e.name)
            end
        end
    end
    return true
end

return cpm
