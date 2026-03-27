-- AIOS v2 — Window Manager
-- Lua library loaded by desktop shell. Handles rendering and input routing.
-- Centered dock with Win95-style Start button, system stats widget.

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

-- Traffic light button layout (must match titlebar.lua)
local TB_BTN_RADIUS = 6
local TB_BTN_Y_CENTER = 14
local TB_BTN_X_START = 12
local TB_BTN_SPACING = 20

-- Per-surface titlebar hover state
local tb_hover_state = {}  -- surface_id -> "close"/"minimize"/"maximize"/nil

function wm.get_titlebar_hover(surface)
    return tb_hover_state[surface]
end

local function update_titlebar_hover(surface, mx, my)
    local sx, sy = chaos_gl.surface_get_position(surface)
    local lx, ly = mx - sx, my - sy
    local btn_cy = TB_BTN_Y_CENTER
    local old = tb_hover_state[surface]
    local new_hover = nil

    if ly >= 0 and ly < TITLEBAR_H then
        for i, name in ipairs({"close", "minimize", "maximize"}) do
            local bx = TB_BTN_X_START + (i - 1) * TB_BTN_SPACING
            if (lx - bx)^2 + (ly - btn_cy)^2 <= (TB_BTN_RADIUS + 2)^2 then
                new_hover = name
                break
            end
        end
    end

    tb_hover_state[surface] = new_hover
end

-- ── Initialization ──────────────────────────────────

function wm.init()
    -- Use core.lua's theme system (reads saved preference from /system/theme.cfg)
    local ui = require("core")
    -- theme global is now set by core.lua's init
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
local START_BTN_W = 72       -- Win95-style Start button width
local START_BTN_H = 36       -- Start button height (matches DOCK_ICON)

-- ── Dock layout helper ──────────────────────────────

local function calc_dock_pill_x()
    local wins = aios.wm.get_windows()
    local app_w = #wins * (DOCK_ICON + DOCK_GAP) - (#wins > 0 and DOCK_GAP or 0)
    local sep_w = (#wins > 0) and (DOCK_GAP + 2 + DOCK_GAP) or 0
    local stats_w = 48
    local clock_w = 50
    local content_w = START_BTN_W + DOCK_GAP + app_w + sep_w + stats_w + DOCK_GAP + clock_w
    local pill_w = content_w + DOCK_PAD * 2
    return (1024 - pill_w) // 2
end

-- ── Dock (floating centered pill, Win95 Start button) ───

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
    local dock_bg = theme and theme.taskbar_bg or 0x002A2A32
    local dock_border = theme and theme.border or 0x00444455
    local dock_highlight = theme and theme.button_hover or 0x00505060
    local btn_bg = theme and theme.button_normal or 0x00383840
    local inactive_bg = theme and theme.button_disabled or 0x00484850

    -- Gather dock items: Start button + app icons + separator + stats + clock
    local wins = aios.wm.get_windows()

    -- Calculate total dock width
    -- [Start btn] [gap] [app1] [app2] ... [separator] [stats] [gap] [clock]
    local menu_w = START_BTN_W
    local app_w = #wins * (DOCK_ICON + DOCK_GAP) - (#wins > 0 and DOCK_GAP or 0)
    local sep_w = (#wins > 0) and (DOCK_GAP + 2 + DOCK_GAP) or 0
    local stats_w = 48
    local clock_str_val = ""
    local date_str_val = ""
    do
        local t = aios.os.time()
        if t and t.synced then
            local h = t.hour or 0
            local ampm = h >= 12 and "PM" or "AM"
            local h12 = h % 12
            if h12 == 0 then h12 = 12 end
            clock_str_val = string.format("%d:%02d %s", h12, t.min or 0, ampm)
            date_str_val = string.format("%02d/%02d/%04d", t.day or 0, t.month or 0, t.year or 0)
        else
            local millis = aios.os.millis()
            local secs = math.floor(millis / 1000)
            clock_str_val = string.format("%02d:%02d", math.floor(secs / 3600) % 24, math.floor(secs / 60) % 60)
        end
    end
    local clock_w = math.max(chaos_gl.text_width(clock_str_val), chaos_gl.text_width(date_str_val)) + 8

    local content_w = menu_w + DOCK_GAP + app_w + sep_w + stats_w + DOCK_GAP + clock_w
    local pill_w = content_w + DOCK_PAD * 2
    local pill_h = DOCK_ICON + DOCK_PAD
    local pill_x = (1024 - pill_w) // 2
    local pill_y = DOCK_H - pill_h - DOCK_MARGIN_B

    -- Draw pill background with border
    chaos_gl.rect_rounded(pill_x, pill_y, pill_w, pill_h, DOCK_RADIUS, dock_bg)
    chaos_gl.rect_rounded_outline(pill_x, pill_y, pill_w, pill_h, DOCK_RADIUS, dock_border, 1)
    -- Subtle top highlight
    chaos_gl.rect(pill_x + DOCK_RADIUS, pill_y + 1, pill_w - DOCK_RADIUS * 2, 1, dock_highlight)

    -- Layout items inside pill
    local ix = pill_x + DOCK_PAD
    local iy = pill_y + (pill_h - DOCK_ICON) // 2

    -- Start button (matches dock rounded style)
    local fh = chaos_gl.font_height(-1)
    local start_pressed = app_menu_visible
    local start_bg = start_pressed and accent or btn_bg

    chaos_gl.rect_rounded(ix, iy, START_BTN_W, START_BTN_H, 6, start_bg)

    -- AIOS logo icon (small stylized "A" in accent/white)
    local ico_c = start_pressed and 0x00FFFFFF or accent
    local ico_x = ix + 8
    local ico_y = iy + (START_BTN_H - 14) // 2
    chaos_gl.rect(ico_x, ico_y + 4, 2, 10, ico_c)
    chaos_gl.rect(ico_x + 2, ico_y + 2, 2, 4, ico_c)
    chaos_gl.rect(ico_x + 4, ico_y, 3, 3, ico_c)
    chaos_gl.rect(ico_x + 9, ico_y + 4, 2, 10, ico_c)
    chaos_gl.rect(ico_x + 7, ico_y + 2, 2, 4, ico_c)
    chaos_gl.rect(ico_x + 2, ico_y + 8, 7, 2, ico_c)

    -- "Start" text
    local start_text_c = start_pressed and 0x00FFFFFF or text_c
    local start_text = "Start"
    local tx = ix + 8 + 14 + 4
    local ty = iy + (START_BTN_H - fh) // 2
    chaos_gl.text(tx, ty, start_text, start_text_c, 0, 0)

    ix = ix + START_BTN_W + DOCK_GAP

    -- App icons
    for _, w in ipairs(wins) do
        local is_active = w.active
        local ibg = is_active and accent or inactive_bg
        chaos_gl.rect_rounded(ix, iy, DOCK_ICON, DOCK_ICON, 8, ibg)

        -- Try to find app icon texture
        local app_tex = -1
        for _, app in ipairs(app_list) do
            if app.name == w.title and app.tex >= 0 then
                app_tex = app.tex
                break
            end
        end

        if app_tex >= 0 then
            -- Blit icon centered in dock slot
            local tw, th = chaos_gl.texture_get_size(app_tex)
            local ox = ix + (DOCK_ICON - tw) // 2
            local oy = iy + (DOCK_ICON - th) // 2
            if chaos_gl.texture_has_alpha(app_tex) then
                chaos_gl.blit_alpha(ox, oy, tw, th, app_tex)
            else
                chaos_gl.blit_keyed(ox, oy, tw, th, app_tex, 0x00FF00FF)
            end
        else
            -- Fallback: first letter
            local title = w.title or "?"
            local letter = title:sub(1, 1):upper()
            local lw = chaos_gl.text_width(letter)
            chaos_gl.text(ix + (DOCK_ICON - lw) // 2, iy + (DOCK_ICON - fh) // 2, letter, 0x00FFFFFF, 0, 0)
        end

        -- Active/minimized indicators
        if w.minimized then
            -- Dim overlay for minimized
            chaos_gl.rect(ix, iy, DOCK_ICON, DOCK_ICON, 0x80000000)
        end
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

    -- Clock (time + date)
    if #date_str_val > 0 then
        chaos_gl.text(ix, iy + 2, clock_str_val, text_c, 0, 0)
        chaos_gl.text(ix, iy + fh + 2, date_str_val, sec_c, 0, 0)
    else
        chaos_gl.text(ix, iy + (DOCK_ICON - fh) // 2, clock_str_val, sec_c, 0, 0)
    end

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
    -- Widget: 48px wide, DOCK_ICON tall, 4 mini progress bars
    local w = 48
    local h = DOCK_ICON
    local bar_h = 6
    local gap = (h - bar_h * 4) // 5  -- evenly space 4 bars

    chaos_gl.rect_rounded(x, y, w, h, 6, 0x00333340)

    local bw = w - 8

    -- FPS bar (green) — scale: 0-62fps
    local fps_pct = math.min(1.0, stats_fps / 62)
    local by = y + gap
    chaos_gl.rect(x + 4, by, bw, bar_h, 0x00222222)
    if fps_pct > 0 then
        chaos_gl.rect(x + 4, by, math.max(1, math.floor(bw * fps_pct)), bar_h, 0x0000CC00)
    end

    -- CPU bar (red) — scheduler CPU usage 0-100%
    by = by + bar_h + gap
    local cpu_pct = math.min(1.0, aios.task.cpu_usage() / 100)
    chaos_gl.rect(x + 4, by, bw, bar_h, 0x00222222)
    if cpu_pct > 0 then
        chaos_gl.rect(x + 4, by, math.max(1, math.floor(bw * cpu_pct)), bar_h, 0x00EE4444)
    end

    -- Render bar (orange) — compose time, scale: 0-16ms
    by = by + bar_h + gap
    local compose_stats = chaos_gl.get_compose_stats()
    local render_pct = 0
    if compose_stats then
        render_pct = math.min(1.0, (compose_stats.compose_time_us or 0) / 16000)
    end
    chaos_gl.rect(x + 4, by, bw, bar_h, 0x00222222)
    if render_pct > 0 then
        chaos_gl.rect(x + 4, by, math.max(1, math.floor(bw * render_pct)), bar_h, 0x00FF8800)
    end

    -- MEM bar (blue)
    by = by + bar_h + gap
    local info = aios.os.meminfo()
    local mem_pct = 0
    if info then
        local rp = info.pmm_ram_pages or info.pmm_total_pages or 1
        if rp > 0 then
            mem_pct = (rp - (info.pmm_free_pages or 0)) / rp
        end
    end
    chaos_gl.rect(x + 4, by, bw, bar_h, 0x00222222)
    if mem_pct > 0 then
        chaos_gl.rect(x + 4, by, math.max(1, math.floor(bw * mem_pct)), bar_h, 0x004488FF)
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
    local content_w = START_BTN_W + DOCK_GAP + app_w + sep_w + stats_w + DOCK_GAP + clock_w
    local pill_w = content_w + DOCK_PAD * 2
    local pill_x = (1024 - pill_w) // 2

    local ix = pill_x + DOCK_PAD

    -- Start button
    if mx >= ix and mx < ix + START_BTN_W then
        wm._toggle_app_menu()
        return
    end
    ix = ix + START_BTN_W + DOCK_GAP

    -- App icons
    for _, w in ipairs(wins) do
        if mx >= ix and mx < ix + DOCK_ICON then
            aios.wm.toggle(w.surface)
            return
        end
        ix = ix + DOCK_ICON + DOCK_GAP
    end

    -- Separator
    if #wins > 0 then
        ix = ix + 2 + DOCK_GAP
    end

    -- Stats widget
    if mx >= ix and mx < ix + stats_w then
        aios.task.spawn("/apps/sysmon/main.lua", "System Monitor")
        return
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
    -- Titlebar = move (but not if clicking traffic light buttons)
    if ly < TITLEBAR_H then
        -- Check if click is on a traffic light button
        local btn_cy = TB_BTN_Y_CENTER
        for i = 0, 2 do
            local bx = TB_BTN_X_START + i * TB_BTN_SPACING
            if (lx - bx)^2 + (ly - btn_cy)^2 <= (TB_BTN_RADIUS + 2)^2 then
                return nil  -- let the click through to the app
            end
        end
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

    -- Track titlebar button hover on mouse move
    if hit and event.type == EVENT_MOUSE_MOVE then
        update_titlebar_hover(hit, event.mouse_x, event.mouse_y)
    end

    -- Clear hover for surfaces the mouse isn't over
    for surf, _ in pairs(tb_hover_state) do
        if surf ~= hit then
            tb_hover_state[surf] = nil
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

    local ok, entries = pcall(aios.io.listdir, "/apps")
    if not ok or not entries then return end

    -- Phase 1: Scan /apps/*/manifest.lua (directory-based apps)
    for _, entry in ipairs(entries) do
        if entry.is_dir and entry.name ~= "." and entry.name ~= ".." then
            local manifest_path = "/apps/" .. entry.name .. "/manifest.lua"
            local mok, manifest_fn = pcall(loadfile, manifest_path)
            if mok and manifest_fn then
                local rok, manifest = pcall(manifest_fn)
                if rok and type(manifest) == "table" and manifest.name and manifest.entry then
                    local entry_path = "/apps/" .. entry.name .. "/" .. manifest.entry
                    local tex = -1
                    if manifest.icon then
                        tex = chaos_gl.load_texture(manifest.icon)
                    end
                    app_list[#app_list + 1] = {
                        name = manifest.name,
                        path = entry_path,
                        tex = tex,
                        app_dir = "/apps/" .. entry.name,
                    }
                end
            end
        end
    end

    -- Phase 2: Backward compat — flat /apps/*.lua with @app tags
    for _, entry in ipairs(entries) do
        if not entry.is_dir and entry.name:match("%.lua$") then
            local path = "/apps/" .. entry.name
            local fok, fd = pcall(aios.io.open, path, "r")
            if fok and fd then
                local line = aios.io.read(fd, 256)
                aios.io.close(fd)
                if line then
                    local app_name = line:match('^%-%- @app name="([^"]+)"')
                    if app_name then
                        local app_icon = line:match('icon="([^"]+)"')
                        local tex = -1
                        if app_icon then
                            tex = chaos_gl.load_texture(app_icon)
                        end
                        app_list[#app_list + 1] = {name = app_name, path = path, tex = tex}
                    end
                end
            end
        end
    end
end

function wm._toggle_app_menu()
    if app_menu_visible then
        wm._close_app_menu()
    else
        wm._open_app_menu()
    end
end

-- Menu layout constants
local MENU_ITEM_H = 32
local MENU_SIDEBAR_W = 24
local MENU_W = 200
local MENU_SEP_H = 9      -- separator row height

-- Build the full menu item list (apps + separator + power items)
local function build_menu_items()
    local items = {}
    for _, app in ipairs(app_list) do
        items[#items + 1] = {kind = "app", name = app.name, path = app.path, tex = app.tex}
    end
    items[#items + 1] = {kind = "separator"}
    items[#items + 1] = {kind = "action", name = "Restart", action = "restart"}
    items[#items + 1] = {kind = "action", name = "Shut Down", action = "shutdown"}
    return items
end

local menu_items = {}

function wm._open_app_menu()
    menu_items = build_menu_items()
    if #menu_items == 0 then return end

    -- Calculate menu height
    local menu_h = 4  -- top/bottom padding
    for _, item in ipairs(menu_items) do
        if item.kind == "separator" then
            menu_h = menu_h + MENU_SEP_H
        else
            menu_h = menu_h + MENU_ITEM_H
        end
    end

    local menu_y = 768 - DOCK_H - menu_h - 4

    -- Position above the Start button in the dock pill
    local pill_x = calc_dock_pill_x()
    local menu_x = pill_x + DOCK_PAD

    app_menu_surface = chaos_gl.surface_create(MENU_W, menu_h, false)
    chaos_gl.surface_set_position(app_menu_surface, menu_x, menu_y)
    chaos_gl.surface_set_zorder(app_menu_surface, 101)
    chaos_gl.surface_set_visible(app_menu_surface, true)

    wm._draw_app_menu(-1)
    app_menu_visible = true
end

function wm._draw_app_menu(hover_idx)
    if not app_menu_surface then return end
    chaos_gl.surface_bind(app_menu_surface)
    local bg = theme and theme.menu_bg or 0x00333340
    local border = theme and theme.menu_border or 0x00555565
    local text_c = theme and theme.menu_text or 0x00FFFFFF
    local hover_c = theme and theme.menu_hover or 0x00444460
    local accent = theme and theme.accent or 0x00FF8800
    local sep_c = theme and theme.menu_separator or 0x00404048
    local mw, mh = chaos_gl.surface_get_size(app_menu_surface)
    local afh = chaos_gl.font_height(-1)

    -- Background with rounded border (matches dock style)
    chaos_gl.surface_clear(app_menu_surface, bg)
    chaos_gl.rect_rounded_outline(0, 0, mw, mh, 6, border, 1)

    -- Win95-style sidebar strip on the left
    chaos_gl.rect(2, 2, MENU_SIDEBAR_W, mh - 4, accent)
    -- Gradient-ish darker strip at bottom of sidebar
    local sidebar_dark = 0x00882200
    chaos_gl.rect(2, mh // 2, MENU_SIDEBAR_W, mh // 2 - 2, sidebar_dark)
    -- "AIOS" text vertically on sidebar (draw each letter stacked)
    local sidebar_text = "AIOS"
    local letter_h = afh + 2
    local text_start_y = mh - 4 - #sidebar_text * letter_h
    for ci = 1, #sidebar_text do
        local ch = sidebar_text:sub(ci, ci)
        local cw = chaos_gl.text_width(ch)
        chaos_gl.text(2 + (MENU_SIDEBAR_W - cw) // 2, text_start_y + (ci - 1) * letter_h, ch, 0x00FFFFFF, 0, 0)
    end

    -- Draw menu items
    local y = 2
    for i, item in ipairs(menu_items) do
        if item.kind == "separator" then
            -- Horizontal separator line
            local sep_y = y + MENU_SEP_H // 2
            chaos_gl.rect(MENU_SIDEBAR_W + 6, sep_y, mw - MENU_SIDEBAR_W - 10, 1, sep_c)
            y = y + MENU_SEP_H
        else
            -- Hoverable item
            if i == hover_idx then
                chaos_gl.rect_rounded(MENU_SIDEBAR_W + 2, y + 1, mw - MENU_SIDEBAR_W - 4, MENU_ITEM_H - 2, 4, hover_c)
            end

            local tx = MENU_SIDEBAR_W + 10

            if item.kind == "app" and item.tex and item.tex >= 0 then
                local iw, ih = chaos_gl.texture_get_size(item.tex)
                local iy = y + (MENU_ITEM_H - ih) // 2
                if chaos_gl.texture_has_alpha(item.tex) then
                    chaos_gl.blit_alpha(tx, iy, iw, ih, item.tex)
                else
                    chaos_gl.blit_keyed(tx, iy, iw, ih, item.tex, 0x00FF00FF)
                end
                tx = tx + iw + 8
            elseif item.kind == "action" then
                -- Power icons: small colored indicator
                local icon_y = y + (MENU_ITEM_H - 8) // 2
                if item.action == "shutdown" then
                    chaos_gl.rect(tx + 1, icon_y, 8, 8, 0x00FF4444)
                    chaos_gl.rect(tx + 4, icon_y - 2, 2, 4, 0x00FF4444)
                elseif item.action == "restart" then
                    chaos_gl.rect(tx + 1, icon_y, 8, 8, 0x0044CC44)
                    chaos_gl.rect(tx + 4, icon_y + 2, 4, 2, bg)
                end
                tx = tx + 14
            end

            local item_text_c = text_c
            if i == hover_idx then
                item_text_c = 0x00FFFFFF
            end
            chaos_gl.text(tx, y + (MENU_ITEM_H - afh) // 2, item.name, item_text_c, 0, 0)
            y = y + MENU_ITEM_H
        end
    end

    chaos_gl.surface_present(app_menu_surface)
end

local app_menu_hover = -1

-- Convert a mouse Y coordinate to menu item index
local function menu_hit_index(local_y)
    local y = 2
    for i, item in ipairs(menu_items) do
        local h = (item.kind == "separator") and MENU_SEP_H or MENU_ITEM_H
        if local_y >= y and local_y < y + h then
            if item.kind == "separator" then return -1 end
            return i
        end
        y = y + h
    end
    return -1
end

function wm._handle_app_menu_hover(event)
    if not app_menu_surface then return end
    local mx, my = chaos_gl.surface_get_position(app_menu_surface)
    local local_y = event.mouse_y - my
    local idx = menu_hit_index(local_y)
    if idx ~= app_menu_hover then
        app_menu_hover = idx
        wm._draw_app_menu(app_menu_hover)
    end
end

function wm._handle_app_menu_click(event)
    if not app_menu_surface then return end
    local mx, my = chaos_gl.surface_get_position(app_menu_surface)
    local local_y = event.mouse_y - my
    local idx = menu_hit_index(local_y)
    if idx >= 1 and idx <= #menu_items then
        local item = menu_items[idx]
        if item.kind == "app" then
            aios.task.spawn(item.path, item.name)
        elseif item.kind == "action" then
            if item.action == "shutdown" then
                aios.os.shutdown()
            elseif item.action == "restart" then
                aios.os.restart()
            end
        end
    end
    wm._close_app_menu()
end

function wm._close_app_menu()
    if app_menu_surface then
        chaos_gl.surface_destroy(app_menu_surface)
        app_menu_surface = nil
    end
    app_menu_visible = false
    app_menu_hover = -1
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
