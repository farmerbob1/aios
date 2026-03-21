-- @app name="Settings" icon="/system/icons/settings_48.raw"
-- AIOS v2 — Settings Application

local surface = chaos_gl.surface_create(450, 350, false)
chaos_gl.surface_set_position(surface, 150, 100)
chaos_gl.surface_set_visible(surface, true)
aios.wm.register(surface, {
    title = "Settings",
    task_id = aios.task.self().id,
})

local active_tab = 1
local tabs = {"Appearance", "System", "Modules"}

local running = true
while running do
    chaos_gl.surface_bind(surface)
    local bg = theme and theme.window_bg or 0x002D2D2D
    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800
    local titlebar_bg = theme and theme.titlebar_bg or 0x003C3C3C
    chaos_gl.surface_clear(surface, bg)

    -- Title bar
    chaos_gl.rect(0, 0, 450, 28, titlebar_bg)
    chaos_gl.text(8, 6, "Settings", text_c, 0, 0)
    chaos_gl.rect(450 - 28, 0, 28, 28, 0x00FF4444)
    chaos_gl.text(450 - 20, 6, "X", 0x00FFFFFF, 0, 0)

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

    chaos_gl.surface_present(surface)

    -- Events
    local event = aios.wm.poll_event(surface)
    while event do
        if event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            local mx, my = event.mouse_x, event.mouse_y

            -- Close button
            if mx >= 450 - 28 and my < 28 then
                running = false
            -- Tab clicks
            elseif my >= 28 and my < 60 then
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
        elseif event.type == EVENT_KEY_DOWN then
            if event.key == 1 then running = false end
        end
        event = aios.wm.poll_event(surface)
    end

    aios.os.sleep(100)
end

aios.wm.unregister(surface)
chaos_gl.surface_destroy(surface)
