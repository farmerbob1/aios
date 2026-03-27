-- AIOS v2 — Terminal / Lua REPL
local AppWindow = require("appwindow")
local win = AppWindow.new("Terminal", 520, 380, {x=120, y=80})

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
        add_output("Packages: pkg refresh|install|remove|update|search|list|info")
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
    elseif cmd:match("^pkg%s*") then
        local sub = cmd:match("^pkg%s+(%S+)") or ""
        local arg = cmd:match("^pkg%s+%S+%s+(.+)")
        local pok, pcpm = pcall(require, "cpm")
        if not pok then
            add_error("pkg: cpm library not found")
        elseif sub == "refresh" then
            add_output("Refreshing package index...")
            local result = pcpm.refresh(function(stage, detail)
                add_output("  " .. stage .. ": " .. (detail.name or detail.source or ""))
            end)
            add_output("  " .. #(result.packages or {}) .. " package(s) found")
            for _, e in ipairs(result.errors or {}) do
                add_error("  " .. e.source .. ": " .. e.error)
            end
        elseif sub == "install" and arg then
            add_output("Installing " .. arg .. "...")
            local ok2, err2 = pcpm.install(arg, function(stage, detail)
                add_output("  " .. stage .. ": " .. (detail.name or ""))
            end)
            if ok2 then add_output("  Installed!", 0x0044FF44)
            else add_error("  " .. tostring(err2)) end
        elseif sub == "remove" and arg then
            local ok2, err2 = pcpm.uninstall(arg)
            if ok2 then add_output("Removed " .. arg, 0x0044FF44)
            else add_error(tostring(err2)) end
        elseif sub == "update" then
            if arg then
                local ok2, err2 = pcpm.update(arg)
                if ok2 then add_output("Updated " .. arg, 0x0044FF44)
                else add_error(tostring(err2)) end
            else
                add_output("Updating all packages...")
                local r = pcpm.update_all()
                for _, n in ipairs(r.updated) do add_output("  Updated: " .. n, 0x0044FF44) end
                for _, f in ipairs(r.failed) do add_error("  Failed: " .. f.name .. " - " .. f.error) end
            end
        elseif sub == "search" and arg then
            local results = pcpm.search(arg)
            if #results == 0 then add_output("No packages matching '" .. arg .. "'")
            else
                for _, p in ipairs(results) do
                    add_output("  " .. p.name .. " v" .. p.version .. " - " .. (p.description or ""))
                end
            end
        elseif sub == "list" then
            local inst = pcpm.installed()
            local count = 0
            for name, info in pairs(inst) do
                add_output("  " .. name .. " v" .. info.version .. " (" .. (info.source or "?") .. ")")
                count = count + 1
            end
            if count == 0 then add_output("No packages installed via CPM") end
        elseif sub == "info" and arg then
            local info = pcpm.info(arg)
            if not info then add_error("Package not found: " .. arg)
            else
                add_output("  Name: " .. (info.name or arg))
                add_output("  Version: " .. (info.version or "?"))
                add_output("  Author: " .. (info.author or "?"))
                add_output("  Description: " .. (info.description or ""))
                if info.installed then
                    add_output("  Status: Installed (v" .. info.installed_version .. ")", 0x0044FF44)
                else
                    add_output("  Status: Not installed")
                end
            end
        else
            add_output("Usage: pkg refresh|install|remove|update|search|list|info [name]")
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

while win:is_running() do
    blink_timer = blink_timer + 1
    if blink_timer >= 30 then  -- toggle every ~500ms (30 frames at 60fps)
        cursor_visible = not cursor_visible
        blink_timer = 0
    end

    win.title = "Terminal [" .. cwd .. "]"
    win:begin_frame()

    local bg = theme and theme.field_bg or 0x001A1A2E
    local prompt_c = theme and theme.accent or 0x0044FF44

    -- Output area
    local output_y = 30
    local output_h = 380 - 28 - 24
    local line_h = chaos_gl.font_height(-1)
    local max_visible = output_h // line_h

    -- Draw terminal background over content area
    chaos_gl.rect(0, output_y, 520, output_h, bg)

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
        local sb_track = theme and theme.slider_track or 0x00333344
        local sb_thumb = theme and theme.slider_thumb_color or 0x00666688
        chaos_gl.rect(sb_x, output_y, 6, sb_h, sb_track)
        chaos_gl.rect(sb_x, thumb_y, 6, thumb_h, sb_thumb)
    end

    -- Input line
    local input_y = 380 - 24
    local input_bg = theme and theme.titlebar_bg or 0x00222244
    local cursor_c = theme and theme.field_cursor or 0x00FFFFFF
    chaos_gl.rect(0, input_y, 520, 24, input_bg)
    chaos_gl.text(4, input_y + 4, "> " .. input_line, prompt_c, 0, 0)

    -- Cursor blink
    if cursor_visible then
        local cursor_x = 4 + chaos_gl.text_width("> " .. input_line:sub(1, cursor_pos))
        chaos_gl.rect(cursor_x, input_y + 2, 2, 16, cursor_c)
    end

    win:end_frame()

    -- Events
    for _, event in ipairs(win:poll_events()) do
        if event.type == EVENT_KEY_DOWN then
            cursor_visible = true
            blink_timer = 0
            if event.key == 28 then -- Enter
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
        elseif event.type == EVENT_MOUSE_WHEEL then
            scroll_y = math.max(0, scroll_y + (event.wheel or 0) * -3)
        end
    end

    aios.os.sleep(16)
end

win:destroy()
