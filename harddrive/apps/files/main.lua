-- AIOS v2 — File Browser Application

local filetypes = require("filetypes")
local AppWindow = require("appwindow")
local Button = require("button")

local W, H = 560, 420
local win = AppWindow.new("Files", W, H, {x=80, y=60})

local current_path = "/"
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

refresh()

-- Toolbar widgets
local btn_back = Button.new("Back", function() go_back() end, {h=20})
local btn_up   = Button.new("Up",   function() go_up() end, {h=20})
local btn_view = Button.new("List", function()
    view_mode = (view_mode == "grid") and "list" or "grid"
    btn_view.text = (view_mode == "grid") and "List" or "Grid"
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
    -- View toggle on right
    btn_view.text = (view_mode == "grid") and "List" or "Grid"
    local vw, _ = btn_view:get_size()
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
            chaos_gl.text(24, y + 4, display_name, text_c, 0, 0)

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
    chaos_gl.pop_clip()
end

-- -- Main loop -------------------------------------------------

while win:is_running() do
    local sw, sh = win:get_size()

    win.title = "Files - " .. current_path
    win:begin_frame()

    draw_toolbar(sw)
    draw_pathbar(sw)

    if view_mode == "list" then
        draw_list_view(sw, sh)
    else
        draw_grid_view(sw, sh)
    end

    win:end_frame()

    -- Process events
    for _, event in ipairs(win:poll_events()) do
        -- Forward to toolbar button widgets first
        local handled = false
        for _, btn in ipairs(file_buttons) do
            if btn:on_input(event) then handled = true; break end
        end
        if not handled and btn_view:on_input(event) then handled = true end

        if not handled and event.type == EVENT_MOUSE_DOWN and event.button == 1 then
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
                local idx = 0
                if view_mode == "list" then
                    local list_top = CONTENT_Y + HEADER_H
                    idx = math.floor((my - list_top + scroll_y) / LIST_ITEM_H) + 1
                else
                    local grid_top = CONTENT_Y
                    local cols = math.max(1, sw // GRID_CELL_W)
                    local col = mx // GRID_CELL_W
                    local row = (my - grid_top + scroll_y) // GRID_CELL_H
                    idx = row * cols + col + 1
                end

                if idx >= 1 and idx <= #entries then
                    local now = aios.os.millis()
                    if idx == last_click_idx and (now - last_click_time) < DOUBLE_CLICK_MS then
                        -- Double click
                        open_entry(entries[idx])
                        last_click_idx = 0
                    else
                        selected = idx
                        last_click_idx = idx
                        last_click_time = now
                    end
                else
                    selected = 0
                end
            end

        elseif event.type == EVENT_KEY_DOWN then
            if event.key == 28 and selected > 0 then -- Enter
                open_entry(entries[selected])
            end
        end
    end

    aios.os.sleep(32)
end

win:destroy()
