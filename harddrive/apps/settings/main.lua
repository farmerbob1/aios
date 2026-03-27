-- AIOS v2 — Settings Application
local AppWindow = require("appwindow")
local ui = require("core")
local prefs = require("lib/prefs")
local Button = require("button")
local Slider = require("slider")
local ProgressBar = require("progressbar")

local win = AppWindow.new("Settings", 500, 400, {x=120, y=60})

local active_tab = 1
local tabs = {"Appearance", "Display", "Sound", "Network", "System", "Modules"}
local last_refresh = 0

-- Cached data
local mem_info, fs_info, net_info, cache_info, compose_stats
local cpu_pct = 0
local volume_level = prefs.get("volume", 80)
local module_list = {}
local cur_theme = prefs.get("theme", "/system/themes/dark.lua"):match("([^/]+)%.lua$") or "dark"

local function refresh_data()
    mem_info = aios.os.meminfo()
    fs_info = aios.os.fsinfo()
    cache_info = aios.os.cache_stats()
    cpu_pct = aios.task.cpu_usage() or 0
    compose_stats = chaos_gl.get_compose_stats()
    local ok, info = pcall(aios.net.ifconfig)
    net_info = ok and info or nil
    local ok2, mods = pcall(aios.io.listdir, "/system/modules")
    module_list = {}
    if ok2 and mods then
        for _, m in ipairs(mods) do
            if m.name and m.name:match("%.kaos$") then
                module_list[#module_list + 1] = m
            end
        end
    end
end

-- Helper: section header
local function section(y, title)
    chaos_gl.text(16, y, title, theme.text_primary or 0x00FFFFFF, 0, 0)
    chaos_gl.rect(16, y + 16, 460, 1, theme.separator or theme.border or 0x00444444)
    return y + 24
end

-- Helper: label + value row
local function row(y, label, value)
    chaos_gl.text(24, y, label, theme.text_secondary or 0x00AAAAAA, 0, 0)
    chaos_gl.text(200, y, tostring(value), theme.text_primary or 0x00FFFFFF, 0, 0)
    return y + 18
end

-- Helper: progress bar
local _pb_temp = ProgressBar.new({w=180, h=10})
local function progress(y, label, pct, w)
    chaos_gl.text(24, y, label, theme.text_secondary or 0x00AAAAAA, 0, 0)
    _pb_temp.w = w or 180
    _pb_temp.value = math.floor(math.max(0, math.min(1, pct)) * 100)
    _pb_temp.max = 100
    _pb_temp:draw(200, y + 2)
    chaos_gl.text(200 + (w or 180) + 8, y, string.format("%.0f%%", pct * 100),
                  theme.text_secondary or 0x00AAAAAA, 0, 0)
    return y + 20
end

-- Helper: draw a themed button
local function draw_button(x, y, w, h, label, selected)
    local bg = selected and (theme.accent or 0x00FF8800)
                         or (theme.button_normal or 0x00444444)
    local fg = selected and (theme.accent_text or 0x00FFFFFF)
                         or (theme.text_primary or 0x00FFFFFF)
    local r = theme.button_radius or 4
    chaos_gl.rect_rounded(x, y, w, h, r, bg)
    local tw = chaos_gl.text_width(label)
    chaos_gl.text(x + (w - tw) // 2, y + (h - 12) // 2, label, fg, 0, 0)
end

-- Helper: mini desktop preview showing theme colors
local function draw_preview(x, y, w, h)
    local bg = theme.desktop_bg or 0x00222244
    local win_bg = theme.window_bg or 0x002D2D2D
    local title_bg = theme.titlebar_bg or 0x00333333
    local accent = theme.accent or 0x00FF8800
    local text1 = theme.text_primary or 0x00FFFFFF
    local text2 = theme.text_secondary or 0x00AAAAAA
    local btn_bg = theme.button_normal or 0x00444444
    local taskbar = theme.taskbar_bg or 0x001A1A1A

    -- Desktop background
    chaos_gl.rect(x, y, w, h, bg)

    -- Mini window
    local wx, wy, ww, wh = x + 8, y + 6, w - 16, h - 22
    chaos_gl.rect(wx, wy, ww, wh, win_bg)
    chaos_gl.rect(wx, wy, ww, 10, title_bg)

    -- Traffic light dots
    chaos_gl.circle(wx + 6, wy + 5, 2, 0x00FF5F57)
    chaos_gl.circle(wx + 13, wy + 5, 2, 0x00FFBD2E)
    chaos_gl.circle(wx + 20, wy + 5, 2, 0x0028C840)

    -- Window content lines
    chaos_gl.rect(wx + 4, wy + 14, ww * 3 // 4, 3, text2)
    chaos_gl.rect(wx + 4, wy + 20, ww // 2, 3, text2)
    chaos_gl.rect(wx + 4, wy + 28, 30, 8, accent)
    chaos_gl.rect(wx + 38, wy + 28, 30, 8, btn_bg)

    -- Taskbar
    chaos_gl.rect(x, y + h - 10, w, 10, taskbar)
    chaos_gl.rect(x + 4, y + h - 8, 6, 6, accent)
    chaos_gl.rect(x + 14, y + h - 8, 6, 6, text2)
    chaos_gl.rect(x + 24, y + h - 8, 6, 6, text2)
end

refresh_data()
pcall(aios.audio.volume, volume_level)

-- Widgets
local btn_dark  = Button.new("Dark", function()
    ui.set_theme("/system/themes/dark.lua"); cur_theme = "dark"
end, {w=130, h=30})
local btn_light = Button.new("Light", function()
    ui.set_theme("/system/themes/light.lua"); cur_theme = "light"
end, {w=130, h=30})
local vol_slider = Slider.new(0, 100, volume_level, function(v)
    volume_level = math.floor(v)
    pcall(aios.audio.volume, volume_level)
    prefs.put("volume", volume_level)
end, {w=260, h=16})
local pb_ram  = ProgressBar.new({w=200, h=12})
local pb_heap = ProgressBar.new({w=200, h=12})
local pb_cpu  = ProgressBar.new({w=200, h=12})
local settings_widgets = {btn_dark, btn_light, vol_slider, pb_ram, pb_heap, pb_cpu}

while win:is_running() do
    win:begin_frame()

    local now = aios.os.millis()
    if now - last_refresh > 1000 then
        refresh_data()
        last_refresh = now
    end

    -- Tab bar
    local tab_bg = theme.tab_bg or 0x00333333
    local tab_active = theme.tab_active_bg or theme.window_bg or 0x002D2D2D
    local tab_ind = theme.tab_indicator or theme.accent or 0x00FF8800
    local tx = 0
    for i, label in ipairs(tabs) do
        local tw = chaos_gl.text_width(label) + 20
        if i == active_tab then
            chaos_gl.rect(tx, 28, tw, 32, tab_active)
            chaos_gl.rect(tx, 58, tw, 2, tab_ind)
            chaos_gl.text(tx + 10, 37, label, theme.text_primary or 0x00FFFFFF, 0, 0)
        else
            chaos_gl.rect(tx, 28, tw, 32, tab_bg)
            chaos_gl.text(tx + 10, 37, label, theme.text_secondary or 0x00AAAAAA, 0, 0)
        end
        tx = tx + tw
    end
    chaos_gl.rect(tx, 28, 500 - tx, 32, tab_bg)

    local y = 68

    if active_tab == 1 then
        -- ═══ Appearance ═══
        y = section(y, "Theme")

        -- Use real Button widgets
        btn_dark.pressed = (cur_theme == "dark")
        btn_light.pressed = (cur_theme == "light")
        btn_dark:draw(24, y)
        btn_light:draw(170, y)
        y = y + 48

        -- Mini preview
        y = section(y, "Preview")
        draw_preview(24, y, 180, 100)

        -- Color swatches
        local sx = 220
        local colors = {
            {"Background", theme.window_bg or 0x002D2D2D},
            {"Text", theme.text_primary or 0x00FFFFFF},
            {"Accent", theme.accent or 0x00FF8800},
            {"Button", theme.button_normal or 0x00444444},
            {"Taskbar", theme.taskbar_bg or 0x001A1A1A},
        }
        for _, c in ipairs(colors) do
            chaos_gl.rect_rounded(sx, y, 16, 16, 3, c[2])
            chaos_gl.text(sx + 22, y + 2, c[1], theme.text_secondary or 0x00AAAAAA, 0, 0)
            y = y + 20
        end

    elseif active_tab == 2 then
        -- ═══ Display ═══
        y = section(y, "Screen")
        local sw, sh = chaos_gl.get_screen_size()
        y = row(y, "Resolution", (sw or "?") .. " x " .. (sh or "?"))
        y = row(y, "Color Depth", "32-bit BGRA")
        y = row(y, "Refresh", "Software compositor")
        y = y + 8

        if compose_stats then
            y = section(y, "Renderer Stats")
            y = row(y, "Compose Time", (compose_stats.compose_time_us or 0) .. " us")
            y = row(y, "Frame Time", (compose_stats.frame_time_us or 0) .. " us")
            y = row(y, "Surfaces", compose_stats.surfaces_composited or 0)
            y = row(y, "Triangles", (compose_stats.triangles_drawn or 0) .. "/" .. (compose_stats.triangles_submitted or 0))
            y = row(y, "2D Draw Calls", compose_stats.draw_calls_2d or 0)
        end

    elseif active_tab == 3 then
        -- ═══ Sound ═══
        y = section(y, "Master Volume")

        vol_slider.value = volume_level
        vol_slider:draw(24, y)
        y = y + 28

        if volume_level == 0 then
            chaos_gl.text(24, y, "MUTED", 0x00C04040, 0, 0)
            y = y + 20
        end
        y = y + 8

        y = section(y, "Playback")
        local ok3, playing = pcall(aios.audio.playing)
        local count = (ok3 and playing) and #playing or 0
        y = row(y, "Active Sources", count)

    elseif active_tab == 4 then
        -- ═══ Network ═══
        if net_info then
            y = section(y, "Interface: e0")
            local link_ok = net_info.ip and net_info.ip ~= "0.0.0.0"
            chaos_gl.circle(32, y + 6, 5, link_ok and 0x0040C040 or 0x00C04040)
            chaos_gl.text(44, y, link_ok and "Connected" or "Awaiting DHCP...",
                          link_ok and 0x0040C040 or 0x00C04040, 0, 0)
            y = y + 24
            y = row(y, "IP Address", net_info.ip or "--")
            y = row(y, "Subnet Mask", net_info.mask or "--")
            y = row(y, "Gateway", net_info.gw or "--")
            y = row(y, "MAC Address", net_info.mac or "--")
        else
            y = section(y, "Network")
            chaos_gl.text(24, y, "No network interface detected.",
                          theme.text_secondary or 0x00AAAAAA, 0, 0)
        end

    elseif active_tab == 5 then
        -- ═══ System ═══
        y = section(y, "Overview")
        y = row(y, "OS Version", "AIOS " .. (aios.os.version() or "?"))
        local millis = aios.os.millis()
        local secs = math.floor(millis / 1000)
        y = row(y, "Uptime", string.format("%dh %02dm %02ds",
                math.floor(secs/3600), math.floor(secs/60)%60, secs%60))
        y = y + 4

        if mem_info then
            y = section(y, "Memory")
            local ram_pages = mem_info.pmm_ram_pages or mem_info.pmm_total_pages or 0
            local free_pages = mem_info.pmm_free_pages or 0
            local used_pages = ram_pages - free_pages
            local ram_mb = ram_pages * 4 / 1024
            local pct = ram_pages > 0 and (used_pages / ram_pages) or 0
            y = progress(y, "RAM", pct, 180)
            y = row(y, "", string.format("%d / %d MB",
                    used_pages * 4 // 1024, math.floor(ram_mb)))

            local heap_used = (mem_info.heap_used or 0) // 1024
            local heap_free = (mem_info.heap_free or 0) // 1024
            local heap_total = heap_used + heap_free
            local heap_pct = heap_total > 0 and (heap_used / heap_total) or 0
            y = progress(y, "Heap", heap_pct, 180)
            y = row(y, "", string.format("%d / %d KB", heap_used, heap_total))
        end

        y = progress(y, "CPU", (cpu_pct or 0) / 100, 180)
        y = y + 4

        if fs_info then
            y = section(y, "Storage")
            local total_mb = (fs_info.total_blocks or 0) * 4 / 1024
            local free_mb = (fs_info.free_blocks or 0) * 4 / 1024
            y = row(y, "ChaosFS", string.format("%.0f / %.0f MB used",
                    total_mb - free_mb, total_mb))
            y = row(y, "Free Inodes", fs_info.free_inodes or 0)
        end

        if cache_info then
            y = row(y, "Cache Hit Rate", (cache_info.hit_rate or 0) .. "%")
        end

    elseif active_tab == 6 then
        -- ═══ Modules ═══
        y = section(y, "Kernel Modules")
        if #module_list > 0 then
            for _, m in ipairs(module_list) do
                local name = m.name:gsub("%.kaos$", "")
                local sz = m.size and string.format("%d B", m.size) or ""
                local item_bg = theme.list_hover or theme.field_bg or 0x00333333
                chaos_gl.rect_rounded(24, y, 440, 22, 3, item_bg)
                chaos_gl.text(32, y + 4, name, theme.text_primary or 0x00FFFFFF, 0, 0)
                chaos_gl.text(400, y + 4, sz, theme.text_secondary or 0x00AAAAAA, 0, 0)
                y = y + 26
            end
        else
            chaos_gl.text(24, y, "No modules in /system/modules/",
                          theme.text_secondary or 0x00AAAAAA, 0, 0)
        end
    end

    win:end_frame()

    -- ═══ Events ═══
    local content_y = 68
    for _, event in ipairs(win:poll_events()) do
        -- Forward to widgets
        local handled = false
        for _, w in ipairs(settings_widgets) do
            if w.on_input and w:on_input(event) then handled = true; break end
        end

        if not handled and event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            local mx, my = event.mouse_x, event.mouse_y

            -- Tab clicks
            if my >= 28 and my < 60 then
                local ttx = 0
                for i, label in ipairs(tabs) do
                    local tw = chaos_gl.text_width(label) + 20
                    if mx >= ttx and mx < ttx + tw then
                        active_tab = i
                        break
                    end
                    ttx = ttx + tw
                end
            end

        elseif event.type == EVENT_MOUSE_MOVE then
            if active_tab == 3 and event.button == 1 then
                local mx = event.mouse_x
                if mx >= 24 and mx < 284 then
                    volume_level = math.floor(((mx - 24) / 260) * 100 + 0.5)
                    volume_level = math.max(0, math.min(100, volume_level))
                    pcall(aios.audio.volume, volume_level)
                end
            end
        end
    end

    aios.os.sleep(50)
end

win:destroy()
