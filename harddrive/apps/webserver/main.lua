-- AIOS v2 — Static File HTTP Web Server
local AppWindow = require("appwindow")
-- Serves files from /apps/webserver/www/

local win = AppWindow.new("Web Server", 400, 350, {x=200, y=120})
local TITLEBAR_H = AppWindow.TITLEBAR_H

local PORT = 9090
local WWW_ROOT = "/apps/webserver/www"
local server = nil
local log = {}
local max_log = 50
local request_count = 0
local started = false

local function add_log(text)
    log[#log + 1] = text
    if #log > max_log then
        table.remove(log, 1)
    end
end

-- Content type by file extension
local mime_types = {
    html = "text/html; charset=utf-8",
    htm  = "text/html; charset=utf-8",
    css  = "text/css; charset=utf-8",
    js   = "application/javascript; charset=utf-8",
    json = "application/json; charset=utf-8",
    txt  = "text/plain; charset=utf-8",
    xml  = "text/xml; charset=utf-8",
    svg  = "image/svg+xml",
    png  = "image/png",
    jpg  = "image/jpeg",
    gif  = "image/gif",
    ico  = "image/x-icon",
}

local function get_mime(path)
    local ext = path:match("%.(%w+)$")
    if ext then ext = ext:lower() end
    return mime_types[ext] or "application/octet-stream"
end

-- Read a file from ChaosFS
local function read_file(path)
    local ok, fd = pcall(aios.io.open, path, "r")
    if not ok or not fd then return nil end
    local ok2, data = pcall(aios.io.read, fd, 1024 * 64)
    aios.io.close(fd)
    if not ok2 then return nil end
    return data
end

-- Sanitize request path (prevent directory traversal)
local function sanitize_path(path)
    path = path:match("^([^?#]*)") or path
    if path:find("%.%.") then return nil end
    if path == "/" then path = "/index.html" end
    return path
end

-- Parse HTTP request line
local function parse_request(data)
    if not data then return nil, nil end
    local method, path = data:match("^(%u+)%s+(%S+)")
    return method, path
end

-- Build HTTP response
local function http_response(status, content_type, body)
    local status_texts = {
        [200] = "OK",
        [404] = "Not Found",
        [400] = "Bad Request",
    }
    local resp = "HTTP/1.0 " .. status .. " " .. (status_texts[status] or "OK") .. "\r\n"
    resp = resp .. "Content-Type: " .. content_type .. "\r\n"
    resp = resp .. "Content-Length: " .. #body .. "\r\n"
    resp = resp .. "Connection: close\r\n"
    resp = resp .. "Server: AIOS/1.0\r\n"
    resp = resp .. "\r\n"
    resp = resp .. body
    return resp
end

local function serve_404()
    local body = "<html><head><title>404</title>"
    body = body .. "<style>body{font-family:monospace;background:#1e1e1e;color:#ccc;padding:40px;text-align:center}"
    body = body .. "h1{color:#f44;font-size:3em}a{color:#8cf}</style></head><body>"
    body = body .. "<h1>404</h1><p>File not found.</p>"
    body = body .. "<p><a href='/'>Go Home</a></p></body></html>"
    return http_response(404, "text/html; charset=utf-8", body)
end

-- Handle one client connection
local function handle_client(client)
    local data = aios.net.tcp_recv(client, 2048, 2000)
    local method, raw_path = parse_request(data)

    if not method then
        aios.net.tcp_close(client)
        return
    end

    request_count = request_count + 1

    local path = sanitize_path(raw_path or "/")
    if not path then
        add_log("#" .. request_count .. " " .. method .. " " .. raw_path .. " 400")
        aios.net.tcp_send(client, http_response(400, "text/plain", "Bad Request"))
        aios.net.tcp_close(client)
        return
    end

    local file_path = WWW_ROOT .. path
    local body = read_file(file_path)

    if body then
        local mime = get_mime(path)
        add_log("#" .. request_count .. " " .. method .. " " .. path .. " 200")
        aios.net.tcp_send(client, http_response(200, mime, body))
    else
        add_log("#" .. request_count .. " " .. method .. " " .. path .. " 404")
        aios.net.tcp_send(client, serve_404())
    end

    aios.net.tcp_close(client)
end

-- Start the server
add_log("Starting server...")
local s, err = aios.net.tcp_listen(PORT)
if s then
    server = s
    started = true
    add_log("Listening on port " .. PORT)
    add_log("Root: " .. WWW_ROOT)
else
    add_log("ERROR: " .. (err or "unknown"))
end

-- Main loop
while win:is_running() do
    local sw, sh = win:get_size()
    win:begin_frame()

    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800

    -- Status panel
    local info = aios.net.ifconfig() or {}
    local status_color = started and 0x0044FF44 or 0x00FF4444
    local status_text = started and "RUNNING" or "STOPPED"
    local py = TITLEBAR_H + 8
    chaos_gl.text(12, py, "Status:", sec_c, 0, 0)
    chaos_gl.text(80, py, status_text, status_color, 0, 0)
    chaos_gl.text(12, py + 16, "Address:", sec_c, 0, 0)
    chaos_gl.text(80, py + 16, (info.ip or "...") .. ":" .. PORT, text_c, 0, 0)
    chaos_gl.text(12, py + 32, "Requests:", sec_c, 0, 0)
    chaos_gl.text(80, py + 32, tostring(request_count), accent, 0, 0)

    -- Separator
    local sep_y = py + 50
    chaos_gl.rect(8, sep_y, sw - 16, 1, 0x00444444)

    -- Log area
    chaos_gl.text(12, sep_y + 6, "Request Log:", sec_c, 0, 0)
    local log_y = sep_y + 22
    local visible_lines = 12
    local start_idx = math.max(1, #log - visible_lines + 1)
    for i = start_idx, #log do
        if log_y >= sh - 10 then break end
        local c = text_c
        if log[i]:match("^ERROR") then c = 0x00FF4444
        elseif log[i]:match("200$") then c = 0x0044FF44
        elseif log[i]:match("404$") then c = 0x00FFAA00
        elseif log[i]:match("^#") then c = 0x0088CCFF end
        chaos_gl.text(12, log_y, log[i], c, 0, 0)
        log_y = log_y + 15
    end

    win:end_frame()

    -- Check for incoming connections (non-blocking, very short timeout)
    if started and server then
        local client = aios.net.tcp_accept(server, 1)
        if client then
            handle_client(client)
        end
    end

    -- Events
    for _, event in ipairs(win:poll_events()) do
        -- All close/ESC handling done by AppWindow; no app-specific events needed
    end

    aios.os.sleep(16)
end

-- Cleanup
if server then
    aios.net.tcp_server_close(server)
end
win:destroy()
