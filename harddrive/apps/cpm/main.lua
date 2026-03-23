-- AIOS v2 — Chaos Package Manager GUI
local AppWindow = require("appwindow")
local win = AppWindow.new("Packages", 500, 400, {x=100, y=60})

local cpm = require("cpm")

local active_tab = 1
local tabs = {"Browse", "Installed", "Settings"}
local packages = {}
local errors = {}
local installed = {}
local status_msg = ""
local status_time = 0
local scroll_y = 0
local busy = false

local function set_status(msg)
    status_msg = msg
    status_time = aios.os.millis()
end

local function do_refresh()
    busy = true
    set_status("Refreshing package index...")
    local result = cpm.refresh(function(stage, detail)
        set_status(stage .. ": " .. (detail.name or detail.source or ""))
    end)
    packages = result.packages or {}
    errors = result.errors or {}
    installed = cpm.installed()
    if #errors > 0 then
        set_status("Refresh done (" .. #errors .. " error(s))")
    else
        set_status("Found " .. #packages .. " package(s)")
    end
    busy = false
end

local function do_install(name)
    busy = true
    set_status("Installing " .. name .. "...")
    local ok, err = cpm.install(name, function(stage, detail)
        set_status(stage .. ": " .. (detail.name or ""))
    end)
    installed = cpm.installed()
    if ok then
        set_status("Installed " .. name)
    else
        set_status("Error: " .. tostring(err))
    end
    busy = false
end

local function do_uninstall(name)
    busy = true
    set_status("Uninstalling " .. name .. "...")
    local ok, err = cpm.uninstall(name)
    installed = cpm.installed()
    if ok then
        set_status("Uninstalled " .. name)
    else
        set_status("Error: " .. tostring(err))
    end
    busy = false
end

local function do_update(name)
    busy = true
    set_status("Updating " .. name .. "...")
    local ok, err = cpm.update(name, function(stage, detail)
        set_status(stage .. ": " .. (detail.name or ""))
    end)
    installed = cpm.installed()
    if ok then
        set_status("Updated " .. name)
    else
        set_status("Error: " .. tostring(err))
    end
    busy = false
end

-- Initial load
installed = cpm.installed()
set_status("Press Refresh to fetch packages")

-- Button hit test helper
local _buttons = {}
local function draw_button(x, y, w, h, label, color)
    chaos_gl.rect_rounded(x, y, w, h, 4, color)
    local tw = chaos_gl.text_width(label)
    chaos_gl.text(x + (w - tw) // 2, y + (h - 12) // 2, label, 0x00FFFFFF, 0, 0)
    _buttons[#_buttons + 1] = {x=x, y=y, w=w, h=h, label=label}
end

local function hit_button(mx, my)
    for _, b in ipairs(_buttons) do
        if mx >= b.x and mx < b.x + b.w and my >= b.y and my < b.y + b.h then
            return b.label
        end
    end
    return nil
end

while win:is_running() do
    win:begin_frame()
    _buttons = {}

    local bg = win:color("window_bg", 0x002D2D2D)
    local text_c = win:color("text_primary", 0x00FFFFFF)
    local sec_c = win:color("text_secondary", 0x00AAAAAA)
    local accent = win:color("accent", 0x00FF8800)
    local btn_green = 0x00228B22
    local btn_red = 0x00AA2222
    local btn_blue = 0x00336699

    -- Tab bar
    local tab_x = 0
    for i, label in ipairs(tabs) do
        local tw = chaos_gl.text_width(label) + 24
        local tab_bg = (i == active_tab) and bg or 0x00333333
        chaos_gl.rect(tab_x, 28, tw, 32, tab_bg)
        local tc = (i == active_tab) and text_c or sec_c
        chaos_gl.text(tab_x + 12, 36, label, tc, 0, 0)
        if i == active_tab then
            chaos_gl.rect(tab_x, 58, tw, 2, accent)
        end
        tab_x = tab_x + tw
    end

    local cy = 64

    if active_tab == 1 then
        -- Browse tab
        draw_button(8, cy + 4, 80, 24, "Refresh", btn_blue)

        local count_str = #packages .. " package(s)"
        chaos_gl.text(100, cy + 10, count_str, sec_c, 0, 0)
        cy = cy + 36

        if #packages == 0 then
            chaos_gl.text(16, cy, "No packages loaded. Press Refresh.", sec_c, 0, 0)
        else
            for _, pkg in ipairs(packages) do
                if cy > 380 then break end
                chaos_gl.rect(8, cy, 480, 48, 0x00333333)

                chaos_gl.text(16, cy + 4, pkg.name .. " v" .. (pkg.version or "?"), text_c, 0, 0)
                chaos_gl.text(16, cy + 22, pkg.description or "", sec_c, 0, 0)

                local inst = installed[pkg.name]
                if inst then
                    if cpm._compare_versions(inst.version, pkg.version) < 0 then
                        draw_button(400, cy + 10, 70, 24, "Update:" .. pkg.name, accent)
                    else
                        chaos_gl.text(410, cy + 16, "Installed", 0x0044AA44, 0, 0)
                    end
                else
                    draw_button(400, cy + 10, 70, 24, "Install:" .. pkg.name, btn_green)
                end

                cy = cy + 52
            end
        end

    elseif active_tab == 2 then
        -- Installed tab
        local inst_list = {}
        for name, info in pairs(installed) do
            inst_list[#inst_list + 1] = { name = name, info = info }
        end
        table.sort(inst_list, function(a, b) return a.name < b.name end)

        if #inst_list == 0 then
            chaos_gl.text(16, cy + 8, "No packages installed via CPM.", sec_c, 0, 0)
        else
            for _, item in ipairs(inst_list) do
                if cy > 380 then break end
                chaos_gl.rect(8, cy, 480, 44, 0x00333333)
                chaos_gl.text(16, cy + 4, item.name .. " v" .. item.info.version, text_c, 0, 0)
                chaos_gl.text(16, cy + 22, "from " .. (item.info.source or "unknown"), sec_c, 0, 0)

                draw_button(400, cy + 8, 70, 24, "Remove:" .. item.name, btn_red)
                cy = cy + 48
            end
        end

    elseif active_tab == 3 then
        -- Settings tab
        chaos_gl.text(16, cy + 8, "AIOS Version: " .. aios.os.version(), text_c, 0, 0)
        cy = cy + 32

        chaos_gl.text(16, cy, "Package Sources:", text_c, 0, 0)
        cy = cy + 22
        local sources = {}
        pcall(function()
            local data = aios.io.readfile("/system/cpm/sources.lua")
            if data then
                local fn = load("return " .. data) or load(data)
                if fn then sources = fn() end
            end
        end)
        for _, src in ipairs(sources) do
            local enabled_str = src.enabled ~= false and "[ON]" or "[OFF]"
            chaos_gl.text(24, cy, enabled_str .. " " .. src.name, sec_c, 0, 0)
            cy = cy + 18
            chaos_gl.text(40, cy, src.url, 0x00666666, 0, 0)
            cy = cy + 22
        end

        cy = cy + 8
        draw_button(16, cy, 100, 24, "Clear Cache", btn_red)
        cy = cy + 32

        -- Cache info
        local cache_count = 0
        local cache_size = 0
        pcall(function()
            local entries = aios.io.listdir("/system/cpm/cache")
            if entries then
                for _, e in ipairs(entries) do
                    if not e.is_dir and e.name ~= "." and e.name ~= ".." then
                        cache_count = cache_count + 1
                        cache_size = cache_size + (e.size or 0)
                    end
                end
            end
        end)
        chaos_gl.text(16, cy, string.format("Cache: %d files, %d KB", cache_count, cache_size // 1024), sec_c, 0, 0)

        -- Block cache stats
        cy = cy + 24
        local cs = aios.os.cache_stats()
        if cs then
            chaos_gl.text(16, cy, string.format("Disk cache: %d%% hit rate (%d hits, %d misses)",
                cs.hit_rate, cs.hits, cs.misses), sec_c, 0, 0)
        end
    end

    -- Status bar at bottom
    if status_msg ~= "" then
        local age = aios.os.millis() - status_time
        local alpha = age < 5000 and text_c or sec_c
        chaos_gl.text(8, 378, status_msg, alpha, 0, 0)
    end

    win:end_frame()

    -- Events
    for _, event in ipairs(win:poll_events()) do
        if event.type == EVENT_MOUSE_DOWN and event.button == 1 and not busy then
            local mx, my = event.mouse_x, event.mouse_y

            -- Tab clicks
            if my >= 28 and my < 60 then
                local tx = 0
                for i, label in ipairs(tabs) do
                    local tw = chaos_gl.text_width(label) + 24
                    if mx >= tx and mx < tx + tw then
                        active_tab = i
                        break
                    end
                    tx = tx + tw
                end
            else
                local btn = hit_button(mx, my)
                if btn then
                    if btn == "Refresh" then
                        do_refresh()
                    elseif btn == "Clear Cache" then
                        cpm.clear_cache()
                        set_status("Cache cleared")
                    elseif btn:sub(1, 8) == "Install:" then
                        do_install(btn:sub(9))
                    elseif btn:sub(1, 7) == "Update:" then
                        do_update(btn:sub(8))
                    elseif btn:sub(1, 7) == "Remove:" then
                        do_uninstall(btn:sub(8))
                    end
                end
            end
        end
    end

    aios.os.sleep(50)
end

win:destroy()
