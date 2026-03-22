-- AIOS v2 — HTTP/1.0 Client Library
-- Pure Lua, built on aios.net TCP functions.
-- Usage: local http = require("http")
--        local body, status, headers = http.get("http://example.com/path", 5000)

local http = {}

-- Parse URL into components
local function parse_url(url)
    local scheme, rest = url:match("^(https?)://(.+)$")
    if not scheme then return nil end

    local host_port, path = rest:match("^([^/]+)(/.*)$")
    if not host_port then
        host_port = rest
        path = "/"
    end

    local host, port = host_port:match("^(.+):(%d+)$")
    if not host then
        host = host_port
        port = (scheme == "https") and 443 or 80
    else
        port = tonumber(port)
    end

    return {
        scheme = scheme,
        host = host,
        port = port,
        path = path,
    }
end

-- Parse HTTP response into status, headers, body
local function parse_response(raw)
    -- Find end of headers
    local header_end = raw:find("\r\n\r\n")
    if not header_end then
        return nil, nil, raw
    end

    local header_part = raw:sub(1, header_end - 1)
    local body = raw:sub(header_end + 4)

    -- Parse status line
    local status = tonumber(header_part:match("HTTP/%d%.%d (%d+)"))

    -- Parse headers into table
    local headers = {}
    for line in header_part:gmatch("[^\r\n]+") do
        local key, val = line:match("^([^:]+):%s*(.+)$")
        if key then
            headers[key:lower()] = val
        end
    end

    return status, headers, body
end

-- HTTP GET request
function http.get(url, timeout)
    timeout = timeout or 10000

    local parsed = parse_url(url)
    if not parsed then
        return nil, 0, "invalid URL"
    end

    local is_https = (parsed.scheme == "https")
    local sock, err

    if is_https then
        sock, err = aios.net.tls_connect(parsed.host, parsed.port, timeout)
    else
        sock, err = aios.net.tcp_connect(parsed.host, parsed.port, timeout)
    end
    if not sock then
        return nil, 0, "connect failed: " .. (err or "unknown")
    end

    -- Send/recv functions adapt based on protocol
    local send_fn = is_https and aios.net.tls_send or aios.net.tcp_send
    local recv_fn = is_https and aios.net.tls_recv or aios.net.tcp_recv
    local close_fn = is_https and aios.net.tls_close or aios.net.tcp_close

    -- Send request
    local request = "GET " .. parsed.path .. " HTTP/1.0\r\n" ..
                    "Host: " .. parsed.host .. "\r\n" ..
                    "User-Agent: AIOS/2.0\r\n" ..
                    "Connection: close\r\n" ..
                    "\r\n"

    local ok, serr = send_fn(sock, request)
    if not ok then
        close_fn(sock)
        return nil, 0, "send failed: " .. (serr or "unknown")
    end

    -- Receive response (read until connection closes)
    local parts = {}
    local total = 0
    while true do
        local data = recv_fn(sock, 4096, timeout)
        if not data then break end
        parts[#parts + 1] = data
        total = total + #data
        if total > 1024 * 1024 then break end  -- 1MB limit
    end

    close_fn(sock)

    local raw = table.concat(parts)
    local status, headers, body = parse_response(raw)

    return body, status or 0, headers
end

-- HTTP POST request
function http.post(url, body_data, content_type, timeout)
    timeout = timeout or 10000
    content_type = content_type or "application/x-www-form-urlencoded"

    local parsed = parse_url(url)
    if not parsed then
        return nil, 0, "invalid URL"
    end

    local is_https = (parsed.scheme == "https")
    local sock, err

    if is_https then
        sock, err = aios.net.tls_connect(parsed.host, parsed.port, timeout)
    else
        sock, err = aios.net.tcp_connect(parsed.host, parsed.port, timeout)
    end
    if not sock then
        return nil, 0, "connect failed: " .. (err or "unknown")
    end

    local send_fn = is_https and aios.net.tls_send or aios.net.tcp_send
    local recv_fn = is_https and aios.net.tls_recv or aios.net.tcp_recv
    local close_fn = is_https and aios.net.tls_close or aios.net.tcp_close

    local request = "POST " .. parsed.path .. " HTTP/1.0\r\n" ..
                    "Host: " .. parsed.host .. "\r\n" ..
                    "User-Agent: AIOS/2.0\r\n" ..
                    "Content-Type: " .. content_type .. "\r\n" ..
                    "Content-Length: " .. tostring(#body_data) .. "\r\n" ..
                    "Connection: close\r\n" ..
                    "\r\n" ..
                    body_data

    local ok, serr = send_fn(sock, request)
    if not ok then
        close_fn(sock)
        return nil, 0, "send failed: " .. (serr or "unknown")
    end

    local parts = {}
    local total = 0
    while true do
        local data = recv_fn(sock, 4096, timeout)
        if not data then break end
        parts[#parts + 1] = data
        total = total + #data
        if total > 1024 * 1024 then break end
    end

    close_fn(sock)

    local raw = table.concat(parts)
    local status, headers, resp_body = parse_response(raw)

    return resp_body, status or 0, headers
end

return http
