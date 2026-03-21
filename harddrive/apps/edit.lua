-- @app name="Editor" icon="/system/icons/text_32.raw"
-- AIOS v2 — Text Editor

local surface = chaos_gl.surface_create(560, 420, false)
chaos_gl.surface_set_position(surface, 100, 50)
chaos_gl.surface_set_visible(surface, true)
aios.wm.register(surface, {
    title = "Editor",
    task_id = aios.task.self().id,
})

-- Get file path from args if available
local file_path = nil
local file_content = ""
local modified = false
local text_lines = {""}
local cursor_line = 1
local cursor_col = 0
local scroll_y = 0
local scroll_x = 0
local gutter_w = 40

-- Check for file argument
if arg and arg.file then
    file_path = arg.file
elseif _G._edit_file then
    file_path = _G._edit_file
end

local function split_lines(text)
    local result = {}
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        result[#result + 1] = line
    end
    if #result == 0 then result[1] = "" end
    return result
end

local function load_file(path)
    local ok, content = pcall(aios.io.read, path)
    if ok and content then
        file_content = content
        text_lines = split_lines(content)
        modified = false
        cursor_line = 1
        cursor_col = 0
        scroll_y = 0
    end
end

local function save_file()
    if not file_path then return end
    local content = table.concat(text_lines, "\n")
    local ok, err = pcall(aios.io.write, file_path, content)
    if ok then
        modified = false
    end
end

if file_path then
    load_file(file_path)
end

local running = true
while running do
    chaos_gl.surface_bind(surface)
    local bg = 0x001E1E2E
    local text_c = 0x00DDDDDD
    local line_num_c = 0x00666688
    local cursor_c = 0x00FFFFFF
    local titlebar_bg = theme and theme.titlebar_bg or 0x003C3C3C
    local title_c = theme and theme.titlebar_text or 0x00FFFFFF
    chaos_gl.surface_clear(surface, bg)

    -- Title bar
    chaos_gl.rect(0, 0, 560, 28, titlebar_bg)
    local title = "Editor"
    if file_path then title = title .. " - " .. file_path end
    if modified then title = title .. " *" end
    chaos_gl.text(8, 6, title, title_c, 0, 0)
    chaos_gl.rect(560 - 28, 0, 28, 28, 0x00FF4444)
    chaos_gl.text(560 - 20, 6, "X", 0x00FFFFFF, 0, 0)

    -- Editor area
    local edit_y = 28
    local edit_h = 420 - 28 - 20
    local line_h = 16
    local max_visible = edit_h // line_h

    -- Gutter + content
    chaos_gl.push_clip(0, edit_y, 560, edit_h)
    chaos_gl.rect(0, edit_y, gutter_w, edit_h, 0x00252540)

    for i = scroll_y + 1, math.min(#text_lines, scroll_y + max_visible + 1) do
        local y = edit_y + (i - scroll_y - 1) * line_h

        -- Line number
        local num_str = tostring(i)
        local nw = chaos_gl.text_width(num_str)
        chaos_gl.text(gutter_w - nw - 4, y, num_str, line_num_c, 0, 0)

        -- Current line highlight
        if i == cursor_line then
            chaos_gl.rect(gutter_w, y, 560 - gutter_w, line_h, 0x00282848)
        end

        -- Text content
        local line = text_lines[i] or ""
        chaos_gl.text(gutter_w + 4 - scroll_x, y, line, text_c, 0, 0)
    end

    -- Draw cursor
    local cursor_screen_y = edit_y + (cursor_line - scroll_y - 1) * line_h
    if cursor_screen_y >= edit_y and cursor_screen_y < edit_y + edit_h then
        local line = text_lines[cursor_line] or ""
        local before = line:sub(1, cursor_col)
        local cx = gutter_w + 4 + chaos_gl.text_width(before) - scroll_x
        chaos_gl.rect(cx, cursor_screen_y, 2, line_h, cursor_c)
    end

    chaos_gl.pop_clip()

    -- Status bar
    local status_y = 420 - 20
    chaos_gl.rect(0, status_y, 560, 20, 0x00333355)
    local status = string.format("Ln %d, Col %d", cursor_line, cursor_col + 1)
    if modified then status = status .. "  [Modified]" end
    if file_path then status = status .. "  " .. file_path end
    chaos_gl.text(8, status_y + 2, status, 0x00AAAAAA, 0, 0)

    -- Ctrl+S hint
    chaos_gl.text(560 - 80, status_y + 2, "Ctrl+S Save", 0x00666688, 0, 0)

    chaos_gl.surface_present(surface)

    -- Events
    local event = aios.wm.poll_event(surface)
    while event do
        if event.type == EVENT_KEY_DOWN then
            local key = event.key

            if event.ctrl and key == 31 then -- Ctrl+S
                save_file()
            elseif key == 1 then -- Escape
                running = false
            elseif key == 28 then -- Enter
                local line = text_lines[cursor_line] or ""
                local before = line:sub(1, cursor_col)
                local after = line:sub(cursor_col + 1)
                text_lines[cursor_line] = before
                table.insert(text_lines, cursor_line + 1, after)
                cursor_line = cursor_line + 1
                cursor_col = 0
                modified = true
            elseif key == 14 then -- Backspace
                if cursor_col > 0 then
                    local line = text_lines[cursor_line] or ""
                    text_lines[cursor_line] = line:sub(1, cursor_col - 1) ..
                                              line:sub(cursor_col + 1)
                    cursor_col = cursor_col - 1
                    modified = true
                elseif cursor_line > 1 then
                    local prev = text_lines[cursor_line - 1] or ""
                    cursor_col = #prev
                    text_lines[cursor_line - 1] = prev .. (text_lines[cursor_line] or "")
                    table.remove(text_lines, cursor_line)
                    cursor_line = cursor_line - 1
                    modified = true
                end
            elseif key == 211 then -- Delete
                local line = text_lines[cursor_line] or ""
                if cursor_col < #line then
                    text_lines[cursor_line] = line:sub(1, cursor_col) ..
                                              line:sub(cursor_col + 2)
                    modified = true
                elseif cursor_line < #text_lines then
                    text_lines[cursor_line] = line .. (text_lines[cursor_line + 1] or "")
                    table.remove(text_lines, cursor_line + 1)
                    modified = true
                end
            elseif key == 200 then -- Up
                if cursor_line > 1 then
                    cursor_line = cursor_line - 1
                    cursor_col = math.min(cursor_col, #(text_lines[cursor_line] or ""))
                end
            elseif key == 208 then -- Down
                if cursor_line < #text_lines then
                    cursor_line = cursor_line + 1
                    cursor_col = math.min(cursor_col, #(text_lines[cursor_line] or ""))
                end
            elseif key == 203 then -- Left
                if cursor_col > 0 then
                    cursor_col = cursor_col - 1
                elseif cursor_line > 1 then
                    cursor_line = cursor_line - 1
                    cursor_col = #(text_lines[cursor_line] or "")
                end
            elseif key == 205 then -- Right
                local line = text_lines[cursor_line] or ""
                if cursor_col < #line then
                    cursor_col = cursor_col + 1
                elseif cursor_line < #text_lines then
                    cursor_line = cursor_line + 1
                    cursor_col = 0
                end
            elseif key == 199 then -- Home
                cursor_col = 0
            elseif key == 207 then -- End
                cursor_col = #(text_lines[cursor_line] or "")
            elseif key == 201 then -- Page Up
                cursor_line = math.max(1, cursor_line - max_visible)
                cursor_col = math.min(cursor_col, #(text_lines[cursor_line] or ""))
            elseif key == 209 then -- Page Down
                cursor_line = math.min(#text_lines, cursor_line + max_visible)
                cursor_col = math.min(cursor_col, #(text_lines[cursor_line] or ""))
            end

            -- Ensure cursor visible (scroll)
            if cursor_line <= scroll_y then
                scroll_y = cursor_line - 1
            elseif cursor_line > scroll_y + max_visible then
                scroll_y = cursor_line - max_visible
            end

        elseif event.type == EVENT_KEY_CHAR then
            local ch = event.char
            if ch and ch ~= "" and ch:byte() >= 32 then
                local line = text_lines[cursor_line] or ""
                text_lines[cursor_line] = line:sub(1, cursor_col) .. ch ..
                                          line:sub(cursor_col + 1)
                cursor_col = cursor_col + 1
                modified = true
            end

        elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            if event.mouse_x >= 560 - 28 and event.mouse_y < 28 then
                running = false
            end

        elseif event.type == EVENT_MOUSE_WHEEL then
            scroll_y = math.max(0, scroll_y + (event.wheel or 0) * -3)
            scroll_y = math.min(scroll_y, math.max(0, #text_lines - max_visible))
        end

        event = aios.wm.poll_event(surface)
    end

    aios.os.sleep(16)
end

aios.wm.unregister(surface)
chaos_gl.surface_destroy(surface)
