-- AIOS v2 — Window Manager
-- Lua library loaded by desktop shell. Handles rendering and input routing.
-- macOS-inspired taskbar with centered dock, system stats widget.

local wm = {}

local taskbar_surface = nil
local cursor_surface = nil
local cursor_tex = nil
local app_menu_visible = false
local app_menu_surface = nil
local app_list = {}

-- Drag/resize state
local drag_surface = nil
local drag_mode = nil  -- "move", "resize_br", "resize_r", "resize_b"
local drag_ox, drag_oy = 0, 0
local drag_start_w, drag_start_h = 0, 0
local drag_start_mx, drag_start_my = 0, 0

-- Stats for widget
local stats_fps = 0
local stats_frame_count = 0
local stats_last_time = 0

-- Titlebar height apps use
local TITLEBAR_H = 28
local TASKBAR_H = 36
local RESIZE_BORDER = 6

-- ── Initialization ──────────────────────────────────

function wm.init()
    local theme_fn = loadfile("/system/themes/dark.lua")
    if theme_fn then
        theme = theme_fn()
    else
        theme = { taskbar_bg = 0x002A2A2A, text_primary = 0x00FFFFFF,
                   text_secondary = 0x00AAAAAA, accent = 0x00FF8800,
                   app_active_indicator = 0x00FF8800, button_hover = 0x004A4A4A,
                   menu_bg = 0x003A3A3A, menu_border = 0x00555555,
                   menu_text = 0x00FFFFFF, menu_hover = 0x00FF8800,
                   desktop_bg = 0x00283040 }
    end
    wm._init_taskbar()
    wm._init_cursor()
    wm._scan_apps()
    stats_last_time = aios.os.millis()
end

-- ── Dock constants ──────────────────────────────────

local DOCK_KEY = 0x00FF00FF  -- color key for transparency
local DOCK_H = 52            -- total surface height (pill + margin)
local DOCK_ICON = 36         -- icon size inside dock
local DOCK_GAP = 8           -- gap between icons
local DOCK_PAD = 12          -- padding inside pill
local DOCK_MARGIN_B = 6      -- margin from screen bottom
local DOCK_RADIUS = 12       -- pill corner radius

-- ── Dock layout helper ──────────────────────────────

local function calc_dock_pill_x()
    local wins = aios.wm.get_windows()
    local app_w = #wins * (DOCK_ICON + DOCK_GAP) - (#wins > 0 and DOCK_GAP or 0)
    local sep_w = (#wins > 0) and (DOCK_GAP + 2 + DOCK_GAP) or 0
    local stats_w = 48
    local clock_w = 50
    local content_w = DOCK_ICON + DOCK_GAP + app_w + sep_w + stats_w + DOCK_GAP + clock_w
    local pill_w = content_w + DOCK_PAD * 2
    return (1024 - pill_w) // 2
end

-- ── macOS-style Dock (floating centered pill) ───────

function wm._init_taskbar()
    taskbar_surface = chaos_gl.surface_create(1024, DOCK_H, false)
    chaos_gl.surface_set_position(taskbar_surface, 0, 768 - DOCK_H)
    chaos_gl.surface_set_zorder(taskbar_surface, 100)
    chaos_gl.surface_set_visible(taskbar_surface, true)
    chaos_gl.surface_set_color_key(taskbar_surface, true, DOCK_KEY)
end

function wm._draw_taskbar()
    if not taskbar_surface then return end
    chaos_gl.surface_bind(taskbar_surface)

    -- Clear to transparent (color key)
    chaos_gl.surface_clear(taskbar_surface, DOCK_KEY)

    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800
    local dock_bg = 0x002A2A32

    -- Gather dock items: menu + app icons + separator + stats + clock
    local wins = aios.wm.get_windows()

    -- Calculate total dock width
    -- [menu icon] [gap] [app1] [app2] ... [separator] [stats] [gap] [clock]
    local menu_w = DOCK_ICON
    local app_w = #wins * (DOCK_ICON + DOCK_GAP) - (#wins > 0 and DOCK_GAP or 0)
    local sep_w = (#wins > 0) and (DOCK_GAP + 2 + DOCK_GAP) or 0
    local stats_w = 48
    local clock_str_val = ""
    do
        local millis = aios.os.millis()
        local secs = math.floor(millis / 1000)
        local mins = math.floor(secs / 60) % 60
        local hrs = math.floor(secs / 3600) % 24
        clock_str_val = string.format("%02d:%02d", hrs, mins)
    end
    local clock_w = chaos_gl.text_width(clock_str_val) + 8

    local content_w = menu_w + DOCK_GAP + app_w + sep_w + stats_w + DOCK_GAP + clock_w
    local pill_w = content_w + DOCK_PAD * 2
    local pill_h = DOCK_ICON + DOCK_PAD
    local pill_x = (1024 - pill_w) // 2
    local pill_y = DOCK_H - pill_h - DOCK_MARGIN_B

    -- Draw pill background
    chaos_gl.rect_rounded(pill_x, pill_y, pill_w, pill_h, DOCK_RADIUS, dock_bg)
    -- Subtle top highlight
    chaos_gl.rect(pill_x + DOCK_RADIUS, pill_y, pill_w - DOCK_RADIUS * 2, 1, 0x00444450)

    -- Layout items inside pill
    local ix = pill_x + DOCK_PAD
    local iy = pill_y + (pill_h - DOCK_ICON) // 2

    -- Menu button
    chaos_gl.rect_rounded(ix, iy, DOCK_ICON, DOCK_ICON, 6, 0x00383840)
    local a_w = chaos_gl.text_width("A")
    local fh = chaos_gl.font_height(-1)
    chaos_gl.text(ix + (DOCK_ICON - a_w) // 2, iy + (DOCK_ICON - fh) // 2, "A", accent, 0, 0)
    ix = ix + DOCK_ICON + DOCK_GAP

    -- App icons
    for _, w in ipairs(wins) do
        local is_active = w.active
        local ibg = is_active and accent or 0x00484850
        chaos_gl.rect_rounded(ix, iy, DOCK_ICON, DOCK_ICON, 6, ibg)

        -- First letter of title
        local title = w.title or "?"
        local letter = title:sub(1, 1):upper()
        local lw = chaos_gl.text_width(letter)
        chaos_gl.text(ix + (DOCK_ICON - lw) // 2, iy + (DOCK_ICON - fh) // 2, letter, 0x00FFFFFF, 0, 0)

        -- Active dot below pill
        if is_active then
            chaos_gl.circle(ix + DOCK_ICON // 2, pill_y + pill_h + 3, 2, accent)
        end

        ix = ix + DOCK_ICON + DOCK_GAP
    end

    -- Separator (thin vertical line)
    if #wins > 0 then
        chaos_gl.rect(ix, iy + 4, 2, DOCK_ICON - 8, 0x00555560)
        ix = ix + 2 + DOCK_GAP
    end

    -- Stats widget
    wm._draw_stats_widget(ix, iy)
    ix = ix + stats_w + DOCK_GAP

    -- Clock
    chaos_gl.text(ix, iy + (DOCK_ICON - fh) // 2, clock_str_val, sec_c, 0, 0)

    chaos_gl.surface_present(taskbar_surface)
end

-- ── Stats Widget (FPS / CPU / MEM mini bars) ────────

function wm._update_stats()
    stats_frame_count = stats_frame_count + 1
    local now = aios.os.millis()
    local elapsed = now - stats_last_time
    if elapsed >= 1000 then
        stats_fps = math.floor(stats_frame_count * 1000 / elapsed)
        stats_frame_count = 0
        stats_last_time = now
    end
end

function wm._draw_stats_widget(x, y)
    -- Widget: 48px wide, DOCK_ICON tall, 3 mini progress bars
    local w = 48
    local h = DOCK_ICON
    local bar_h = 8
    local gap = (h - bar_h * 3) // 4  -- evenly space 3 bars

    chaos_gl.rect_rounded(x, y, w, h, 6, 0x00333340)

    -- FPS bar (green) — scale: 0-62fps
    local fps_pct = math.min(1.0, stats_fps / 62)
    local by = y + gap
    chaos_gl.rect_rounded(x + 4, by, w - 8, bar_h, 2, 0x00222222)
    if fps_pct > 0 then
        chaos_gl.rect(x + 4, by, math.max(1, math.floor((w - 8) * fps_pct)), bar_h, 0x0000CC00)
    end

    -- CPU bar (orange)
    by = by + bar_h + gap
    local compose_stats = chaos_gl.get_compose_stats()
    local cpu_pct = 0
    if compose_stats then
        cpu_pct = math.min(1.0, (compose_stats.compose_time_us or 0) / 16000)
    end
    chaos_gl.rect_rounded(x + 4, by, w - 8, bar_h, 2, 0x00222222)
    if cpu_pct > 0 then
        chaos_gl.rect(x + 4, by, math.max(1, math.floor((w - 8) * cpu_pct)), bar_h, 0x00FF8800)
    end

    -- MEM bar (blue)
    local info = aios.os.meminfo()
    local mem_pct = 0
    if info and info.pmm_total_pages and info.pmm_total_pages > 0 then
        mem_pct = 1.0 - (info.pmm_free_pages / info.pmm_total_pages)
    end
    by = by + bar_h + gap
    chaos_gl.rect_rounded(x + 4, by, w - 8, bar_h, 2, 0x00222222)
    if mem_pct > 0 then
        chaos_gl.rect(x + 4, by, math.max(1, math.floor((w - 8) * mem_pct)), bar_h, 0x004488FF)
    end
end

function wm._handle_taskbar_click(event)
    local mx = event.mouse_x

    -- Recalculate pill layout to determine click targets
    local wins = aios.wm.get_windows()
    local app_w = #wins * (DOCK_ICON + DOCK_GAP) - (#wins > 0 and DOCK_GAP or 0)
    local sep_w = (#wins > 0) and (DOCK_GAP + 2 + DOCK_GAP) or 0
    local stats_w = 48
    local clock_w = 50
    local content_w = DOCK_ICON + DOCK_GAP + app_w + sep_w + stats_w + DOCK_GAP + clock_w
    local pill_w = content_w + DOCK_PAD * 2
    local pill_x = (1024 - pill_w) // 2

    local ix = pill_x + DOCK_PAD

    -- Menu button
    if mx >= ix and mx < ix + DOCK_ICON then
        wm._toggle_app_menu()
        return
    end
    ix = ix + DOCK_ICON + DOCK_GAP

    -- App icons
    for _, w in ipairs(wins) do
        if mx >= ix and mx < ix + DOCK_ICON then
            aios.wm.toggle(w.surface)
            return
        end
        ix = ix + DOCK_ICON + DOCK_GAP
    end
end

-- ── Cursor ──────────────────────────────────────────

function wm._init_cursor()
    cursor_surface = chaos_gl.surface_create(24, 24, false)
    chaos_gl.surface_set_zorder(cursor_surface, 999)
    chaos_gl.surface_set_visible(cursor_surface, true)
    chaos_gl.surface_set_color_key(cursor_surface, true, 0x00FF00FF)

    cursor_tex = chaos_gl.load_texture("/system/icons/cursor_24.raw")

    chaos_gl.surface_bind(cursor_surface)
    chaos_gl.surface_clear(cursor_surface, 0x00FF00FF)
    if cursor_tex and cursor_tex >= 0 then
        chaos_gl.blit_keyed(0, 0, 24, 24, cursor_tex, 0x00FF00FF)
    else
        for row = 0, 17 do
            local w = row // 2 + 1
            for col = 0, w - 1 do
                chaos_gl.pixel(col, row, 0x00FFFFFF)
            end
            chaos_gl.pixel(w, row, 0x00333333)
        end
    end
    chaos_gl.surface_present(cursor_surface)
end

function wm._update_cursor(mx, my)
    if not cursor_surface then return end
    mx = math.max(0, math.min(1023, mx))
    my = math.max(0, math.min(767, my))
    chaos_gl.surface_set_position(cursor_surface, mx, my)
end

-- ── Window Drag & Resize ────────────────────────────

local function get_resize_zone(surface, mx, my)
    local sx, sy = chaos_gl.surface_get_position(surface)
    local sw, sh = chaos_gl.surface_get_size(surface)
    local lx, ly = mx - sx, my - sy

    -- Bottom-right corner
    if lx >= sw - RESIZE_BORDER and ly >= sh - RESIZE_BORDER then
        return "resize_br"
    end
    -- Right edge
    if lx >= sw - RESIZE_BORDER and ly >= TITLEBAR_H then
        return "resize_r"
    end
    -- Bottom edge
    if ly >= sh - RESIZE_BORDER and lx >= RESIZE_BORDER then
        return "resize_b"
    end
    -- Titlebar = move
    if ly < TITLEBAR_H then
        return "move"
    end
    return nil
end

-- ── Input Routing ───────────────────────────────────

function wm.route_mouse(event)
    if event.mouse_x and event.mouse_y then
        wm._update_cursor(event.mouse_x, event.mouse_y)
    end

    -- Handle active drag/resize
    if drag_surface and event.type == EVENT_MOUSE_MOVE then
        local mx, my = event.mouse_x, event.mouse_y
        if drag_mode == "move" then
            local nx, ny = mx - drag_ox, my - drag_oy
            nx, ny = aios.wm.clamp_position(drag_surface, nx, ny)
            chaos_gl.surface_set_position(drag_surface, nx, ny)
        elseif drag_mode == "resize_br" or drag_mode == "resize_r" or drag_mode == "resize_b" then
            local dw = mx - drag_start_mx
            local dh = my - drag_start_my
            local nw = drag_start_w
            local nh = drag_start_h
            if drag_mode ~= "resize_b" then nw = math.max(160, drag_start_w + dw) end
            if drag_mode ~= "resize_r" then nh = math.max(100, drag_start_h + dh) end
            chaos_gl.surface_resize(drag_surface, nw, nh)
        end
        return drag_surface  -- consume event
    end

    if drag_surface and event.type == EVENT_MOUSE_UP then
        drag_surface = nil
        drag_mode = nil
        return nil
    end

    -- Dock hit (bottom DOCK_H pixels)
    if event.mouse_y and event.mouse_y >= (768 - DOCK_H) then
        return "taskbar"
    end

    -- App menu hit
    if app_menu_visible and app_menu_surface then
        local amx, amy = chaos_gl.surface_get_position(app_menu_surface)
        local amw, amh = chaos_gl.surface_get_size(app_menu_surface)
        if event.mouse_x >= amx and event.mouse_x < amx + amw and
           event.mouse_y >= amy and event.mouse_y < amy + amh then
            return "app_menu"
        end
    end

    -- Hit-test windows (front to back by z-order)
    local wins = aios.wm.get_windows()
    local hit = nil
    local hit_z = -1
    for _, w in ipairs(wins) do
        if not w.minimized then
            local sx, sy = chaos_gl.surface_get_position(w.surface)
            local sw, sh = chaos_gl.surface_get_size(w.surface)
            if event.mouse_x >= sx and event.mouse_x < sx + sw and
               event.mouse_y >= sy and event.mouse_y < sy + sh then
                local z = chaos_gl.surface_get_zorder(w.surface)
                if z > hit_z then
                    hit = w.surface
                    hit_z = z
                end
            end
        end
    end

    -- Start drag/resize on mouse down in titlebar or edges
    if hit and event.type == EVENT_MOUSE_DOWN then
        aios.wm.focus(hit)
        local zone = get_resize_zone(hit, event.mouse_x, event.mouse_y)
        if zone == "move" then
            local sx, sy = chaos_gl.surface_get_position(hit)
            drag_surface = hit
            drag_mode = "move"
            drag_ox = event.mouse_x - sx
            drag_oy = event.mouse_y - sy
            return hit  -- don't pass click to app for titlebar
        elseif zone then
            local sw, sh = chaos_gl.surface_get_size(hit)
            drag_surface = hit
            drag_mode = zone
            drag_start_w = sw
            drag_start_h = sh
            drag_start_mx = event.mouse_x
            drag_start_my = event.mouse_y
            return hit
        end
    end

    return hit
end

function wm.translate_event(event, surface)
    local sx, sy = chaos_gl.surface_get_position(surface)
    local translated = {}
    for k, v in pairs(event) do translated[k] = v end
    translated.mouse_x = event.mouse_x - sx
    translated.mouse_y = event.mouse_y - sy
    return translated
end

function wm.route_keyboard(event)
    if event.type == EVENT_KEY_DOWN then
        if event.alt and event.key == 62 then
            local active = aios.wm.get_active()
            if active then
                aios.wm.unregister(active)
            end
            return nil
        end
        if event.alt and event.key == 15 then
            wm._cycle_focus(event.shift)
            return nil
        end
    end
    return aios.wm.get_active()
end

function wm._cycle_focus(reverse)
    local wins = aios.wm.get_windows()
    local visible = {}
    for _, w in ipairs(wins) do
        if not w.minimized then
            visible[#visible + 1] = w
        end
    end
    if #visible <= 1 then return end

    local active = aios.wm.get_active()
    local idx = 1
    for i, w in ipairs(visible) do
        if w.surface == active then idx = i; break end
    end

    if reverse then
        idx = ((idx - 2) % #visible) + 1
    else
        idx = (idx % #visible) + 1
    end

    aios.wm.focus(visible[idx].surface)
end

-- ── App Menu ────────────────────────────────────────

function wm._scan_apps()
    app_list = {}
    -- Recursively scan entire filesystem for .lua files with @app metadata
    local function scan_dir(dir)
        local ok, entries = pcall(aios.io.listdir, dir)
        if not ok or not entries then return end
        for _, entry in ipairs(entries) do
            if not entry.name or entry.name == "." or entry.name == ".." then
                -- skip
            else
                local path = (dir == "/") and ("/" .. entry.name) or (dir .. "/" .. entry.name)
                if entry.is_dir then
                    scan_dir(path)
                elseif entry.name:match("%.lua$") then
                    local fok, fd = pcall(aios.io.open, path, "r")
                    if fok and fd then
                        local line = aios.io.read(fd, 256)
                        aios.io.close(fd)
                        if line then
                            local app_name = line:match('^%-%- @app name="([^"]+)"')
                            if app_name then
                                app_list[#app_list + 1] = {name = app_name, path = path}
                            end
                        end
                    end
                end
            end
        end
    end
    scan_dir("/")
end

function wm._toggle_app_menu()
    if app_menu_visible then
        wm._close_app_menu()
    else
        wm._open_app_menu()
    end
end

function wm._open_app_menu()
    if #app_list == 0 then return end
    local item_h = 28
    local menu_w = 160
    local menu_h = #app_list * item_h + 4
    local menu_y = 768 - DOCK_H - menu_h - 4

    -- Position above the A button in the dock pill
    local pill_x = calc_dock_pill_x()
    local menu_x = pill_x + DOCK_PAD

    app_menu_surface = chaos_gl.surface_create(menu_w, menu_h, false)
    chaos_gl.surface_set_position(app_menu_surface, menu_x, menu_y)
    chaos_gl.surface_set_zorder(app_menu_surface, 101)
    chaos_gl.surface_set_visible(app_menu_surface, true)

    wm._draw_app_menu(-1)
    app_menu_visible = true
end

function wm._draw_app_menu(hover_idx)
    if not app_menu_surface then return end
    chaos_gl.surface_bind(app_menu_surface)
    local bg = theme and theme.menu_bg or 0x003A3A3A
    local border = theme and theme.menu_border or 0x00555555
    local text_c = theme and theme.menu_text or 0x00FFFFFF
    local hover_c = theme and theme.menu_hover or 0x00FF8800
    local mw, mh = chaos_gl.surface_get_size(app_menu_surface)

    chaos_gl.surface_clear(app_menu_surface, bg)
    chaos_gl.rect_outline(0, 0, mw, mh, border, 1)

    for i, app in ipairs(app_list) do
        local y = (i - 1) * 28 + 2
        if i == hover_idx then
            chaos_gl.rect(2, y, mw - 4, 28, hover_c)
        end
        local afh = chaos_gl.font_height(-1)
        chaos_gl.text(12, y + (28 - afh) // 2, app.name, text_c, 0, 0)
    end

    chaos_gl.surface_present(app_menu_surface)
end

function wm._handle_app_menu_click(event)
    if not app_menu_surface then return end
    local mx, my = chaos_gl.surface_get_position(app_menu_surface)
    local local_y = event.mouse_y - my
    local idx = math.floor((local_y - 2) / 28) + 1
    if idx >= 1 and idx <= #app_list then
        local app = app_list[idx]
        aios.task.spawn(app.path, app.name)
    end
    wm._close_app_menu()
end

function wm._close_app_menu()
    if app_menu_surface then
        chaos_gl.surface_destroy(app_menu_surface)
        app_menu_surface = nil
    end
    app_menu_visible = false
end

-- ── Desktop click handler ───────────────────────────

function wm._handle_desktop_click(event)
    if app_menu_visible then
        wm._close_app_menu()
    end
end

-- ── Accessors ───────────────────────────────────────

function wm.get_taskbar_surface()
    return taskbar_surface
end

function wm.get_cursor_surface()
    return cursor_surface
end

return wm
