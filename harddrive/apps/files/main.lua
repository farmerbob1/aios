-- AIOS v2 — File Browser Application

local filetypes = require("filetypes")
local AppWindow = require("appwindow")
local Button = require("button")
local Menu = require("menu")
local trash = require("lib/trash")
local clipboard = require("lib/clipboard")

local W, H = 560, 420
local win = AppWindow.new("Files", W, H, {x=80, y=60})

-- Accept startup arguments (e.g., from Trash icon or desktop folder)
local current_path = "/"
if arg and arg.path then
    current_path = arg.path
elseif arg and arg.file then
    -- Open containing directory
    local dir = arg.file:match("^(.+)/[^/]+$") or "/"
    current_path = dir
end

local entries = {}
local selected = 0
local scroll_y = 0
local view_mode = "grid"  -- "grid" or "list"
local history = {}

-- Sorting
local sort_col = "name"  -- "name", "size", "type"
local sort_asc = true

-- Double-click tracking
local last_click_time = 0
local last_click_idx = 0
local DOUBLE_CLICK_MS = 400

-- Drag-and-drop state
local dragging_entry = 0
local drag_pending = false
local drag_pending_idx = 0
local drag_pending_mx, drag_pending_my = 0, 0
local DRAG_THRESHOLD = 5
local drag_hover_idx = 0  -- folder being hovered during drag

-- Context menu
local context_menu = nil

-- Inline rename state
local renaming_idx = 0
local rename_text = ""
local rename_cursor = 0

-- Layout constants
local TITLEBAR_H = AppWindow.TITLEBAR_H
local TOOLBAR_H = 28
local PATHBAR_H = 20
local HEADER_H = 22
local CONTENT_Y = TITLEBAR_H + TOOLBAR_H + PATHBAR_H
local LIST_ITEM_H = 22
local GRID_CELL_W = 80
local GRID_CELL_H = 80

local function format_size(bytes)
    if not bytes then return "" end
    if bytes < 1024 then return tostring(bytes) .. " B" end
    if bytes < 1024 * 1024 then return string.format("%.1f KB", bytes / 1024) end
    return string.format("%.1f MB", bytes / (1024 * 1024))
end

local function get_type(entry)
    if entry.is_app then return "App" end
    return filetypes.get_type_label(entry.name, entry.is_dir)
end

local function do_sort()
    table.sort(entries, function(a, b)
        -- Directories always first
        if a.is_dir and not b.is_dir then return true end
        if not a.is_dir and b.is_dir then return false end

        local va, vb
        if sort_col == "name" then
            va, vb = a.name:lower(), b.name:lower()
        elseif sort_col == "size" then
            va, vb = a.size or 0, b.size or 0
        elseif sort_col == "type" then
            va, vb = get_type(a), get_type(b)
        end

        if sort_asc then return va < vb else return va > vb end
    end)
end

local function build_path(name)
    if current_path == "/" then return "/" .. name end
    return current_path .. "/" .. name
end

-- Cache of loaded app icon textures (keyed by icon path)
local app_tex_cache = {}

local function load_app_icon(icon_path)
    if not icon_path then return -1 end
    if app_tex_cache[icon_path] then return app_tex_cache[icon_path] end
    local tex = chaos_gl.load_texture(icon_path)
    app_tex_cache[icon_path] = tex
    return tex
end

local function refresh()
    entries = {}
    local ok, list = pcall(aios.io.listdir, current_path)
    if ok and list then
        for _, e in ipairs(list) do
            -- Check if directory is a packaged app
            if e.is_dir and e.name ~= "." and e.name ~= ".." then
                local dir_path = build_path(e.name)
                local manifest_path = dir_path .. "/manifest.lua"
                local mok, mfn = pcall(loadfile, manifest_path)
                if mok and mfn then
                    local rok, manifest = pcall(mfn)
                    if rok and type(manifest) == "table" and manifest.name then
                        e.is_app = true
                        e.app_name = manifest.name
                        e.app_icon_tex = load_app_icon(manifest.icon)
                    end
                end
            end
            entries[#entries + 1] = e
        end
    end
    do_sort()
    selected = 0
    scroll_y = 0
    renaming_idx = 0
end

local function navigate(path)
    history[#history + 1] = current_path
    current_path = path
    refresh()
end

local function go_back()
    if #history > 0 then
        current_path = table.remove(history)
        refresh()
    end
end

local function go_up()
    if current_path == "/" then return end
    local parent = current_path:match("^(.+)/[^/]+$") or "/"
    navigate(parent)
end


local function is_app_file(fpath)
    -- Check for "-- @app" header marker (legacy flat-file apps)
    local ok, content = pcall(aios.io.readfile, fpath)
    if ok and content and content:sub(1, 6) == "-- @ap" then
        return true
    end
    return false
end

local function is_app_dir(dir_path)
    -- Check if directory contains manifest.lua (packaged app)
    local ok, items = pcall(aios.io.listdir, dir_path)
    if ok and items then
        for _, e in ipairs(items) do
            if e.name == "manifest.lua" then return true end
        end
    end
    return false
end

local function open_entry(entry)
    if entry.is_dir then
        local dir_path = build_path(entry.name)
        -- Check if it's a packaged app directory
        if is_app_dir(dir_path) then
            local manifest_path = dir_path .. "/manifest.lua"
            local mok, mfn = pcall(loadfile, manifest_path)
            if mok and mfn then
                local rok, manifest = pcall(mfn)
                if rok and type(manifest) == "table" and manifest.entry then
                    local entry_path = dir_path .. "/" .. manifest.entry
                    aios.task.spawn(entry_path, manifest.name or entry.name)
                    return
                end
            end
        end
        navigate(dir_path)
        return
    end

    local fpath = build_path(entry.name)

    -- Check if it's a legacy Lua app first
    if entry.name:match("%.lua$") and is_app_file(fpath) then
        local app_name = entry.name:gsub("%.lua$", "")
        app_name = app_name:sub(1,1):upper() .. app_name:sub(2)
        aios.task.spawn(fpath, app_name)
        return
    end

    -- Use filetypes to find the right handler app
    local handler = filetypes.get_app(entry.name)
    if handler then
        local app_name = handler:match("([^/]+)%.lua$") or "App"
        app_name = app_name:sub(1,1):upper() .. app_name:sub(2)
        aios.task.spawn(handler, app_name, {file = fpath})
    end
end

local function get_surface_w()
    local sw, _ = chaos_gl.surface_get_size(win.surface)
    return sw
end

-- ── Trash-specific helpers ───────────────────────────
local is_trash_view = (current_path == "/system/trash")

local function update_trash_view()
    is_trash_view = (current_path == "/system/trash")
end

-- ── Context menu helpers ─────────────────────────────

local function dismiss_context()
    if context_menu then
        context_menu:dismiss()
        context_menu = nil
    end
end

local function show_item_context_menu(idx, screen_x, screen_y)
    dismiss_context()
    local entry = entries[idx]
    if not entry then return end
    selected = idx
    local fpath = build_path(entry.name)
    local items = {}

    items[#items + 1] = { label = "Open", on_click = function() open_entry(entry) end }
    items[#items + 1] = { separator = true }

    items[#items + 1] = { label = "Copy", shortcut = "Ctrl+C", on_click = function()
        clipboard.copy({fpath})
    end }
    items[#items + 1] = { label = "Cut", shortcut = "Ctrl+X", on_click = function()
        clipboard.cut({fpath})
    end }

    if clipboard.has_content() then
        items[#items + 1] = { label = "Paste", shortcut = "Ctrl+V", on_click = function()
            clipboard.paste(current_path)
            refresh()
        end }
    end

    items[#items + 1] = { separator = true }

    if is_trash_view then
        items[#items + 1] = { label = "Restore", on_click = function()
            trash.restore(entry.name)
            refresh()
        end }
        items[#items + 1] = { label = "Delete Permanently", on_click = function()
            aios.io.unlink(fpath)
            refresh()
        end }
    else
        items[#items + 1] = { label = "Delete", shortcut = "Del", on_click = function()
            trash.move(fpath)
            refresh()
        end }
    end

    items[#items + 1] = { label = "Rename", shortcut = "F2", on_click = function()
        renaming_idx = idx
        rename_text = entry.name
        rename_cursor = #rename_text
    end }

    items[#items + 1] = { separator = true }
    items[#items + 1] = { label = "Get Info", on_click = function()
        -- TODO: info dialog
    end }

    context_menu = Menu.new(items)
    local mw, mh = context_menu:_calc_size()
    local sw, sh = chaos_gl.surface_get_size(win.surface)
    local cx = math.min(screen_x, 1024 - mw - 4)
    local cy = math.min(screen_y, 768 - mh - 4)
    context_menu:show(cx, cy)
end

local function show_bg_context_menu(screen_x, screen_y)
    dismiss_context()
    selected = 0
    local items = {}

    items[#items + 1] = { label = "New Folder", on_click = function()
        local name = "New Folder"
        local n = 1
        while aios.io.exists(build_path(name)) do
            n = n + 1
            name = "New Folder " .. n
        end
        aios.io.mkdir(build_path(name))
        refresh()
    end }

    items[#items + 1] = { separator = true }

    if clipboard.has_content() then
        items[#items + 1] = { label = "Paste", shortcut = "Ctrl+V", on_click = function()
            clipboard.paste(current_path)
            refresh()
        end }
        items[#items + 1] = { separator = true }
    end

    items[#items + 1] = { label = "Refresh", on_click = function() refresh() end }
    items[#items + 1] = { separator = true }
    items[#items + 1] = { label = sort_col == "name" and (sort_asc and "Sort: Name ^" or "Sort: Name v") or "Sort by Name",
        on_click = function()
            if sort_col == "name" then sort_asc = not sort_asc
            else sort_col = "name"; sort_asc = true end
            do_sort()
        end }
    items[#items + 1] = { label = sort_col == "size" and (sort_asc and "Sort: Size ^" or "Sort: Size v") or "Sort by Size",
        on_click = function()
            if sort_col == "size" then sort_asc = not sort_asc
            else sort_col = "size"; sort_asc = false end
            do_sort()
        end }
    items[#items + 1] = { label = sort_col == "type" and (sort_asc and "Sort: Type ^" or "Sort: Type v") or "Sort by Type",
        on_click = function()
            if sort_col == "type" then sort_asc = not sort_asc
            else sort_col = "type"; sort_asc = true end
            do_sort()
        end }

    if is_trash_view then
        items[#items + 1] = { separator = true }
        items[#items + 1] = { label = "Empty Trash", on_click = function()
            trash.empty()
            refresh()
        end }
    end

    context_menu = Menu.new(items)
    local mw, mh = context_menu:_calc_size()
    local cx = math.min(screen_x, 1024 - mw - 4)
    local cy = math.min(screen_y, 768 - mh - 4)
    context_menu:show(cx, cy)
end

refresh()

-- Toolbar widgets
local btn_back = Button.new("Back", function() go_back() end, {h=20})
local btn_up   = Button.new("Up",   function() go_up() end, {h=20})
local btn_view = Button.new("List", function()
    view_mode = (view_mode == "grid") and "list" or "grid"
    btn_view.text = (view_mode == "grid") and "List" or "Grid"
end, {h=20})
local btn_empty_trash = Button.new("Empty Trash", function()
    trash.empty()
    refresh()
end, {h=20})
local file_buttons = {btn_back, btn_up}

-- -- Drawing ---------------------------------------------------

local function draw_toolbar(sw)
    local toolbar_bg = theme and theme.tab_bg or 0x00353535
    chaos_gl.rect(0, TITLEBAR_H, sw, TOOLBAR_H, toolbar_bg)
    local bx = 4
    for _, btn in ipairs(file_buttons) do
        btn:draw(bx, TITLEBAR_H + 4)
        local bw = btn:get_size()
        bx = bx + bw + 4
    end
    -- View toggle
    btn_view.text = (view_mode == "grid") and "List" or "Grid"
    local vw, _ = btn_view:get_size()

    -- Empty Trash button (only in trash view)
    if is_trash_view then
        local ew, _ = btn_empty_trash:get_size()
        btn_empty_trash:draw(sw - vw - ew - 12, TITLEBAR_H + 4)
    end

    btn_view:draw(sw - vw - 4, TITLEBAR_H + 4)
end

local function draw_pathbar(sw)
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local pathbar_bg = theme and theme.field_bg or 0x00222222
    chaos_gl.rect(0, TITLEBAR_H + TOOLBAR_H, sw, PATHBAR_H, pathbar_bg)
    chaos_gl.text(4, TITLEBAR_H + TOOLBAR_H + 3, current_path, sec_c, 0, 0)
end

local function draw_list_header(sw)
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800
    local hy = CONTENT_Y
    local header_bg = theme and theme.tab_bg or 0x00303030
    chaos_gl.rect(0, hy, sw, HEADER_H, header_bg)

    -- Column headers with sort indicators
    local cols = {
        {label = "Name", col = "name", x = 24, w = sw - 200},
        {label = "Size", col = "size", x = sw - 170, w = 80},
        {label = "Type", col = "type", x = sw - 80, w = 70},
    }
    for _, c in ipairs(cols) do
        local color = (sort_col == c.col) and accent or sec_c
        local arrow = ""
        if sort_col == c.col then
            arrow = sort_asc and " ^" or " v"
        end
        chaos_gl.text(c.x, hy + 4, c.label .. arrow, color, 0, 0)
    end
    local sep_c = theme and theme.border or theme.separator or 0x00444444
    chaos_gl.rect(0, hy + HEADER_H - 1, sw, 1, sep_c)
end

local function draw_list_view(sw, sh)
    draw_list_header(sw)
    local list_top = CONTENT_Y + HEADER_H
    local visible_h = sh - list_top
    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800
    local alt_row = theme and theme.list_hover or 0x00323232

    chaos_gl.push_clip(0, list_top, sw, visible_h)
    for i, entry in ipairs(entries) do
        local y = list_top + (i - 1) * LIST_ITEM_H - scroll_y
        if y + LIST_ITEM_H > list_top and y < sh then
            if i == selected then
                chaos_gl.rect(0, y, sw, LIST_ITEM_H, accent)
            elseif i == drag_hover_idx then
                chaos_gl.rect(0, y, sw, LIST_ITEM_H, 0x40FF8800)
            elseif i % 2 == 0 then
                chaos_gl.rect(0, y, sw, LIST_ITEM_H, alt_row)
            end

            -- Icon
            if entry.is_app and entry.app_icon_tex and entry.app_icon_tex >= 0 then
                local iw, ih = chaos_gl.texture_get_size(entry.app_icon_tex)
                local scale = math.min(16 / iw, 16 / ih)
                local dw = math.floor(iw * scale)
                local dh = math.floor(ih * scale)
                local ix = 4 + (16 - dw) // 2
                local iy = y + 2 + (16 - dh) // 2
                if chaos_gl.texture_has_alpha(entry.app_icon_tex) then
                    chaos_gl.blit_alpha(ix, iy, iw, ih, entry.app_icon_tex)
                else
                    chaos_gl.blit(ix, iy, iw, ih, entry.app_icon_tex)
                end
            elseif entry.is_dir then
                chaos_gl.rect_rounded(4, y + 2, 16, 16, 2, 0x0000A8CC)
                chaos_gl.text(7, y + 3, "D", 0x00FFFFFF, 0, 0)
            else
                chaos_gl.rect_rounded(4, y + 2, 16, 16, 2, 0x00888888)
            end

            -- Name (show app display name for app dirs)
            local display_name = entry.is_app and entry.app_name or entry.name
            if renaming_idx == i then
                -- Inline rename
                chaos_gl.rect(24, y + 1, sw - 200, LIST_ITEM_H - 2, 0x00222222)
                chaos_gl.rect_outline(24, y + 1, sw - 200, LIST_ITEM_H - 2, accent, 1)
                chaos_gl.text(26, y + 4, rename_text, text_c, 0, 0)
                -- Cursor
                local cw = chaos_gl.text_width(rename_text:sub(1, rename_cursor))
                chaos_gl.rect(26 + cw, y + 3, 1, LIST_ITEM_H - 4, accent)
            else
                chaos_gl.text(24, y + 4, display_name, text_c, 0, 0)
            end

            -- Size
            if not entry.is_dir then
                local sz = format_size(entry.size)
                local szw = chaos_gl.text_width(sz)
                chaos_gl.text(sw - 170 + (80 - szw), y + 4, sz, sec_c, 0, 0)
            end

            -- Type
            local tp = entry.is_app and "App" or get_type(entry)
            chaos_gl.text(sw - 80, y + 4, tp, sec_c, 0, 0)
        end
    end
    chaos_gl.pop_clip()
end

local function draw_grid_view(sw, sh)
    local grid_top = CONTENT_Y
    local visible_h = sh - grid_top
    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local accent = theme and theme.accent or 0x00FF8800
    local cols = math.max(1, sw // GRID_CELL_W)

    chaos_gl.push_clip(0, grid_top, sw, visible_h)
    for i, entry in ipairs(entries) do
        local col = (i - 1) % cols
        local row = (i - 1) // cols
        local cx = col * GRID_CELL_W + (GRID_CELL_W - 48) // 2
        local cy = grid_top + row * GRID_CELL_H - scroll_y + 8

        if cy + GRID_CELL_H > grid_top and cy < sh then
            -- Selection bg
            if i == selected then
                chaos_gl.rect_rounded(col * GRID_CELL_W + 4, cy - 4, GRID_CELL_W - 8, GRID_CELL_H - 8, 4, 0x40FFFFFF)
            elseif i == drag_hover_idx then
                chaos_gl.rect_rounded(col * GRID_CELL_W + 4, cy - 4, GRID_CELL_W - 8, GRID_CELL_H - 8, 4, 0x40FF8800)
            end

            -- Icon
            if entry.is_app and entry.app_icon_tex and entry.app_icon_tex >= 0 then
                local iw, ih = chaos_gl.texture_get_size(entry.app_icon_tex)
                local ix = cx + (48 - iw) // 2
                local iy = cy + (40 - ih) // 2
                if chaos_gl.texture_has_alpha(entry.app_icon_tex) then
                    chaos_gl.blit_alpha(ix, iy, iw, ih, entry.app_icon_tex)
                else
                    chaos_gl.blit(ix, iy, iw, ih, entry.app_icon_tex)
                end
            elseif entry.is_dir then
                chaos_gl.rect_rounded(cx, cy, 48, 40, 4, 0x0000A8CC)
                local dw = chaos_gl.text_width("DIR")
                chaos_gl.text(cx + (48 - dw) // 2, cy + 13, "DIR", 0x00FFFFFF, 0, 0)
            else
                chaos_gl.rect_rounded(cx, cy, 48, 40, 4, 0x00666680)
                local ext = entry.name:match("%.(%w+)$") or ""
                local ew = chaos_gl.text_width(ext)
                chaos_gl.text(cx + (48 - ew) // 2, cy + 13, ext, 0x00FFFFFF, 0, 0)
            end

            -- Label (show app display name for app dirs)
            local name = entry.is_app and entry.app_name or entry.name
            if renaming_idx == i then
                -- Inline rename in grid
                local rw = math.max(GRID_CELL_W - 4, chaos_gl.text_width(rename_text) + 8)
                local rx = col * GRID_CELL_W + (GRID_CELL_W - rw) // 2
                chaos_gl.rect(rx, cy + 42, rw, 16, 0x00222222)
                chaos_gl.rect_outline(rx, cy + 42, rw, 16, accent, 1)
                chaos_gl.text(rx + 2, cy + 44, rename_text, text_c, 0, 0)
                local curw = chaos_gl.text_width(rename_text:sub(1, rename_cursor))
                chaos_gl.rect(rx + 2 + curw, cy + 43, 1, 14, accent)
            else
                if #name > 10 then name = name:sub(1, 9) .. ".." end
                local nw = chaos_gl.text_width(name)
                local lbl_x = col * GRID_CELL_W + (GRID_CELL_W - nw) // 2
                if i == selected then
                    chaos_gl.rect_rounded(lbl_x - 2, cy + 44, nw + 4, 14, 2, accent)
                    chaos_gl.text(lbl_x, cy + 44, name, 0x00FFFFFF, 0, 0)
                else
                    chaos_gl.text(lbl_x, cy + 44, name, text_c, 0, 0)
                end
            end
        end
    end
    chaos_gl.pop_clip()

    -- Draw drag ghost
    if dragging_entry > 0 and entries[dragging_entry] then
        local ename = entries[dragging_entry].name
        if #ename > 15 then ename = ename:sub(1, 14) .. ".." end
        local gw = chaos_gl.text_width(ename) + 8
        chaos_gl.rect_rounded(drag_pending_mx + 12, drag_pending_my - 8, gw, 18, 3, 0xC0333333)
        chaos_gl.text(drag_pending_mx + 16, drag_pending_my - 6, ename, 0x00FFFFFF, 0, 0)
    end
end

-- Hit test: which entry index is at (mx, my)?
local function hit_test_entry(mx, my, sw)
    if my < CONTENT_Y then return 0 end
    if view_mode == "list" then
        local list_top = CONTENT_Y + HEADER_H
        if my < list_top then return 0 end
        local idx = math.floor((my - list_top + scroll_y) / LIST_ITEM_H) + 1
        if idx >= 1 and idx <= #entries then return idx end
    else
        local grid_top = CONTENT_Y
        local cols = math.max(1, sw // GRID_CELL_W)
        local col = mx // GRID_CELL_W
        local row = (my - grid_top + scroll_y) // GRID_CELL_H
        local idx = row * cols + col + 1
        if idx >= 1 and idx <= #entries then return idx end
    end
    return 0
end

-- -- Main loop -------------------------------------------------

while win:is_running() do
    local sw, sh = win:get_size()

    win.title = "Files - " .. current_path
    update_trash_view()
    win:begin_frame()

    draw_toolbar(sw)
    draw_pathbar(sw)

    if view_mode == "list" then
        draw_list_view(sw, sh)
    else
        draw_grid_view(sw, sh)
    end

    -- Draw context menu on top
    if context_menu and context_menu.visible then
        context_menu:draw(0, 0)
    end

    win:end_frame()

    -- Process events
    for _, event in ipairs(win:poll_events()) do
        -- Context menu intercept: Menu expects screen-space coords,
        -- but win:poll_events() gives window-local coords. Convert.
        if context_menu and context_menu.visible then
            local sx, sy = chaos_gl.surface_get_position(win.surface)
            local screen_event = {}
            for k, v in pairs(event) do screen_event[k] = v end
            if event.mouse_x then screen_event.mouse_x = event.mouse_x + sx end
            if event.mouse_y then screen_event.mouse_y = event.mouse_y + sy end
            if context_menu:on_input(screen_event) then
                if not context_menu.visible then context_menu = nil end
                goto next_event
            end
            if event.type == EVENT_MOUSE_DOWN then
                dismiss_context()
                goto next_event
            end
        end

        -- Forward to toolbar button widgets first
        local handled = false
        for _, btn in ipairs(file_buttons) do
            if btn:on_input(event) then handled = true; break end
        end
        if not handled and btn_view:on_input(event) then handled = true end
        if not handled and is_trash_view and btn_empty_trash:on_input(event) then handled = true end

        -- Inline rename input handling
        if renaming_idx > 0 then
            if event.type == EVENT_KEY_DOWN then
                if event.key == 28 then  -- Enter
                    local entry = entries[renaming_idx]
                    if entry and rename_text ~= "" and rename_text ~= entry.name then
                        aios.io.rename(build_path(entry.name), build_path(rename_text))
                        refresh()
                    end
                    renaming_idx = 0
                    goto next_event
                elseif event.key == 1 then  -- Escape
                    renaming_idx = 0
                    goto next_event
                elseif event.key == 14 then  -- Backspace
                    if rename_cursor > 0 then
                        rename_text = rename_text:sub(1, rename_cursor - 1) .. rename_text:sub(rename_cursor + 1)
                        rename_cursor = rename_cursor - 1
                    end
                    goto next_event
                elseif event.key == 203 then  -- Left arrow
                    if rename_cursor > 0 then rename_cursor = rename_cursor - 1 end
                    goto next_event
                elseif event.key == 205 then  -- Right arrow
                    if rename_cursor < #rename_text then rename_cursor = rename_cursor + 1 end
                    goto next_event
                end
            elseif event.type == EVENT_KEY_CHAR then
                if event.char and event.char ~= "" then
                    rename_text = rename_text:sub(1, rename_cursor) .. event.char .. rename_text:sub(rename_cursor + 1)
                    rename_cursor = rename_cursor + 1
                end
                goto next_event
            end
        end

        if not handled and event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            dismiss_context()
            local mx, my = event.mouse_x, event.mouse_y

            -- List header clicks (sort)
            if view_mode == "list" and my >= CONTENT_Y and my < CONTENT_Y + HEADER_H then
                if mx < sw - 170 then
                    if sort_col == "name" then sort_asc = not sort_asc
                    else sort_col = "name"; sort_asc = true end
                elseif mx < sw - 80 then
                    if sort_col == "size" then sort_asc = not sort_asc
                    else sort_col = "size"; sort_asc = false end
                else
                    if sort_col == "type" then sort_asc = not sort_asc
                    else sort_col = "type"; sort_asc = true end
                end
                do_sort()

            -- Content area clicks
            elseif my >= CONTENT_Y then
                local idx = hit_test_entry(mx, my, sw)

                if idx >= 1 and idx <= #entries then
                    -- Cancel rename if clicking elsewhere
                    if renaming_idx > 0 and renaming_idx ~= idx then
                        renaming_idx = 0
                    end

                    local now = aios.os.millis()
                    if idx == last_click_idx and (now - last_click_time) < DOUBLE_CLICK_MS then
                        -- Double click
                        open_entry(entries[idx])
                        last_click_idx = 0
                    else
                        selected = idx
                        last_click_idx = idx
                        last_click_time = now
                        -- Start drag pending
                        drag_pending = true
                        drag_pending_idx = idx
                        drag_pending_mx = mx
                        drag_pending_my = my
                    end
                else
                    selected = 0
                    renaming_idx = 0
                end
            end

        elseif not handled and event.type == EVENT_MOUSE_DOWN and event.button == 2 then
            -- Right-click: context menu
            local mx, my = event.mouse_x, event.mouse_y
            local sx, sy = chaos_gl.surface_get_position(win.surface)
            local screen_x, screen_y = mx + sx, my + sy

            local idx = hit_test_entry(mx, my, sw)
            if idx >= 1 and idx <= #entries then
                show_item_context_menu(idx, screen_x, screen_y)
            else
                show_bg_context_menu(screen_x, screen_y)
            end

        elseif event.type == EVENT_MOUSE_MOVE then
            -- Drag detection
            if drag_pending and drag_pending_idx > 0 then
                local dx = math.abs(event.mouse_x - drag_pending_mx)
                local dy = math.abs(event.mouse_y - drag_pending_my)
                if dx > DRAG_THRESHOLD or dy > DRAG_THRESHOLD then
                    dragging_entry = drag_pending_idx
                    drag_pending = false
                end
            end

            if dragging_entry > 0 then
                drag_pending_mx = event.mouse_x
                drag_pending_my = event.mouse_y
                -- Determine hover target (folder)
                local idx = hit_test_entry(event.mouse_x, event.mouse_y, sw)
                if idx > 0 and idx ~= dragging_entry and entries[idx] and entries[idx].is_dir then
                    drag_hover_idx = idx
                else
                    drag_hover_idx = 0
                end
            end

        elseif event.type == EVENT_MOUSE_UP then
            if dragging_entry > 0 then
                -- Drop
                if drag_hover_idx > 0 and entries[drag_hover_idx] and entries[drag_hover_idx].is_dir then
                    local src = build_path(entries[dragging_entry].name)
                    local dst = build_path(entries[drag_hover_idx].name) .. "/" .. entries[dragging_entry].name
                    aios.io.rename(src, dst)
                    refresh()
                end
                dragging_entry = 0
                drag_hover_idx = 0
                drag_pending = false
            elseif drag_pending then
                drag_pending = false
                drag_pending_idx = 0
            end

        elseif event.type == EVENT_KEY_DOWN then
            if renaming_idx == 0 then
                if event.key == 28 and selected > 0 then -- Enter
                    open_entry(entries[selected])
                elseif event.ctrl and (event.key == 46) then -- Ctrl+C
                    if selected > 0 then
                        clipboard.copy({build_path(entries[selected].name)})
                    end
                elseif event.ctrl and (event.key == 45) then -- Ctrl+X
                    if selected > 0 then
                        clipboard.cut({build_path(entries[selected].name)})
                    end
                elseif event.ctrl and (event.key == 47) then -- Ctrl+V
                    clipboard.paste(current_path)
                    refresh()
                elseif event.key == 211 then -- Delete key (83+128)
                    if selected > 0 then
                        local fpath = build_path(entries[selected].name)
                        if is_trash_view then
                            aios.io.unlink(fpath)
                        else
                            trash.move(fpath)
                        end
                        refresh()
                    end
                elseif event.key == 60 then -- F2 = rename
                    if selected > 0 then
                        renaming_idx = selected
                        rename_text = entries[selected].name
                        rename_cursor = #rename_text
                    end
                end
            end

        elseif event.type == EVENT_MOUSE_WHEEL then
            local delta = event.wheel or 0
            if delta > 0 then scroll_y = math.max(0, scroll_y - 40)
            else scroll_y = scroll_y + 40 end
        end

        ::next_event::
    end

    aios.os.sleep(32)
end

win:destroy()
