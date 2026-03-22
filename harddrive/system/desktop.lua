-- AIOS v2 — Desktop Shell (Phase 10)
-- First Lua task spawned. Hosts the WM event loop.

print("[desktop] Loading WM...")
local wm = require("wm")

-- Enable GUI input mode (mouse/keyboard events flow to input queue)
aios.input.set_gui_mode(true)

-- Initialize WM (taskbar, cursor, app scan)
wm.init()

-- Create desktop surface (z=0, fullscreen)
local desktop_surface = chaos_gl.surface_create(1024, 768, false)
chaos_gl.surface_set_position(desktop_surface, 0, 0)
chaos_gl.surface_set_zorder(desktop_surface, 0)
chaos_gl.surface_set_visible(desktop_surface, true)

-- Load desktop icons config
local ok, icon_defs = pcall(require, "desktop_icons")
if not ok then icon_defs = {} end

-- Load icon textures
local desktop_icons = {}
for _, def in ipairs(icon_defs) do
    local tex = chaos_gl.load_texture(def.icon)
    desktop_icons[#desktop_icons + 1] = {
        name = def.name,
        app = def.app,
        tex = tex,
        x = 0, y = 0,
    }
end

-- Position icons in a grid on the left side
local icon_start_x = 24
local icon_start_y = 24
local icon_spacing = 90
for i, ic in ipairs(desktop_icons) do
    ic.x = icon_start_x
    ic.y = icon_start_y + (i - 1) * icon_spacing
end

-- Load system TTF font
local sys_font = chaos_gl.font_load("/system/fonts/Inter-Regular.ttf", 14)
if sys_font >= 0 then
    chaos_gl.set_font(sys_font)
    print("[desktop] System font loaded (handle " .. sys_font .. ")")
else
    print("[desktop] WARNING: TTF font load failed, using bitmap fallback")
end

-- Destroy boot splash
chaos_gl.boot_splash_destroy()

print("[desktop] Shell started")

-- ── Selection & double-click tracking ─────────────

local selected_icon = 0
local last_click_time = 0
local last_click_x = 0
local last_click_y = 0
local DOUBLE_CLICK_MS = 400
local DOUBLE_CLICK_DIST = 8

local function is_double_click(x, y)
    local now = aios.os.millis()
    local dt = now - last_click_time
    local dx = math.abs(x - last_click_x)
    local dy = math.abs(y - last_click_y)
    last_click_time = now
    last_click_x = x
    last_click_y = y
    return dt < DOUBLE_CLICK_MS and dx < DOUBLE_CLICK_DIST and dy < DOUBLE_CLICK_DIST
end

local function hit_test_icon(mx, my)
    for i, ic in ipairs(desktop_icons) do
        if mx >= ic.x and mx < ic.x + 48 and
           my >= ic.y and my < ic.y + 72 then
            return i
        end
    end
    return 0
end

-- ── Draw desktop ──────────────────────────────────

local dirty = true

local function draw_desktop()
    chaos_gl.surface_bind(desktop_surface)
    local bg = theme and theme.desktop_bg or 0x00283040
    chaos_gl.surface_clear(desktop_surface, bg)
    local accent = theme and theme.accent or 0x00FF8800
    local text_c = theme and theme.text_primary or 0x00FFFFFF

    for i, ic in ipairs(desktop_icons) do
        -- Selection highlight
        if i == selected_icon then
            chaos_gl.rect_rounded(ic.x - 4, ic.y - 4, 56, 80, 4, 0x40FFFFFF)
        end

        if ic.tex and ic.tex >= 0 then
            local tw, th = chaos_gl.texture_get_size(ic.tex)
            local ox = ic.x + (48 - tw) // 2
            local oy = ic.y + (48 - th) // 2
            if chaos_gl.texture_has_alpha(ic.tex) then
                chaos_gl.blit_alpha(ox, oy, tw, th, ic.tex)
            else
                chaos_gl.blit_keyed(ox, oy, tw, th, ic.tex, 0x00FF00FF)
            end
        else
            chaos_gl.rect(ic.x, ic.y, 48, 48, 0x00444466)
        end

        -- Label — highlight bg if selected
        local lbl_w = chaos_gl.text_width(ic.name)
        local lbl_x = ic.x + (48 - lbl_w) // 2
        if i == selected_icon then
            chaos_gl.rect_rounded(lbl_x - 3, ic.y + 50, lbl_w + 6, 18, 3, accent)
            chaos_gl.text(lbl_x, ic.y + 52, ic.name, 0x00FFFFFF, 0, 0)
        else
            chaos_gl.text(lbl_x, ic.y + 52, ic.name, text_c, 0, 0)
        end
    end

    chaos_gl.surface_present(desktop_surface)
end

draw_desktop()

-- ── Main event loop ───────────────────────────────

local running = true
local frame_count = 0
local event_count = 0
local last_heartbeat = aios.os.millis()
local last_taskbar = 0
local taskbar_dirty = true

while running do
    if dirty then
        draw_desktop()
        dirty = false
    end

    local now = aios.os.millis()
    wm._update_stats()
    if taskbar_dirty or now - last_taskbar >= 1000 then
        wm._draw_taskbar()
        last_taskbar = now
        taskbar_dirty = false
    end

    chaos_gl.compose(0)

    local event = aios.input.poll()
    while event do
        event_count = event_count + 1
        if event.type == EVENT_KEY_DOWN or event.type == EVENT_KEY_UP then
            local target = wm.route_keyboard(event)
            if target then
                aios.wm.push_event(target, event)
                -- Generate EVENT_KEY_CHAR for printable characters
                if event.type == EVENT_KEY_DOWN and event.char and event.char ~= "" then
                    aios.wm.push_event(target, {
                        type = EVENT_KEY_CHAR,
                        char = event.char,
                        key = event.key,
                    })
                end
            end
        else
            local target = wm.route_mouse(event)
            if target == "taskbar" then
                if event.type == EVENT_MOUSE_DOWN then
                    wm._handle_taskbar_click(event)
                    taskbar_dirty = true
                end
            elseif target == "app_menu" then
                if event.type == EVENT_MOUSE_DOWN then
                    wm._handle_app_menu_click(event)
                    taskbar_dirty = true
                elseif event.type == EVENT_MOUSE_MOVE then
                    wm._handle_app_menu_hover(event)
                end
            elseif target then
                local translated = wm.translate_event(event, target)
                aios.wm.push_event(target, translated)
            else
                -- Click on desktop background
                if event.type == EVENT_MOUSE_DOWN then
                    local hit_idx = hit_test_icon(event.mouse_x, event.mouse_y)
                    if hit_idx > 0 then
                        if selected_icon == hit_idx and is_double_click(event.mouse_x, event.mouse_y) then
                            aios.task.spawn(desktop_icons[hit_idx].app, desktop_icons[hit_idx].name)
                            taskbar_dirty = true
                        else
                            selected_icon = hit_idx
                            is_double_click(event.mouse_x, event.mouse_y) -- reset timer
                        end
                    else
                        selected_icon = 0
                        wm._handle_desktop_click(event)
                    end
                    dirty = true
                end
            end
        end
        event = aios.input.poll()
    end

    collectgarbage("step", 1)

    frame_count = frame_count + 1
    if now - last_heartbeat >= 30000 then
        print(string.format("[desktop] heartbeat: %d frames, %d events, uptime=%ds",
              frame_count, event_count, now // 1000))
        last_heartbeat = now
    end

    aios.os.sleep(16)
end
