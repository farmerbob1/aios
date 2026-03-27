-- AIOS v2 — Desktop Shell (Phase 10)
-- First Lua task spawned. Hosts the WM event loop.
-- Desktop = /desktop/ folder. Supports drag-and-drop, context menus, trash.

print("[desktop] Loading WM...")
local wm = require("wm")
local Menu = require("menu")
local filetypes = require("filetypes")

-- Enable GUI input mode (mouse/keyboard events flow to input queue)
aios.input.set_gui_mode(true)

-- Initialize WM (taskbar, cursor, app scan)
wm.init()

-- Create desktop surface (z=0, fullscreen)
local desktop_surface = chaos_gl.surface_create(1024, 768, false)
chaos_gl.surface_set_position(desktop_surface, 0, 0)
chaos_gl.surface_set_zorder(desktop_surface, 0)
chaos_gl.surface_set_visible(desktop_surface, true)

-- ── Constants ────────────────────────────────────────
local ICON_W = 48
local ICON_H = 72
local GRID_X = 80       -- grid cell width
local GRID_Y = 90       -- grid cell height
local GRID_ORIGIN_X = 24
local GRID_ORIGIN_Y = 24
local SCREEN_W = 1024
local SCREEN_H = 768
local TASKBAR_H = 44
local DOUBLE_CLICK_MS = 400
local DOUBLE_CLICK_DIST = 8
local DRAG_THRESHOLD = 5
local DESKTOP_PATH = "/desktop"
local LAYOUT_PATH = "/desktop/.layout"

-- ── Desktop icon scanning ────────────────────────────

local desktop_icons = {}

-- Load saved layout positions
local function load_layout()
    local fn = loadfile(LAYOUT_PATH)
    if fn then
        local ok, tbl = pcall(fn)
        if ok and type(tbl) == "table" then return tbl end
    end
    return {}
end

-- Save current layout positions
local function save_layout()
    local parts = {"return {\n"}
    for _, ic in ipairs(desktop_icons) do
        local key = ic.filename or ic.name
        parts[#parts + 1] = string.format("    [%q] = { x = %d, y = %d },\n", key, ic.x, ic.y)
    end
    parts[#parts + 1] = "}\n"
    aios.io.writefile(LAYOUT_PATH, table.concat(parts))
end

-- Auto-position: find next free grid slot
local function auto_position_icons()
    local occupied = {}
    for _, ic in ipairs(desktop_icons) do
        if ic.x > 0 or ic.y > 0 then
            local key = ic.x .. "," .. ic.y
            occupied[key] = true
        end
    end
    local col, row = 0, 0
    local max_rows = (SCREEN_H - TASKBAR_H - GRID_ORIGIN_Y) // GRID_Y
    for _, ic in ipairs(desktop_icons) do
        if ic.x == 0 and ic.y == 0 then
            while true do
                local px = GRID_ORIGIN_X + col * GRID_X
                local py = GRID_ORIGIN_Y + row * GRID_Y
                local key = px .. "," .. py
                if not occupied[key] then
                    ic.x = px
                    ic.y = py
                    occupied[key] = true
                    break
                end
                row = row + 1
                if row >= max_rows then
                    row = 0
                    col = col + 1
                end
            end
            row = row + 1
            if row >= max_rows then
                row = 0
                col = col + 1
            end
        end
    end
end

local function scan_desktop()
    desktop_icons = {}
    local layout = load_layout()

    -- Ensure /desktop exists
    aios.io.mkdir(DESKTOP_PATH)

    local ok, list = pcall(aios.io.listdir, DESKTOP_PATH)
    if ok and list then
        for _, e in ipairs(list) do
            if e.name:sub(1, 1) == "." then goto continue end  -- skip hidden

            local ic = {
                name = e.name,
                filename = e.name,
                x = 0, y = 0,
                tex = -1,
            }

            if e.name:match("%.desk$") then
                -- Shortcut file
                local fn = loadfile(DESKTOP_PATH .. "/" .. e.name)
                if fn then
                    local rok, info = pcall(fn)
                    if rok and type(info) == "table" then
                        ic.name = e.name:gsub("%.desk$", "")
                        ic.app = info.app
                        ic.icon_path = info.icon
                        ic.is_shortcut = true
                        if info.icon then
                            ic.tex = chaos_gl.load_texture(info.icon)
                        end
                    end
                end
            elseif e.is_dir then
                ic.is_dir = true
                -- Check if it's a packaged app
                local manifest_path = DESKTOP_PATH .. "/" .. e.name .. "/manifest.lua"
                local mfn = loadfile(manifest_path)
                if mfn then
                    local mok, manifest = pcall(mfn)
                    if mok and type(manifest) == "table" then
                        ic.is_app = true
                        ic.app = DESKTOP_PATH .. "/" .. e.name .. "/" .. (manifest.entry or "main.lua")
                        ic.name = manifest.name or e.name
                        if manifest.icon then
                            ic.tex = chaos_gl.load_texture(manifest.icon)
                        end
                    end
                end
            end

            -- Apply saved layout position
            local saved = layout[ic.filename]
            if saved then
                ic.x = saved.x
                ic.y = saved.y
            end

            desktop_icons[#desktop_icons + 1] = ic
            ::continue::
        end
    end

    -- Always add Trash as special icon
    local trash_pos = layout["__trash__"]
    desktop_icons[#desktop_icons + 1] = {
        name = "Trash",
        filename = "__trash__",
        special = "trash",
        tex = chaos_gl.load_texture("/system/icons/trash_48.png"),
        x = trash_pos and trash_pos.x or 0,
        y = trash_pos and trash_pos.y or 0,
    }

    -- Auto-position any icons without saved positions
    auto_position_icons()
end

scan_desktop()

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
        if mx >= ic.x and mx < ic.x + ICON_W and
           my >= ic.y and my < ic.y + ICON_H then
            return i
        end
    end
    return 0
end

-- ── Open desktop item ────────────────────────────────

local function open_icon(ic)
    if ic.special == "trash" then
        aios.task.spawn("/apps/files/main.lua", "Trash", {path = "/system/trash"})
        return
    end
    if ic.app then
        aios.task.spawn(ic.app, ic.name)
        return
    end
    if ic.is_dir then
        aios.task.spawn("/apps/files/main.lua", "Files", {path = DESKTOP_PATH .. "/" .. ic.filename})
        return
    end
    -- Regular file: use filetypes
    local fpath = DESKTOP_PATH .. "/" .. ic.filename
    local handler = filetypes.get_app(ic.filename)
    if handler then
        local app_name = handler:match("([^/]+)%.lua$") or "App"
        aios.task.spawn(handler, app_name, {file = fpath})
    end
end

-- ── Drag-and-drop state ──────────────────────────────

local dragging_icon = 0
local drag_offset_x, drag_offset_y = 0, 0
local drag_start_x, drag_start_y = 0, 0
local drag_pending = false
local drag_pending_mx, drag_pending_my = 0, 0
local drag_pending_idx = 0

local function snap_to_grid(x, y)
    local gx = math.floor((x - GRID_ORIGIN_X + GRID_X / 2) / GRID_X) * GRID_X + GRID_ORIGIN_X
    local gy = math.floor((y - GRID_ORIGIN_Y + GRID_Y / 2) / GRID_Y) * GRID_Y + GRID_ORIGIN_Y
    -- Clamp to screen
    gx = math.max(GRID_ORIGIN_X, math.min(gx, SCREEN_W - ICON_W - 8))
    gy = math.max(GRID_ORIGIN_Y, math.min(gy, SCREEN_H - TASKBAR_H - ICON_H - 8))
    return gx, gy
end

-- ── Context menu ─────────────────────────────────────

local context_menu = nil
local context_icon_idx = 0

local function dismiss_context_menu()
    if context_menu then
        context_menu:dismiss()
        context_menu = nil
    end
end

local function show_icon_context_menu(idx, mx, my)
    dismiss_context_menu()
    local ic = desktop_icons[idx]
    context_icon_idx = idx
    local items = {}

    if ic.special == "trash" then
        items[#items + 1] = { label = "Open Trash", on_click = function() open_icon(ic) end }
        items[#items + 1] = { separator = true }
        items[#items + 1] = { label = "Empty Trash", on_click = function()
            local trash = require("lib/trash")
            trash.empty()
        end }
    else
        items[#items + 1] = { label = "Open", on_click = function() open_icon(ic) end }
        items[#items + 1] = { separator = true }

        if not ic.is_shortcut or ic.filename then
            items[#items + 1] = { label = "Copy", on_click = function()
                local clipboard = require("lib/clipboard")
                local path = ic.special and "" or (DESKTOP_PATH .. "/" .. ic.filename)
                if path ~= "" then clipboard.copy({path}) end
            end }
            items[#items + 1] = { label = "Cut", on_click = function()
                local clipboard = require("lib/clipboard")
                local path = DESKTOP_PATH .. "/" .. ic.filename
                clipboard.cut({path})
            end }
            items[#items + 1] = { separator = true }
        end

        items[#items + 1] = { label = "Move to Trash", on_click = function()
            local trash = require("lib/trash")
            if ic.is_shortcut then
                aios.io.unlink(DESKTOP_PATH .. "/" .. ic.filename)
            else
                trash.move(DESKTOP_PATH .. "/" .. ic.filename)
            end
            scan_desktop()
            dirty = true
        end }

        items[#items + 1] = { separator = true }
        items[#items + 1] = { label = "Get Info", on_click = function()
            -- TODO: info dialog
        end }
    end

    context_menu = Menu.new(items)
    -- Clamp menu position to screen
    local mw, mh = context_menu:_calc_size()
    local cx = math.min(mx, SCREEN_W - mw - 4)
    local cy = math.min(my, SCREEN_H - mh - TASKBAR_H - 4)
    context_menu:show(cx, cy)
end

local function show_background_context_menu(mx, my)
    dismiss_context_menu()
    context_icon_idx = 0
    local items = {
        { label = "New Folder", on_click = function()
            local name = "New Folder"
            local n = 1
            while aios.io.exists(DESKTOP_PATH .. "/" .. name) do
                n = n + 1
                name = "New Folder " .. n
            end
            aios.io.mkdir(DESKTOP_PATH .. "/" .. name)
            scan_desktop()
            dirty = true
        end },
        { separator = true },
        { label = "Paste", shortcut = "Ctrl+V", on_click = function()
            local clipboard = require("lib/clipboard")
            clipboard.paste(DESKTOP_PATH)
            scan_desktop()
            dirty = true
        end },
        { separator = true },
        { label = "Refresh", on_click = function()
            scan_desktop()
            dirty = true
        end },
    }
    context_menu = Menu.new(items)
    local mw, mh = context_menu:_calc_size()
    local cx = math.min(mx, SCREEN_W - mw - 4)
    local cy = math.min(my, SCREEN_H - mh - TASKBAR_H - 4)
    context_menu:show(cx, cy)
end

-- ── Draw desktop ──────────────────────────────────

local dirty = true

local function draw_icon(ic, idx, accent, text_c, drag_highlight)
    -- Selection highlight
    if idx == selected_icon and dragging_icon == 0 then
        chaos_gl.rect_rounded(ic.x - 4, ic.y - 4, 56, 80, 4, 0x40FFFFFF)
    end

    -- Drag-over-trash highlight
    if drag_highlight then
        chaos_gl.rect_rounded(ic.x - 4, ic.y - 4, 56, 80, 4, 0x60FF4444)
    end

    if ic.tex and ic.tex >= 0 then
        local tw, th = chaos_gl.texture_get_size(ic.tex)
        local ox = ic.x + (ICON_W - tw) // 2
        local oy = ic.y + (ICON_W - th) // 2
        if chaos_gl.texture_has_alpha(ic.tex) then
            chaos_gl.blit_alpha(ox, oy, tw, th, ic.tex)
        else
            chaos_gl.blit_keyed(ox, oy, tw, th, ic.tex, 0x00FF00FF)
        end
    elseif ic.is_dir then
        chaos_gl.rect_rounded(ic.x, ic.y, ICON_W, ICON_W, 4, 0x0000A8CC)
        local dw = chaos_gl.text_width("DIR")
        chaos_gl.text(ic.x + (ICON_W - dw) // 2, ic.y + 16, "DIR", 0x00FFFFFF, 0, 0)
    else
        chaos_gl.rect_rounded(ic.x, ic.y, ICON_W, ICON_W, 4, 0x00666680)
        local ext = (ic.filename or ""):match("%.(%w+)$") or ""
        local ew = chaos_gl.text_width(ext)
        chaos_gl.text(ic.x + (ICON_W - ew) // 2, ic.y + 16, ext, 0x00FFFFFF, 0, 0)
    end

    -- Label
    local label = ic.name
    if #label > 12 then label = label:sub(1, 11) .. ".." end
    local lbl_w = chaos_gl.text_width(label)
    local lbl_x = ic.x + (ICON_W - lbl_w) // 2
    if idx == selected_icon and dragging_icon == 0 then
        chaos_gl.rect_rounded(lbl_x - 3, ic.y + 50, lbl_w + 6, 18, 3, accent)
        chaos_gl.text(lbl_x, ic.y + 52, label, 0x00FFFFFF, 0, 0)
    else
        chaos_gl.text(lbl_x, ic.y + 52, label, text_c, 0, 0)
    end
end

local function draw_desktop()
    chaos_gl.surface_bind(desktop_surface)
    local bg = theme and theme.desktop_bg or 0x00283040
    chaos_gl.surface_clear(desktop_surface, bg)
    local accent = theme and theme.accent or 0x00FF8800
    local text_c = theme and theme.text_primary or 0x00FFFFFF

    -- Check if dragging over trash
    local trash_highlight = false
    if dragging_icon > 0 then
        for i, ic in ipairs(desktop_icons) do
            if ic.special == "trash" and i ~= dragging_icon then
                local dic = desktop_icons[dragging_icon]
                local cx = dic.x + ICON_W // 2
                local cy = dic.y + ICON_H // 2
                if cx >= ic.x and cx < ic.x + ICON_W and cy >= ic.y and cy < ic.y + ICON_H then
                    trash_highlight = true
                end
            end
        end
    end

    for i, ic in ipairs(desktop_icons) do
        if i ~= dragging_icon then
            local hl = (ic.special == "trash" and trash_highlight)
            draw_icon(ic, i, accent, text_c, hl)
        end
    end

    -- Draw dragging icon on top
    if dragging_icon > 0 then
        draw_icon(desktop_icons[dragging_icon], dragging_icon, accent, text_c, false)
    end

    -- Draw context menu
    if context_menu and context_menu.visible then
        context_menu:draw(0, 0)
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
            -- Mouse event --

            -- ALWAYS update cursor position for every mouse event
            wm._update_cursor(event.mouse_x, event.mouse_y)

            -- Desktop drag capture: during drag, intercept mouse move/up
            if dragging_icon > 0 and (event.type == EVENT_MOUSE_MOVE or event.type == EVENT_MOUSE_UP) then
                if event.type == EVENT_MOUSE_MOVE then
                    local ic = desktop_icons[dragging_icon]
                    ic.x = event.mouse_x - drag_offset_x
                    ic.y = event.mouse_y - drag_offset_y
                    dirty = true
                elseif event.type == EVENT_MOUSE_UP then
                    local ic = desktop_icons[dragging_icon]
                    -- Check trash drop
                    local dropped_on_trash = false
                    for ti, tic in ipairs(desktop_icons) do
                        if tic.special == "trash" and ti ~= dragging_icon then
                            local cx = ic.x + ICON_W // 2
                            local cy = ic.y + ICON_H // 2
                            if cx >= tic.x and cx < tic.x + ICON_W and cy >= tic.y and cy < tic.y + ICON_H then
                                dropped_on_trash = true
                                break
                            end
                        end
                    end

                    if dropped_on_trash and not ic.special then
                        -- Move to trash
                        local trash = require("lib/trash")
                        if ic.is_shortcut then
                            aios.io.unlink(DESKTOP_PATH .. "/" .. ic.filename)
                        else
                            trash.move(DESKTOP_PATH .. "/" .. ic.filename)
                        end
                        scan_desktop()
                    else
                        -- Snap to grid
                        ic.x, ic.y = snap_to_grid(ic.x, ic.y)
                        save_layout()
                    end
                    dragging_icon = 0
                    drag_pending = false
                    dirty = true
                end
                goto next_event
            end

            -- Context menu intercept
            if context_menu and context_menu.visible then
                if context_menu:on_input(event) then
                    dirty = true
                    if not context_menu.visible then
                        context_menu = nil
                    end
                    goto next_event
                end
                -- Click outside menu dismisses it
                if event.type == EVENT_MOUSE_DOWN then
                    dismiss_context_menu()
                    dirty = true
                    goto next_event
                end
            end

            local target = wm.route_mouse(event)
            if target == "taskbar" then
                dismiss_context_menu()
                if event.type == EVENT_MOUSE_DOWN then
                    wm._handle_taskbar_click(event)
                    taskbar_dirty = true
                end
            elseif target == "app_menu" then
                dismiss_context_menu()
                if event.type == EVENT_MOUSE_DOWN then
                    wm._handle_app_menu_click(event)
                    taskbar_dirty = true
                elseif event.type == EVENT_MOUSE_MOVE then
                    wm._handle_app_menu_hover(event)
                end
            elseif target then
                dismiss_context_menu()
                local translated = wm.translate_event(event, target)
                aios.wm.push_event(target, translated)
            else
                -- Desktop background event --

                if event.type == EVENT_MOUSE_DOWN and event.button == 1 then
                    dismiss_context_menu()
                    local hit_idx = hit_test_icon(event.mouse_x, event.mouse_y)
                    if hit_idx > 0 then
                        if selected_icon == hit_idx and is_double_click(event.mouse_x, event.mouse_y) then
                            open_icon(desktop_icons[hit_idx])
                            taskbar_dirty = true
                        else
                            selected_icon = hit_idx
                            is_double_click(event.mouse_x, event.mouse_y)
                            -- Start drag pending
                            drag_pending = true
                            drag_pending_idx = hit_idx
                            drag_pending_mx = event.mouse_x
                            drag_pending_my = event.mouse_y
                        end
                    else
                        selected_icon = 0
                        wm._handle_desktop_click(event)
                    end
                    dirty = true

                elseif event.type == EVENT_MOUSE_DOWN and event.button == 2 then
                    -- Right-click: context menu
                    local hit_idx = hit_test_icon(event.mouse_x, event.mouse_y)
                    if hit_idx > 0 then
                        selected_icon = hit_idx
                        show_icon_context_menu(hit_idx, event.mouse_x, event.mouse_y)
                    else
                        selected_icon = 0
                        show_background_context_menu(event.mouse_x, event.mouse_y)
                    end
                    dirty = true

                elseif event.type == EVENT_MOUSE_MOVE then
                    -- Check for drag threshold
                    if drag_pending and drag_pending_idx > 0 then
                        local dx = math.abs(event.mouse_x - drag_pending_mx)
                        local dy = math.abs(event.mouse_y - drag_pending_my)
                        if dx > DRAG_THRESHOLD or dy > DRAG_THRESHOLD then
                            dragging_icon = drag_pending_idx
                            local ic = desktop_icons[dragging_icon]
                            drag_offset_x = drag_pending_mx - ic.x
                            drag_offset_y = drag_pending_my - ic.y
                            drag_start_x = ic.x
                            drag_start_y = ic.y
                            drag_pending = false
                            dirty = true
                        end
                    end

                elseif event.type == EVENT_MOUSE_UP then
                    if drag_pending then
                        drag_pending = false
                        drag_pending_idx = 0
                    end
                end
            end
        end
        ::next_event::
        event = aios.input.poll()
    end

    collectgarbage("step", 1)

    frame_count = frame_count + 1
    if now - last_heartbeat >= 30000 then
        print(string.format("[desktop] heartbeat: %d frames, %d events, uptime=%ds",
              frame_count, event_count, now // 1000))
        last_heartbeat = now
    end

    -- Pick up theme changes from Settings app
    local ui = require("core")
    if ui.poll_theme() then
        dirty = true
        taskbar_dirty = true
    end

    aios.os.sleep(16)
end
