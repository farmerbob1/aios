-- AIOS v2 — Text Editor (Full Rewrite)
local AppWindow = require("appwindow")
local Dialog = require("dialog")
local FileDialog = require("filedialog")

local W, H = 600, 440
local win = AppWindow.new("Editor", W, H, {x=80, y=40})

-- ═══ State ═══
local lines = {""}
local cursor_line = 1
local cursor_col = 0
local scroll_y = 0
local scroll_x = 0
local file_path = nil
local modified = false
local TAB_SIZE = 4
local pending_close = false
local active_dialog = nil
local file_dialog = nil
local gutter_w = 44
local TITLEBAR_H = AppWindow.TITLEBAR_H
local STATUS_H = 20
local line_h = chaos_gl.font_height(-1)

-- Selection (anchor = where selection started)
local sel_line = nil
local sel_col = nil

-- Undo/Redo stacks
local undo_stack = {}
local redo_stack = {}
local MAX_UNDO = 200

-- Find bar
local find_active = false
local find_text = ""
local replace_text = ""
local find_matches = {}
local find_idx = 0
local find_replace_mode = false

-- ═══ Helpers ═══

local function split_lines(text)
    local result = {}
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        result[#result + 1] = line
    end
    if #result == 0 then result[1] = "" end
    return result
end

local function get_text()
    return table.concat(lines, "\n")
end

-- ═══ Undo System ═══

local function push_undo()
    undo_stack[#undo_stack + 1] = {
        lines = {table.unpack(lines)},
        cl = cursor_line, cc = cursor_col,
        sl = sel_line, sc = sel_col,
    }
    if #undo_stack > MAX_UNDO then
        table.remove(undo_stack, 1)
    end
    redo_stack = {}
end

local function do_undo()
    if #undo_stack == 0 then return end
    redo_stack[#redo_stack + 1] = {
        lines = {table.unpack(lines)},
        cl = cursor_line, cc = cursor_col,
    }
    local state = table.remove(undo_stack)
    lines = state.lines
    cursor_line = state.cl
    cursor_col = state.cc
    modified = true
end

local function do_redo()
    if #redo_stack == 0 then return end
    undo_stack[#undo_stack + 1] = {
        lines = {table.unpack(lines)},
        cl = cursor_line, cc = cursor_col,
    }
    local state = table.remove(redo_stack)
    lines = state.lines
    cursor_line = state.cl
    cursor_col = state.cc
    modified = true
end

-- ═══ Selection ═══

local function has_selection()
    return sel_line ~= nil and (sel_line ~= cursor_line or sel_col ~= cursor_col)
end

local function sel_ordered()
    if not has_selection() then return nil end
    local sl, sc, el, ec = sel_line, sel_col, cursor_line, cursor_col
    if sl > el or (sl == el and sc > ec) then
        sl, sc, el, ec = el, ec, sl, sc
    end
    return sl, sc, el, ec
end

local function get_selected_text()
    local sl, sc, el, ec = sel_ordered()
    if not sl then return "" end
    if sl == el then
        return lines[sl]:sub(sc + 1, ec)
    end
    local parts = {lines[sl]:sub(sc + 1)}
    for i = sl + 1, el - 1 do
        parts[#parts + 1] = lines[i]
    end
    parts[#parts + 1] = lines[el]:sub(1, ec)
    return table.concat(parts, "\n")
end

local function delete_selection()
    local sl, sc, el, ec = sel_ordered()
    if not sl then return false end
    push_undo()
    if sl == el then
        lines[sl] = lines[sl]:sub(1, sc) .. lines[sl]:sub(ec + 1)
    else
        local new_line = lines[sl]:sub(1, sc) .. lines[el]:sub(ec + 1)
        for _ = sl + 1, el do
            table.remove(lines, sl + 1)
        end
        lines[sl] = new_line
    end
    cursor_line = sl
    cursor_col = sc
    sel_line = nil
    sel_col = nil
    modified = true
    return true
end

local function clear_selection()
    sel_line = nil
    sel_col = nil
end

local function start_selection()
    if not sel_line then
        sel_line = cursor_line
        sel_col = cursor_col
    end
end

-- ═══ File I/O ═══

local function load_file(path)
    local fd = aios.io.open(path, "r")
    if fd then
        local content = aios.io.read(fd, "a")
        aios.io.close(fd)
        if content then
            lines = split_lines(content)
            modified = false
            cursor_line = 1
            cursor_col = 0
            scroll_y = 0
            undo_stack = {}
            redo_stack = {}
        end
    end
end

local function do_save()
    if not file_path then return false end
    local fd = aios.io.open(file_path, "w")
    if fd then
        aios.io.write(fd, get_text())
        aios.io.close(fd)
        modified = false
        return true
    end
    return false
end

local function show_save_as(then_callback)
    local default_dir = "/"
    local default_name = "untitled.lua"
    if file_path then
        default_dir = file_path:match("^(.+)/[^/]+$") or "/"
        default_name = file_path:match("([^/]+)$") or default_name
    end
    file_dialog = FileDialog.new({
        title = "Save As",
        mode = "save",
        default_path = default_dir,
        default_name = default_name,
        owner_surface = win.surface,
        on_confirm = function(path)
            file_path = path
            do_save()
            if then_callback then then_callback() end
        end,
    })
    file_dialog:show()
end

local function save_file()
    if file_path then
        do_save()
    else
        show_save_as()
    end
end

-- ═══ Find ═══

local function update_find_matches()
    find_matches = {}
    if #find_text == 0 then return end
    local lower_find = find_text:lower()
    for i, line in ipairs(lines) do
        local start = 1
        while true do
            local s, e = line:lower():find(lower_find, start, true)
            if not s then break end
            find_matches[#find_matches + 1] = {line = i, col = s - 1, len = e - s + 1}
            start = e + 1
        end
    end
    find_idx = #find_matches > 0 and 1 or 0
end

local function find_next()
    if #find_matches == 0 then return end
    find_idx = find_idx % #find_matches + 1
    local m = find_matches[find_idx]
    cursor_line = m.line
    cursor_col = m.col
    sel_line = m.line
    sel_col = m.col + m.len
end

local function find_replace_one()
    if find_idx == 0 or #find_matches == 0 then return end
    local m = find_matches[find_idx]
    push_undo()
    local line = lines[m.line]
    lines[m.line] = line:sub(1, m.col) .. replace_text .. line:sub(m.col + m.len + 1)
    modified = true
    update_find_matches()
    if #find_matches > 0 then find_next() end
end

-- ═══ Load initial file ═══

if arg and arg.file then
    file_path = arg.file
elseif _G._edit_file then
    file_path = _G._edit_file
end
if file_path then load_file(file_path) end

-- ═══ Scroll helper ═══

local function ensure_cursor_visible()
    local sw, sh = win:get_size()
    local edit_h = sh - TITLEBAR_H - STATUS_H - (find_active and 24 or 0)
    local max_vis = edit_h // line_h
    if cursor_line <= scroll_y then
        scroll_y = cursor_line - 1
    elseif cursor_line > scroll_y + max_vis then
        scroll_y = cursor_line - max_vis
    end
    -- Horizontal
    local before = (lines[cursor_line] or ""):sub(1, cursor_col)
    local cx = chaos_gl.text_width(before)
    local edit_w = sw - gutter_w - 8
    if cx - scroll_x > edit_w then
        scroll_x = cx - edit_w + 20
    elseif cx - scroll_x < 0 then
        scroll_x = math.max(0, cx - 20)
    end
end

-- ═══ Close confirmation ═══

local function try_close()
    if not modified then
        win:close()
        return
    end
    active_dialog = Dialog.new({
        title = "Unsaved Changes",
        message = "You have unsaved changes. Save before closing?",
        buttons = {
            { label = "Don't Save", on_click = function() win:close() end },
            { label = "Cancel" },
            { label = "Save & Close", style = "primary", on_click = function()
                if file_path then
                    do_save()
                    win:close()
                else
                    show_save_as(function() win:close() end)
                end
            end },
        }
    })
    active_dialog:show(1024, 768, win.surface)
end

-- Override AppWindow's internal close so it goes through our dialog
local _real_poll = win.poll_events
function win:poll_events()
    local events = _real_poll(self)
    -- If AppWindow decided to close, check if we need confirmation
    if not self._running and modified and not pending_close then
        self._running = true  -- Cancel the close
        try_close()
        return {}
    end
    return events
end

-- ═══ Main Loop ═══

while win:is_running() or active_dialog do
    local sw, sh = win:get_size()

    -- Dynamic title
    local title = "Editor"
    if file_path then title = title .. " - " .. file_path end
    if modified then title = title .. " *" end
    win.title = title

    win:begin_frame()

    -- Theme colors
    local bg = theme and theme.field_bg or 0x001E1E2E
    local text_c = theme and theme.text_primary or 0x00DDDDDD
    local line_num_c = theme and theme.text_disabled or 0x00666688
    local cursor_c = theme and theme.field_cursor or 0x00FFFFFF
    local gutter_bg = theme and theme.tab_bg or 0x00252540
    local hl_bg = theme and theme.list_selection or 0x00282848
    local status_bg = theme and theme.titlebar_bg or 0x00333355
    local status_c = theme and theme.text_secondary or 0x00AAAAAA
    local hint_c = theme and theme.text_disabled or 0x00666688
    local accent = theme and theme.accent or 0x00FF8800
    local sel_bg = theme and theme.list_selection or 0x00334466
    local find_hl = theme and theme.accent or 0x00FFAA00

    -- Layout
    local find_bar_h = find_active and 24 or 0
    local edit_y = TITLEBAR_H
    local edit_h = sh - TITLEBAR_H - STATUS_H - find_bar_h
    local max_visible = edit_h // line_h

    -- Gutter
    chaos_gl.push_clip(0, edit_y, sw, edit_h)
    chaos_gl.rect(0, edit_y, gutter_w, edit_h, gutter_bg)

    -- Selection range
    local sl, sc, el, ec = sel_ordered()

    for i = scroll_y + 1, math.min(#lines, scroll_y + max_visible + 1) do
        local y = edit_y + (i - scroll_y - 1) * line_h
        local line = lines[i] or ""

        -- Current line highlight
        if i == cursor_line and not has_selection() then
            chaos_gl.rect(gutter_w, y, sw - gutter_w, line_h, hl_bg)
        end

        -- Selection highlight
        if sl and i >= sl and i <= el then
            local s0 = (i == sl) and chaos_gl.text_width(line:sub(1, sc)) or 0
            local s1 = (i == el) and chaos_gl.text_width(line:sub(1, ec)) or chaos_gl.text_width(line)
            chaos_gl.rect(gutter_w + 4 + s0 - scroll_x, y, s1 - s0, line_h, sel_bg)
        end

        -- Find match highlights
        for _, m in ipairs(find_matches) do
            if m.line == i then
                local mx = chaos_gl.text_width(line:sub(1, m.col))
                local mw = chaos_gl.text_width(line:sub(m.col + 1, m.col + m.len))
                chaos_gl.rect(gutter_w + 4 + mx - scroll_x, y, mw, line_h, find_hl)
            end
        end

        -- Line number
        local num_str = tostring(i)
        local nw = chaos_gl.text_width(num_str)
        chaos_gl.text(gutter_w - nw - 4, y, num_str, line_num_c, 0, 0)

        -- Text
        chaos_gl.text(gutter_w + 4 - scroll_x, y, line, text_c, 0, 0)
    end

    -- Cursor
    local cy = edit_y + (cursor_line - scroll_y - 1) * line_h
    if cy >= edit_y and cy < edit_y + edit_h then
        local before = (lines[cursor_line] or ""):sub(1, cursor_col)
        local cx = gutter_w + 4 + chaos_gl.text_width(before) - scroll_x
        -- Blink
        if (aios.os.millis() // 500) % 2 == 0 then
            chaos_gl.rect(cx, cy, 2, line_h, cursor_c)
        end
    end
    chaos_gl.pop_clip()

    -- Find bar
    if find_active then
        local fy = sh - STATUS_H - find_bar_h
        local bar_bg = theme and theme.tab_bg or 0x00333344
        local field_bg = theme and theme.field_bg or 0x001E1E2E
        chaos_gl.rect(0, fy, sw, find_bar_h, bar_bg)
        chaos_gl.text(4, fy + 6, "Find:", hint_c, 0, 0)
        chaos_gl.rect(40, fy + 2, 160, 20, field_bg)
        chaos_gl.text(44, fy + 6, find_text, text_c, 0, 0)
        local info = #find_matches > 0
            and string.format("%d/%d", find_idx, #find_matches) or "No matches"
        chaos_gl.text(208, fy + 6, info, hint_c, 0, 0)
        if find_replace_mode then
            chaos_gl.text(290, fy + 6, "Replace:", hint_c, 0, 0)
            chaos_gl.rect(350, fy + 2, 140, 20, field_bg)
            chaos_gl.text(354, fy + 6, replace_text, text_c, 0, 0)
        end
    end

    -- Status bar
    local status_y = sh - STATUS_H
    chaos_gl.rect(0, status_y, sw, STATUS_H, status_bg)
    local status = string.format("Ln %d, Col %d  %d lines", cursor_line, cursor_col + 1, #lines)
    if modified then status = status .. "  [Modified]" end
    chaos_gl.text(8, status_y + 3, status, status_c, 0, 0)
    local hints = "Ctrl+S Save  Ctrl+F Find  Ctrl+Z Undo"
    chaos_gl.text(sw - chaos_gl.text_width(hints) - 8, status_y + 3, hints, hint_c, 0, 0)

    -- Dialog overlays
    if active_dialog and active_dialog.visible then
        active_dialog:draw()
    end
    if file_dialog and file_dialog.visible then
        file_dialog:draw()
    end

    win:end_frame()

    -- ═══ Events ═══
    for _, event in ipairs(win:poll_events()) do
        -- File dialog takes priority
        if file_dialog and file_dialog.visible then
            file_dialog:on_input(event)
            if not file_dialog.visible then
                file_dialog = nil
            end
            goto event_continue
        end
        -- Then confirmation dialog
        if active_dialog and active_dialog.visible then
            active_dialog:on_input(event)
            if not active_dialog.visible then
                active_dialog = nil
            end
            goto event_continue
        end
        if event.type == EVENT_KEY_DOWN then
            local key = event.key
            local shift = event.shift
            local ctrl = event.ctrl

            -- ── Ctrl shortcuts ──
            if ctrl then
                if key == 17 then -- Ctrl+W (close)
                    try_close()
                    goto event_continue
                elseif key == 31 and shift then -- Ctrl+Shift+S (Save As)
                    show_save_as()
                elseif key == 31 then -- Ctrl+S
                    save_file()
                elseif key == 44 then -- Ctrl+Z
                    do_undo()
                elseif key == 21 then -- Ctrl+Y
                    do_redo()
                elseif key == 30 then -- Ctrl+A (select all)
                    sel_line = 1
                    sel_col = 0
                    cursor_line = #lines
                    cursor_col = #(lines[#lines] or "")
                elseif key == 32 then -- Ctrl+D (duplicate line)
                    push_undo()
                    table.insert(lines, cursor_line + 1, lines[cursor_line])
                    cursor_line = cursor_line + 1
                    modified = true
                elseif key == 33 then -- Ctrl+F
                    find_active = true
                    find_replace_mode = false
                    find_text = ""
                    find_matches = {}
                elseif key == 35 then -- Ctrl+H
                    find_active = true
                    find_replace_mode = true
                    find_text = ""
                    replace_text = ""
                    find_matches = {}
                elseif key == 199 then -- Ctrl+Home
                    if shift then start_selection() end
                    cursor_line = 1
                    cursor_col = 0
                    if not shift then clear_selection() end
                elseif key == 207 then -- Ctrl+End
                    if shift then start_selection() end
                    cursor_line = #lines
                    cursor_col = #(lines[#lines] or "")
                    if not shift then clear_selection() end
                end
                goto continue
            end

            -- ── Find bar input ──
            if find_active then
                if key == 1 then -- Esc
                    find_active = false
                    find_matches = {}
                    goto continue
                elseif key == 28 then -- Enter
                    if find_replace_mode and shift then
                        find_replace_one()
                    else
                        find_next()
                    end
                    goto continue
                elseif key == 14 then -- Backspace in find
                    if find_replace_mode and #replace_text > 0 then
                        -- If user was typing in replace field... simplified: backspace on find
                    end
                    if #find_text > 0 then
                        find_text = find_text:sub(1, -2)
                        update_find_matches()
                    end
                    goto continue
                elseif key == 15 then -- Tab toggles find/replace field
                    goto continue
                end
            end

            -- ── Navigation with optional selection ──
            if shift then start_selection() end

            if key == 28 then -- Enter
                if has_selection() then delete_selection() end
                push_undo()
                local line = lines[cursor_line] or ""
                local before = line:sub(1, cursor_col)
                local after = line:sub(cursor_col + 1)
                lines[cursor_line] = before
                table.insert(lines, cursor_line + 1, after)
                cursor_line = cursor_line + 1
                cursor_col = 0
                modified = true
                clear_selection()
            elseif key == 14 then -- Backspace
                if has_selection() then
                    delete_selection()
                elseif cursor_col > 0 then
                    push_undo()
                    local line = lines[cursor_line] or ""
                    lines[cursor_line] = line:sub(1, cursor_col - 1) .. line:sub(cursor_col + 1)
                    cursor_col = cursor_col - 1
                    modified = true
                elseif cursor_line > 1 then
                    push_undo()
                    local prev = lines[cursor_line - 1] or ""
                    cursor_col = #prev
                    lines[cursor_line - 1] = prev .. (lines[cursor_line] or "")
                    table.remove(lines, cursor_line)
                    cursor_line = cursor_line - 1
                    modified = true
                end
            elseif key == 211 then -- Delete
                if has_selection() then
                    delete_selection()
                else
                    push_undo()
                    local line = lines[cursor_line] or ""
                    if cursor_col < #line then
                        lines[cursor_line] = line:sub(1, cursor_col) .. line:sub(cursor_col + 2)
                        modified = true
                    elseif cursor_line < #lines then
                        lines[cursor_line] = line .. (lines[cursor_line + 1] or "")
                        table.remove(lines, cursor_line + 1)
                        modified = true
                    end
                end
            elseif key == 15 then -- Tab
                if has_selection() then delete_selection() end
                push_undo()
                local spaces = string.rep(" ", TAB_SIZE)
                local line = lines[cursor_line] or ""
                lines[cursor_line] = line:sub(1, cursor_col) .. spaces .. line:sub(cursor_col + 1)
                cursor_col = cursor_col + TAB_SIZE
                modified = true
                clear_selection()
            elseif key == 200 then -- Up
                if cursor_line > 1 then
                    cursor_line = cursor_line - 1
                    cursor_col = math.min(cursor_col, #(lines[cursor_line] or ""))
                end
                if not shift then clear_selection() end
            elseif key == 208 then -- Down
                if cursor_line < #lines then
                    cursor_line = cursor_line + 1
                    cursor_col = math.min(cursor_col, #(lines[cursor_line] or ""))
                end
                if not shift then clear_selection() end
            elseif key == 203 then -- Left
                if cursor_col > 0 then
                    cursor_col = cursor_col - 1
                elseif cursor_line > 1 then
                    cursor_line = cursor_line - 1
                    cursor_col = #(lines[cursor_line] or "")
                end
                if not shift then clear_selection() end
            elseif key == 205 then -- Right
                local line = lines[cursor_line] or ""
                if cursor_col < #line then
                    cursor_col = cursor_col + 1
                elseif cursor_line < #lines then
                    cursor_line = cursor_line + 1
                    cursor_col = 0
                end
                if not shift then clear_selection() end
            elseif key == 199 then -- Home
                cursor_col = 0
                if not shift then clear_selection() end
            elseif key == 207 then -- End
                cursor_col = #(lines[cursor_line] or "")
                if not shift then clear_selection() end
            elseif key == 201 then -- Page Up
                cursor_line = math.max(1, cursor_line - max_visible)
                cursor_col = math.min(cursor_col, #(lines[cursor_line] or ""))
                if not shift then clear_selection() end
            elseif key == 209 then -- Page Down
                cursor_line = math.min(#lines, cursor_line + max_visible)
                cursor_col = math.min(cursor_col, #(lines[cursor_line] or ""))
                if not shift then clear_selection() end
            elseif key == 1 then -- Esc
                clear_selection()
                if find_active then
                    find_active = false
                    find_matches = {}
                end
            end

            ::continue::
            ensure_cursor_visible()

        elseif event.type == EVENT_KEY_CHAR then
            local ch = event.char
            if ch and ch ~= "" and ch:byte() >= 32 then
                if find_active then
                    -- Type into find bar
                    if find_replace_mode and event.shift then
                        replace_text = replace_text .. ch
                    else
                        find_text = find_text .. ch
                        update_find_matches()
                    end
                else
                    if has_selection() then delete_selection() end
                    push_undo()
                    local line = lines[cursor_line] or ""
                    lines[cursor_line] = line:sub(1, cursor_col) .. ch .. line:sub(cursor_col + 1)
                    cursor_col = cursor_col + 1
                    modified = true
                    clear_selection()
                end
            end

        elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            local mx, my = event.mouse_x, event.mouse_y
            if my >= TITLEBAR_H and my < sh - STATUS_H - find_bar_h then
                -- Click in editor area → position cursor
                local click_line = scroll_y + 1 + (my - TITLEBAR_H) // line_h
                click_line = math.max(1, math.min(#lines, click_line))
                local line = lines[click_line] or ""
                -- Find closest column
                local best_col = 0
                local best_dist = 999999
                for c = 0, #line do
                    local px = gutter_w + 4 + chaos_gl.text_width(line:sub(1, c)) - scroll_x
                    local dist = math.abs(px - mx)
                    if dist < best_dist then
                        best_dist = dist
                        best_col = c
                    end
                end
                if event.shift then
                    start_selection()
                else
                    clear_selection()
                end
                cursor_line = click_line
                cursor_col = best_col
            end

        elseif event.type == EVENT_MOUSE_WHEEL then
            scroll_y = math.max(0, scroll_y + (event.wheel or 0) * -3)
            scroll_y = math.min(scroll_y, math.max(0, #lines - max_visible))
        end

        ::event_continue::
    end

    aios.os.sleep(16)
end

win:destroy()
