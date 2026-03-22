-- AIOS v2 — Settings Application
local AppWindow = require("appwindow")
local win = AppWindow.new("Settings", 450, 350, {x=150, y=100})

local active_tab = 1
local tabs = {"Appearance", "System", "Modules"}

while win:is_running() do
    win:begin_frame()

    local bg = win:color("window_bg", 0x002D2D2D)
    local text_c = win:color("text_primary", 0x00FFFFFF)
    local sec_c = win:color("text_secondary", 0x00AAAAAA)
    local accent = win:color("accent", 0x00FF8800)

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

    -- Tab content
    local content_y = 64
    if active_tab == 1 then
        -- Appearance
        chaos_gl.text(16, content_y + 8, "Theme", text_c, 0, 0)

        local dark_bg = 0x00444444
        chaos_gl.rect_rounded(16, content_y + 32, 120, 32, 4, dark_bg)
        chaos_gl.text(32, content_y + 40, "Dark Theme", text_c, 0, 0)

        chaos_gl.rect_rounded(150, content_y + 32, 120, 32, 4, dark_bg)
        chaos_gl.text(166, content_y + 40, "Light Theme", text_c, 0, 0)

        chaos_gl.text(16, content_y + 80, "Accent Color: " ..
            string.format("0x%08X", accent), sec_c, 0, 0)

    elseif active_tab == 2 then
        -- System info
        local info = aios.os.meminfo()
        local y = content_y + 8
        chaos_gl.text(16, y, "System Information", text_c, 0, 0)
        y = y + 24

        if info then
            local total_kb = (info.pmm_total_pages or 0) * 4
            local free_kb = (info.pmm_free_pages or 0) * 4
            local used_kb = total_kb - free_kb
            chaos_gl.text(16, y, string.format("Physical: %d KB total, %d KB free", total_kb, free_kb), sec_c, 0, 0)
            y = y + 20
            chaos_gl.text(16, y, string.format("Heap: %d KB used, %d KB free",
                (info.heap_used or 0) // 1024, (info.heap_free or 0) // 1024), sec_c, 0, 0)
            y = y + 20

            -- Memory bar
            local bar_w = 200
            local pct = total_kb > 0 and (used_kb / total_kb) or 0
            chaos_gl.rect(16, y, bar_w, 12, 0x00333333)
            chaos_gl.rect(16, y, math.floor(bar_w * pct), 12, accent)
            chaos_gl.text(bar_w + 24, y, string.format("%.0f%%", pct * 100), sec_c, 0, 0)
            y = y + 24
        end

        local millis = aios.os.millis()
        local secs = math.floor(millis / 1000)
        local mins = math.floor(secs / 60)
        local hrs = math.floor(mins / 60)
        chaos_gl.text(16, y, string.format("Uptime: %dh %dm %ds", hrs, mins % 60, secs % 60), sec_c, 0, 0)
        y = y + 20

        local fs = aios.os.fsinfo()
        if fs then
            local total_b = (fs.total_blocks or 0) * 4
            local free_b = (fs.free_blocks or 0) * 4
            chaos_gl.text(16, y, string.format("ChaosFS: %d KB total, %d KB free", total_b, free_b), sec_c, 0, 0)
            y = y + 20
            chaos_gl.text(16, y, string.format("Free inodes: %d", fs.free_inodes or 0), sec_c, 0, 0)
        end

    elseif active_tab == 3 then
        -- KAOS modules — list .kaos files on disk
        chaos_gl.text(16, content_y + 8, "Kernel Modules (on disk)", text_c, 0, 0)
        local y = content_y + 32
        local ok2, mods = pcall(aios.io.listdir, "/system/modules")
        local count = 0
        if ok2 and mods then
            for _, m in ipairs(mods) do
                if m.name and m.name:match("%.kaos$") then
                    local name = m.name:gsub("%.kaos$", "")
                    local size_str = m.size and string.format("  (%d B)", m.size) or ""
                    chaos_gl.text(24, y, name .. size_str, sec_c, 0, 0)
                    y = y + 20
                    count = count + 1
                end
            end
        end
        if count == 0 then
            chaos_gl.text(24, y, "(no modules found)", sec_c, 0, 0)
        end
    end

    win:end_frame()

    -- Events
    for _, event in ipairs(win:poll_events()) do
        if event.type == EVENT_MOUSE_DOWN and event.button == 1 then
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
            -- Theme buttons
            elseif active_tab == 1 then
                if my >= content_y + 32 and my < content_y + 64 then
                    if mx >= 16 and mx < 136 then
                        local ui = require("core")
                        ui.load_theme("/system/themes/dark.lua")
                    elseif mx >= 150 and mx < 270 then
                        local ui = require("core")
                        ui.load_theme("/system/themes/light.lua")
                    end
                end
            end
        end
    end

    aios.os.sleep(100)
end

win:destroy()
