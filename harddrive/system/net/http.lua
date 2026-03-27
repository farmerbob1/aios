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

-- Parse HTTP response using picohttpparser (C library via aios.net.parse_http)
local function parse_response(raw)
    return aios.net.parse_http(raw)
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

    -- Send HTTP/1.1 request with Connection: close
    local request = "GET " .. parsed.path .. " HTTP/1.1\r\n" ..
                    "Host: " .. parsed.host .. "\r\n" ..
                    "User-Agent: AIOS/2.0\r\n" ..
                    "Accept: text/html,*/*\r\n" ..
                    "Accept-Encoding: identity\r\n" ..
                    "Connection: close\r\n" ..
                    "\r\n"

    local ok2, serr = send_fn(sock, request)
    if not ok2 then
        close_fn(sock)
        return nil, 0, "send failed: " .. (serr or "unknown")
    end

    -- Phase 1: Accumulate data until headers are complete
    local buf = ""
    local status, headers
    local chunk_to = 5000

    while #buf < 65536 do
        local data = recv_fn(sock, 8192, chunk_to)
        if not data then break end
        buf = buf .. data

        -- Try parsing headers from accumulated data
        print("[HTTP] try parse, buf=" .. #buf)
        local s, h, body_start = aios.net.parse_http(buf)
        if s then
            print("[HTTP] parsed! status=" .. s .. " body=" .. #body_start)
            status = s
            headers = h
            buf = body_start  -- remaining data after headers = start of body
            break
        end
    end

    if not status then
        close_fn(sock)
        return buf, 0, nil  -- return raw as body fallback
    end

    -- Phase 2: Read body — use Content-Length if available
    local content_length = headers and headers["content-length"]
    content_length = content_length and tonumber(content_length)

    if content_length and content_length > 0 then
        while #buf < content_length and #buf < 1024 * 1024 do
            local want = content_length - #buf
            if want > 8192 then want = 8192 end
            local data = recv_fn(sock, want, chunk_to)
            if not data then break end
            buf = buf .. data
        end
    else
        -- No content-length: read until connection closes (short timeout per chunk)
        while #buf < 1024 * 1024 do
            local data = recv_fn(sock, 8192, 2000)
            if not data then break end
            buf = buf .. data
        end
    end

    local te = headers and headers["transfer-encoding"] or nil

    -- Decode chunked transfer encoding if needed
    if te and te:lower():find("chunked") then
        local decoded = {}
        local pos = 1
        while pos <= #buf do
            -- Find end of chunk size line
            local nl = buf:find("\r\n", pos)
            if not nl then break end
            local hex = buf:sub(pos, nl - 1):match("^%s*(%x+)")
            if not hex then break end
            local chunk_size = tonumber(hex, 16)
            if not chunk_size or chunk_size == 0 then break end
            local data_start = nl + 2
            local data_end = data_start + chunk_size - 1
            if data_end > #buf then
                -- Partial chunk — take what we have
                decoded[#decoded + 1] = buf:sub(data_start)
                break
            end
            decoded[#decoded + 1] = buf:sub(data_start, data_end)
            pos = data_end + 3  -- skip \r\n after chunk data
        end
        buf = table.concat(decoded)
    end

    close_fn(sock)
    return buf, status, headers
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
