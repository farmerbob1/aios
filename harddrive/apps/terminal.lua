-- @app name="Terminal" icon="/system/icons/shell_48.raw"
-- AIOS v2 — Terminal / Lua REPL

local surface = chaos_gl.surface_create(520, 380, false)
chaos_gl.surface_set_position(surface, 120, 80)
chaos_gl.surface_set_visible(surface, true)
aios.wm.register(surface, {
    title = "Terminal",
    task_id = aios.task.self().id,
})

local lines = {}
local input_line = ""
local cursor_pos = 0
local scroll_y = 0
local history = {}
local history_idx = 0
local max_lines = 500
local cwd = "/"

local function add_line(text, color)
    lines[#lines + 1] = {text = text, color = color or 0x00CCCCCC}
    if #lines > max_lines then
        table.remove(lines, 1)
    end
end

local function add_output(text)
    add_line(text, 0x00CCCCCC)
end

local function add_error(text)
    add_line(text, 0x00FF4444)
end

local function add_prompt(text)
    add_line("> " .. text, 0x0044FF44)
end

add_line("AIOS Terminal v1.0", 0x00FF8800)
add_line("Type 'help' for commands", 0x00888888)
add_line("", 0)

local function execute(cmd)
    cmd = cmd:match("^%s*(.-)%s*$") -- trim
    if cmd == "" then return end

    add_prompt(cmd)
    history[#history + 1] = cmd
    history_idx = #history + 1

    -- Built-in commands
    if cmd == "help" then
        add_output("Commands: ls, cat, cd, mkdir, rm, mem, clear, help")
        add_output("Network:  ifconfig, ping, get, dns")
        add_output("Or type any Lua expression")
    elseif cmd == "clear" then
        lines = {}
    elseif cmd == "mem" then
        local info = aios.os.meminfo()
        if info then
            add_output("Total: " .. tostring(math.floor((info.total or 0) / 1024)) .. " KB")
            add_output("Free:  " .. tostring(math.floor((info.free or 0) / 1024)) .. " KB")
            add_output("Used:  " .. tostring(math.floor((info.used or 0) / 1024)) .. " KB")
        end
    elseif cmd:match("^ls") then
        local path = cmd:match("^ls%s+(.+)") or cwd
        local ok, entries = pcall(aios.io.listdir, path)
        if ok and entries then
            for _, e in ipairs(entries) do
                local suffix = e.is_dir and "/" or ""
                add_output("  " .. e.name .. suffix)
            end
        else
            add_error("ls: cannot list " .. path)
        end
    elseif cmd:match("^cd%s+") then
        local path = cmd:match("^cd%s+(.+)")
        if path then
            -- Resolve relative paths
            if path:sub(1, 1) ~= "/" then
                if cwd == "/" then
                    path = "/" .. path
                else
                    path = cwd .. "/" .. path
                end
            end
            local ok, entries = pcall(aios.io.listdir, path)
            if ok then
                cwd = path
            else
                add_error("cd: no such directory: " .. path)
            end
        end
    elseif cmd:match("^cat%s+") then
        local path = cmd:match("^cat%s+(.+)")
        if path then
            if path:sub(1, 1) ~= "/" then
                if cwd == "/" then path = "/" .. path
                else path = cwd .. "/" .. path end
            end
            local ok, content = pcall(aios.io.read, path)
            if ok and content then
                for line in content:gmatch("[^\n]*") do
                    add_output(line)
                end
            else
                add_error("cat: cannot read " .. path)
            end
        end
    elseif cmd:match("^mkdir%s+") then
        local name = cmd:match("^mkdir%s+(.+)")
        if name then
            local path = name
            if path:sub(1, 1) ~= "/" then
                if cwd == "/" then path = "/" .. path
                else path = cwd .. "/" .. path end
            end
            local ok, err = pcall(aios.io.mkdir, path)
            if not ok then add_error("mkdir: " .. tostring(err)) end
        end
    elseif cmd:match("^rm%s+") then
        local name = cmd:match("^rm%s+(.+)")
        if name then
            local path = name
            if path:sub(1, 1) ~= "/" then
                if cwd == "/" then path = "/" .. path
                else path = cwd .. "/" .. path end
            end
            local ok, err = pcall(aios.io.remove, path)
            if not ok then add_error("rm: " .. tostring(err)) end
        end
    elseif cmd == "ifconfig" then
        local info = aios.net.ifconfig()
        if info then
            add_output("  IP:   " .. (info.ip or "none"))
            add_output("  Mask: " .. (info.mask or "none"))
            add_output("  GW:   " .. (info.gw or "none"))
            add_output("  MAC:  " .. (info.mac or "none"))
        else
            add_error("ifconfig: network not available")
        end
    elseif cmd:match("^ping%s+") then
        local host = cmd:match("^ping%s+(.+)")
        if host then
            add_output("Resolving " .. host .. "...")
            local ip, err = aios.net.dns_resolve(host, 5000)
            if ip then
                add_output("  " .. host .. " resolved to " .. ip)
                -- TCP connect test (no ICMP raw socket, so we test reachability)
                add_output("  Connecting to " .. ip .. ":80...")
                local sock, cerr = aios.net.tcp_connect(ip, 80, 3000)
                if sock then
                    aios.net.tcp_close(sock)
                    add_output("  " .. host .. " is reachable (TCP:80 open)", 0x0044FF44)
                else
                    add_output("  " .. host .. " TCP:80 " .. (cerr or "unreachable"), 0x00FFAA00)
                end
            else
                add_error("ping: " .. (err or "resolve failed"))
            end
        end
    elseif cmd:match("^dns%s+") then
        local host = cmd:match("^dns%s+(.+)")
        if host then
            local ip, err = aios.net.dns_resolve(host, 5000)
            if ip then
                add_output("  " .. host .. " = " .. ip)
            else
                add_error("dns: " .. (err or "failed"))
            end
        end
    elseif cmd:match("^get%s+") then
        local url = cmd:match("^get%s+(.+)")
        if url then
            -- Add http:// if no scheme
            if not url:match("^https?://") then
                url = "http://" .. url
            end
            add_output("Fetching " .. url .. "...")
            local ok, http = pcall(require, "http")
            if not ok then
                add_error("get: http library not found")
            else
                local body, status, headers = http.get(url, 10000)
                if body then
                    add_output("  Status: " .. tostring(status), 0x0044FF44)
                    -- Show first 20 lines of body
                    local line_count = 0
                    for line in body:gmatch("[^\n]*") do
                        if line_count >= 20 then
                            add_output("  ... (truncated)")
                            break
                        end
                        add_output("  " .. line)
                        line_count = line_count + 1
                    end
                    add_output("  (" .. tostring(#body) .. " bytes total)")
                else
                    add_error("get: " .. tostring(headers or status or "failed"))
                end
            end
        end
    else
        -- Lua eval
        local fn, err = load("return " .. cmd)
        if not fn then
            fn, err = load(cmd)
        end
        if fn then
            local ok, result = pcall(fn)
            if ok then
                if result ~= nil then
                    add_output(tostring(result))
                end
            else
                add_error(tostring(result))
            end
        else
            add_error(tostring(err))
        end
    end
end

local blink_timer = 0
local cursor_visible = true

local running = true
while running do
    blink_timer = blink_timer + 1
    if blink_timer >= 30 then  -- toggle every ~500ms (30 frames at 60fps)
        cursor_visible = not cursor_visible
        blink_timer = 0
    end
    chaos_gl.surface_bind(surface)
    local bg = 0x001A1A2E
    local text_c = 0x00CCCCCC
    local prompt_c = 0x0044FF44
    local titlebar_bg = theme and theme.titlebar_bg or 0x003C3C3C
    local title_c = theme and theme.titlebar_text or 0x00FFFFFF
    chaos_gl.surface_clear(surface, bg)

    -- Title bar
    chaos_gl.rect(0, 0, 520, 28, titlebar_bg)
    chaos_gl.text(8, 6, "Terminal [" .. cwd .. "]", title_c, 0, 0)
    chaos_gl.rect(520 - 28, 0, 28, 28, 0x00FF4444)
    chaos_gl.text(520 - 20, 6, "X", 0x00FFFFFF, 0, 0)

    -- Output area
    local output_y = 30
    local output_h = 380 - 28 - 24
    local line_h = 16
    local max_visible = output_h // line_h

    chaos_gl.push_clip(0, output_y, 520, output_h)
    local start_line = math.max(1, #lines - max_visible - scroll_y + 1)
    local y = output_y
    for i = start_line, #lines do
        if y >= output_y + output_h then break end
        chaos_gl.text(4, y, lines[i].text, lines[i].color, 0, 0)
        y = y + line_h
    end
    chaos_gl.pop_clip()

    -- Scrollbar
    local total_lines = #lines
    if total_lines > max_visible then
        local sb_x = 520 - 8
        local sb_h = output_h
        local thumb_h = math.max(20, math.floor(sb_h * max_visible / total_lines))
        local max_scroll = total_lines - max_visible
        local scroll_frac = max_scroll > 0 and (1 - scroll_y / max_scroll) or 1
        local thumb_y = output_y + math.floor((sb_h - thumb_h) * scroll_frac)
        -- Track
        chaos_gl.rect(sb_x, output_y, 6, sb_h, 0x00333344)
        -- Thumb
        chaos_gl.rect(sb_x, thumb_y, 6, thumb_h, 0x00666688)
    end

    -- Input line
    local input_y = 380 - 24
    chaos_gl.rect(0, input_y, 520, 24, 0x00222244)
    chaos_gl.text(4, input_y + 4, "> " .. input_line, prompt_c, 0, 0)

    -- Cursor blink
    if cursor_visible then
        local cursor_x = 4 + chaos_gl.text_width("> " .. input_line:sub(1, cursor_pos))
        chaos_gl.rect(cursor_x, input_y + 2, 2, 16, 0x00FFFFFF)
    end

    chaos_gl.surface_present(surface)

    -- Events
    local event = aios.wm.poll_event(surface)
    while event do
        if event.type == EVENT_KEY_DOWN then
            cursor_visible = true
            blink_timer = 0
            if event.key == 1 then -- Escape
                running = false
            elseif event.key == 28 then -- Enter
                execute(input_line)
                input_line = ""
                cursor_pos = 0
                scroll_y = 0
            elseif event.key == 14 then -- Backspace
                if cursor_pos > 0 then
                    input_line = input_line:sub(1, cursor_pos - 1) ..
                                 input_line:sub(cursor_pos + 1)
                    cursor_pos = cursor_pos - 1
                end
            elseif event.key == 211 then -- Delete
                if cursor_pos < #input_line then
                    input_line = input_line:sub(1, cursor_pos) ..
                                 input_line:sub(cursor_pos + 2)
                end
            elseif event.key == 203 then -- Left
                if cursor_pos > 0 then cursor_pos = cursor_pos - 1 end
            elseif event.key == 205 then -- Right
                if cursor_pos < #input_line then cursor_pos = cursor_pos + 1 end
            elseif event.key == 199 then -- Home
                cursor_pos = 0
            elseif event.key == 207 then -- End
                cursor_pos = #input_line
            elseif event.key == 200 then -- Up (history)
                if history_idx > 1 then
                    history_idx = history_idx - 1
                    input_line = history[history_idx] or ""
                    cursor_pos = #input_line
                end
            elseif event.key == 208 then -- Down (history)
                if history_idx < #history then
                    history_idx = history_idx + 1
                    input_line = history[history_idx] or ""
                    cursor_pos = #input_line
                elseif history_idx == #history then
                    history_idx = history_idx + 1
                    input_line = ""
                    cursor_pos = 0
                end
            end
        elseif event.type == EVENT_KEY_CHAR then
            local ch = event.char
            if ch and ch ~= "" then
                input_line = input_line:sub(1, cursor_pos) .. ch ..
                             input_line:sub(cursor_pos + 1)
                cursor_pos = cursor_pos + 1
            end
        elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            if event.mouse_x >= 520 - 28 and event.mouse_y < 28 then
                running = false
            end
        elseif event.type == EVENT_MOUSE_WHEEL then
            scroll_y = math.max(0, scroll_y + (event.wheel or 0) * -3)
        end
        event = aios.wm.poll_event(surface)
    end

    aios.os.sleep(16)
end

aios.wm.unregister(surface)
chaos_gl.surface_destroy(surface)
