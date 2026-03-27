-- AIOS Web Browser (Lexbor HTML5 + Image Loading)
local AppWindow = require("appwindow")
local Button = require("button")
local TextField = require("textfield")
local HTTP = require("net/http")

local W, H = 640, 480
local win = AppWindow.new("Browser", W, H, {x=50, y=30})

local TITLEBAR_H = AppWindow.TITLEBAR_H
local TOOLBAR_H = 32
local CONTENT_Y = TITLEBAR_H + TOOLBAR_H
local current_url = ""
local page_title = ""
local page_boxes = nil
local page_height = 0
local scroll_y = 0
local loading = false
local error_msg = nil
local doc_handle = nil
local history = {}
local history_idx = 0
local status_msg = ""  -- bottom status bar text

-- Image cache: url -> texture handle
local img_cache = {}
local img_loading = {}

local function free_doc()
    if doc_handle then pcall(aios.html.free, doc_handle); doc_handle = nil end
    page_boxes = nil; page_height = 0
    -- Free cached textures
    for _, tex in pairs(img_cache) do
        if tex >= 0 then pcall(chaos_gl.texture_free, tex) end
    end
    img_cache = {}; img_loading = {}; img_fetch_count = 0
end

-- Resolve a relative URL against current page
local function resolve_url(href)
    if not href or #href == 0 then return nil end
    if href:match("^https?://") then return href end
    if href:match("^//") then
        local scheme = current_url:match("^(https?)") or "http"
        return scheme .. ":" .. href
    end
    local base = current_url:match("^(https?://[^/]+)") or ""
    if href:sub(1, 1) == "/" then return base .. href end
    local dir = current_url:match("^(.+)/") or base
    return dir .. "/" .. href
end

-- Fetch an image and create a texture (called during render)
local img_fetch_count = 0
local MAX_IMG_FETCHES = 20  -- limit to prevent OOM on image-heavy pages

local function fetch_image(src_url)
    if img_cache[src_url] then return img_cache[src_url] end
    if img_loading[src_url] then return -1 end
    if img_fetch_count >= MAX_IMG_FETCHES then
        img_cache[src_url] = -1
        return -1
    end
    img_loading[src_url] = true
    img_fetch_count = img_fetch_count + 1

    -- Skip very large image URLs (data URIs etc)
    if #src_url > 2048 then img_cache[src_url] = -1; return -1 end

    local ok, body = pcall(HTTP.get, src_url, 5000)
    if ok and body and #body > 0 and #body < 512 * 1024 then
        local tex = chaos_gl.texture_load_from_memory(body)
        if tex and tex >= 0 then
            img_cache[src_url] = tex
            return tex
        end
    end
    img_cache[src_url] = -1
    return -1
end

-- Render HTML source
local function render_html(source)
    free_doc()
    error_msg = nil
    local ok, d = pcall(aios.html.parse, source)
    if not ok or not d then error_msg = "Failed to parse HTML"; return end
    doc_handle = d

    -- Fetch and attach external CSS stylesheets
    if aios.html.get_stylesheets and aios.html.attach_css then
        local csok, css_list = pcall(aios.html.get_stylesheets, d)
        if csok and css_list and #css_list > 0 then
            print("[browser] found " .. #css_list .. " stylesheets")
            local css_count = 0
            local MAX_CSS_FETCH = 6
            for _, entry in ipairs(css_list) do
                if css_count >= MAX_CSS_FETCH then break end
                if entry.url then
                    local css_url = resolve_url(entry.url)
                    if css_url then
                        css_count = css_count + 1
                        status_msg = "Fetching CSS " .. css_count .. "..."
                        local fok, body, fstatus, fheaders = pcall(HTTP.get, css_url, 8000)
                        if fok and body and #body > 0 and #body < 512 * 1024 then
                            -- Check truncation
                            local expected = fheaders and fheaders["content-length"] and tonumber(fheaders["content-length"])
                            if expected and #body < expected / 2 then
                                print("[browser] CSS " .. css_count .. " truncated: " .. #body .. "/" .. expected)
                            else
                                local first = body:sub(1, 20)
                                if not first:match("^%s*<!DOCTYPE") and not first:match("^%s*<html") then
                                    print("[browser] attaching CSS " .. css_count .. ": " .. #body .. " bytes")
                                    pcall(aios.html.attach_css, d, body)
                                end
                            end
                        end
                    end
                end
            end
        end
    end

    -- Execute scripts (two-phase: discover, fetch external, execute all)
    if aios.html.get_scripts and aios.html.exec_scripts then
        local sok, script_list = pcall(aios.html.get_scripts, d)
        if sok and script_list then
            print("[browser] found " .. #script_list .. " scripts")
            for i, e in ipairs(script_list) do
                if e.type == "inline" then
                    print("[browser]   #" .. i .. " inline " .. #e.code .. "b")
                else
                    print("[browser]   #" .. i .. " external: " .. (e.url or "?"))
                end
            end
        end
        if sok and script_list and #script_list > 0 then
            local resolved = {}
            local total_bytes = 0
            local MAX_SCRIPT_BYTES = 512 * 1024  -- 512KB total script budget
            local MAX_SINGLE_SCRIPT = 256 * 1024 -- 256KB per script
            local MAX_EXTERNAL = 8               -- max external fetches

            local ext_count = 0
            for _, entry in ipairs(script_list) do
                if total_bytes >= MAX_SCRIPT_BYTES then break end

                if entry.type == "inline" and entry.code then
                    if #entry.code <= MAX_SINGLE_SCRIPT then
                        resolved[#resolved + 1] = entry.code
                        total_bytes = total_bytes + #entry.code
                    end
                elseif entry.type == "external" and entry.url and ext_count < MAX_EXTERNAL then
                    local script_url = resolve_url(entry.url)
                    if script_url then
                        ext_count = ext_count + 1
                        status_msg = "Fetching script " .. ext_count .. "..."
                        local fok, body, fstatus, fheaders = pcall(HTTP.get, script_url, 5000)
                        print("[browser] fetched ext #" .. ext_count .. ": ok=" .. tostring(fok) .. " size=" .. (body and #body or 0))
                        if fok and body and #body > 0 then
                            print("[browser]   first 80: " .. body:sub(1, 80))
                            -- Check for truncation: if Content-Length known and we got less than half, skip
                            local expected = fheaders and fheaders["content-length"] and tonumber(fheaders["content-length"])
                            if expected and #body < expected / 2 then
                                print("[browser]   TRUNCATED: got " .. #body .. " of " .. expected .. ", skipping")
                                body = nil
                            end
                        end
                        if fok and body and #body > 0 and #body <= MAX_SINGLE_SCRIPT then
                            local first = body:sub(1, 20)
                            if not first:match("^%s*<!DOCTYPE") and not first:match("^%s*<html") then
                                resolved[#resolved + 1] = body
                                total_bytes = total_bytes + #body
                            end
                        end
                    end
                end
            end
            if #resolved > 0 then
                status_msg = "Executing " .. #resolved .. " scripts..."
                local eok, eerr = pcall(aios.html.exec_scripts, d, resolved)
                if not eok then
                    print("[browser] exec_scripts FAILED: " .. tostring(eerr))
                else
                    print("[browser] exec_scripts done, " .. #resolved .. " scripts")
                end
            end
        end
    end

    local ok2, t = pcall(aios.html.title, d)
    page_title = (ok2 and t and #t > 0) and t or ""
    local sw = win:get_size()
    local ok3, boxes, height = pcall(aios.html.layout, d, sw - 16)
    if not ok3 or not boxes then error_msg = "Layout failed"; return end
    page_boxes = boxes
    page_height = height or 0
    scroll_y = 0
end

-- Navigate to URL
local function navigate(url)
    if not url or #url == 0 then return end
    if url:match("^about:") then
        render_html(home_html)
        current_url = "about:home"; loading = false
        win.title = "AIOS Browser"; return
    end
    if not url:match("^https?://") and not url:match("^file://") then
        url = "http://" .. url
    end
    loading = true; error_msg = nil
    current_url = url; url_text = url; url_cursor = #url
    win.title = "Loading..."
    status_msg = "Connecting..."

    if url:match("^file://") then
        local path = url:sub(8)
        local fd = aios.io.open(path, "r")
        if fd then
            local content = aios.io.read(fd, "a"); aios.io.close(fd)
            if content then render_html(content); loading = false
                win.title = page_title ~= "" and (page_title .. " - Browser") or "Browser"; return end
        end
        error_msg = "File not found: " .. path; loading = false; win.title = "Browser"; return
    end

    status_msg = "Downloading " .. url:match("//([^/]+)") .. "..."
    local ok, body, status, headers = pcall(HTTP.get, url, 15000)
    loading = false
    status_msg = "Rendering..."
    if not ok then error_msg = "Error: " .. tostring(body); win.title = "Browser"; return end
    if not body then error_msg = "Could not connect: " .. tostring(headers or "unknown"); win.title = "Browser"; return end
    status = status or 0

    -- Follow redirects
    if (status == 301 or status == 302) and headers then
        local loc = headers["location"]
        if loc then navigate(loc); return end
    end

    -- Check content type — don't render binary as HTML
    local ct = headers and (headers["content-type"] or "") or ""
    if ct:match("image/") then
        -- Render image directly
        local tex = chaos_gl.texture_load_from_memory(body)
        if tex and tex >= 0 then
            local iw, ih = chaos_gl.texture_get_size(tex)
            local img_html = string.format(
                '<html><body style="background:#222"><p style="color:#aaa">Image: %s</p>'..
                '<p style="color:#666">%dx%d pixels</p></body></html>', url, iw or 0, ih or 0)
            render_html(img_html)
            -- Store the texture for this URL
            img_cache[url] = tex
        else
            error_msg = "Failed to decode image"
        end
        win.title = "Browser"; return
    end

    if status ~= 200 and status ~= 0 and #body > 0 then
        render_html(body)
        win.title = "HTTP " .. tostring(status) .. " - Browser"; return
    end
    if status ~= 200 and status ~= 0 then
        error_msg = "HTTP " .. tostring(status); win.title = "Browser"; return
    end

    render_html(body)
    win.title = page_title ~= "" and (page_title .. " - Browser") or "Browser"
    status_msg = "Done — " .. (body and #body or 0) .. " bytes"
end

local function go_to(url)
    while #history > history_idx do table.remove(history) end
    history[#history + 1] = url; history_idx = #history
    navigate(url)
end
local function go_back() if history_idx > 1 then history_idx = history_idx - 1; navigate(history[history_idx]) end end
local function go_forward() if history_idx < #history then history_idx = history_idx + 1; navigate(history[history_idx]) end end
local function go_home() go_to("about:home") end

-- Home page
home_html = [[
<html><head><title>AIOS Browser</title></head>
<body>
<h1>AIOS Browser</h1>
<p>Welcome to the AIOS web browser — powered by the <b>Lexbor</b> HTML5 parser with a custom layout engine and <b>ChaosGL</b> rendering.</p>
<hr>
<h2>Try These Sites</h2>
<ul>
<li><a href="http://www.google.com">google.com</a> — The world's most popular search engine</li>
<li><a href="https://en.wikipedia.org">wikipedia.org</a> — The free encyclopedia</li>
<li><a href="https://old.reddit.com">old.reddit.com</a> — Reddit classic interface</li>
<li><a href="http://info.cern.ch">info.cern.ch</a> — The world's first website (1991)</li>
<li><a href="http://example.com">example.com</a> — IANA example domain</li>
<li><a href="http://motherfuckingwebsite.com">motherfuckingwebsite.com</a> — A perfectly simple page</li>
</ul>
<h2>Features</h2>
<ul>
<li><b>HTML5 parsing</b> via Lexbor (Apache 2.0)</li>
<li><b>Inline CSS</b> — color, background, margin, padding, font-size, border, display</li>
<li><b>Block layout</b> — headings, paragraphs, lists, blockquotes, preformatted text</li>
<li><b>Tables</b> — grid layout with cell borders</li>
<li><b>Images</b> — fetched via HTTP, decoded with stb_image</li>
<li><b>Forms</b> — input fields and buttons rendered visually</li>
<li><b>Links</b> — click to navigate, back/forward history</li>
</ul>
<h2>Keyboard Shortcuts</h2>
<ul>
<li><b>Ctrl+L</b> — Focus the URL bar</li>
<li><b>Enter</b> — Navigate to URL</li>
<li><b>Escape</b> — Unfocus URL bar</li>
</ul>
<blockquote>
<p><i>"The web as I envisaged it, we have not seen it yet."</i> — Tim Berners-Lee</p>
</blockquote>
<hr>
<p><code>AIOS v2.0</code> — A hobby operating system built from scratch.</p>
</body></html>
]]

render_html(home_html)
current_url = "about:home"
history = {"about:home"}; history_idx = 1

-- Toolbar widgets
local btn_back = Button.new("<", function() go_back() end, {w=24, h=24})
local btn_fwd  = Button.new(">", function() go_forward() end, {w=24, h=24})
local btn_home = Button.new("H", function() go_home() end, {w=24, h=24})
local btn_snap = Button.new("S", function()
    if not page_boxes or #page_boxes == 0 then
        status_msg = "No page to capture"
        return
    end
    status_msg = "Capturing full page..."

    -- Create offscreen surface at full page dimensions
    local sw = win:get_size()
    local full_h = page_height + 20
    if full_h < 100 then full_h = 100 end
    if full_h > 16000 then full_h = 16000 end  -- sanity cap

    local snap_surf = chaos_gl.surface_create(sw, full_h, false)
    if not snap_surf or snap_surf < 0 then
        status_msg = "Failed to create capture surface"
        return
    end
    chaos_gl.surface_set_visible(snap_surf, false)
    chaos_gl.surface_bind(snap_surf)
    chaos_gl.surface_clear(snap_surf, 0x00FFFFFF)

    -- Render all boxes at their absolute positions (no scroll offset)
    local ox, oy = 8, 4
    local fh = 14
    for pass = 1, 2 do
        for i = 1, #page_boxes do
            local b = page_boxes[i]
            if not b then break end
            local bx, by = b.x + ox, b.y + oy
            if pass == 1 then
                if b.type == 3 and b.bg and b.bg ~= 0 then
                    chaos_gl.rect(bx, by, b.w or 0, b.h or 0, b.bg)
                    if b.fg and b.fg ~= 0 then
                        chaos_gl.rect_outline(bx, by, b.w or 0, b.h or 0, b.fg, 1)
                    end
                end
            else
                if b.type == 1 then
                    local c = (b.fg and b.fg ~= 0) and b.fg or 0x00222222
                    chaos_gl.text(bx, by, b.text or "", c, 0, 0)
                    if b.bold then chaos_gl.text(bx + 1, by, b.text or "", c, 0, 0) end
                    if b.underline then chaos_gl.rect(bx, by + (b.h or 12), b.w or 0, 1, c) end
                elseif b.type == 2 then
                    chaos_gl.rect(bx, by, b.w or (sw - 16), 1, b.fg or 0x00CCCCCC)
                elseif b.type == 4 then
                    chaos_gl.circle(bx + 3, by + (b.h or 14) // 2, 2, b.fg or 0x00222222)
                elseif b.type == 5 then
                    local src = b.url and #b.url > 0 and resolve_url(b.url) or nil
                    local tex = src and fetch_image(src) or -1
                    if tex and tex >= 0 then
                        chaos_gl.blit(bx, by, b.w or 200, b.h or 40, tex)
                    else
                        chaos_gl.rect(bx, by, b.w or 200, b.h or 40, 0x00F0F0F0)
                    end
                end
            end
        end
    end

    -- Save and cleanup
    local ok = chaos_gl.surface_save_bmp(snap_surf, "/screenshot.bmp")
    chaos_gl.surface_bind(win.surface)  -- rebind browser surface
    chaos_gl.surface_destroy(snap_surf)

    if ok then
        status_msg = "Full page saved (" .. sw .. "x" .. full_h .. ")"
    else
        status_msg = "Screenshot save failed"
    end
end, {w=24, h=24})
local url_bar  = TextField.new({
    placeholder = "Enter URL...",
    on_submit = function(text) go_to(text) end,
    w = W - 130, h = 24,
})
local toolbar_widgets = {btn_back, btn_fwd, btn_home, btn_snap, url_bar}

-- ═══ Main Loop ═══

while win:is_running() do
    local sw, sh = win:get_size()
    win:begin_frame()

    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800
    local toolbar_bg = theme and theme.tab_bg or 0x00333333
    local field_bg = theme and theme.field_bg or 0x00222222
    local btn_bg = theme and theme.button_normal or 0x00444444

    -- Toolbar
    chaos_gl.rect(0, TITLEBAR_H, sw, TOOLBAR_H, toolbar_bg)

    -- Enable/disable back/forward based on history
    btn_back.enabled = (history_idx > 1)
    btn_fwd.enabled = (history_idx < #history)

    -- Resize URL bar to fill available width
    url_bar.w = sw - 130

    -- Draw toolbar widgets
    btn_back:draw(4, TITLEBAR_H + 4)
    btn_fwd:draw(32, TITLEBAR_H + 4)
    btn_home:draw(60, TITLEBAR_H + 4)
    btn_snap:draw(88, TITLEBAR_H + 4)
    url_bar:draw(118, TITLEBAR_H + 4)

    -- Status bar height
    local STATUS_H = 18

    -- Content
    local content_h = sh - CONTENT_Y - STATUS_H
    chaos_gl.push_clip(0, CONTENT_Y, sw, content_h)
    chaos_gl.rect(0, CONTENT_Y, sw, content_h, 0x00FFFFFF)

    if loading then
        chaos_gl.text(sw // 2 - 30, CONTENT_Y + content_h // 2, "Loading...", 0x00444444, 0, 0)
    elseif error_msg then
        chaos_gl.text(16, CONTENT_Y + 20, "Error", 0x00CC4444, 0, 0)
        chaos_gl.text(16, CONTENT_Y + 40, error_msg, 0x00666666, 0, 0)
    elseif page_boxes then
        local ox, oy = 8, CONTENT_Y - scroll_y + 4
        local box_count = #page_boxes
        if box_count > 4000 then box_count = 4000 end  -- safety cap
        -- Two passes: backgrounds first, then text/content on top
        for pass = 1, 2 do
            for i = 1, box_count do
                local b = page_boxes[i]
                if not b then break end
                local bx, by = b.x + ox, b.y + oy
                if by + (b.h or 14) >= CONTENT_Y and by < CONTENT_Y + content_h then
                    if pass == 1 then
                        -- Background pass
                        if b.type == 3 and b.bg and b.bg ~= 0 then
                            chaos_gl.rect(bx, by, b.w or 0, b.h or 0, b.bg)
                            -- Border
                            if b.fg and b.fg ~= 0 then
                                chaos_gl.rect_outline(bx, by, b.w or 0, b.h or 0, b.fg, 1)
                            end
                        end
                    else
                        -- Content pass
                        if b.type == 1 then -- text
                            local c = (b.fg and b.fg ~= 0) and b.fg or 0x00222222
                            chaos_gl.text(bx, by, b.text or "", c, 0, 0)
                            if b.bold then chaos_gl.text(bx + 1, by, b.text or "", c, 0, 0) end
                            if b.underline then chaos_gl.rect(bx, by + (b.h or 12), b.w or 0, 1, c) end
                        elseif b.type == 2 then -- hr
                            chaos_gl.rect(bx, by, b.w or (sw - 16), 1, b.fg or 0x00CCCCCC)
                        elseif b.type == 4 then -- bullet
                            chaos_gl.circle(bx + 3, by + (b.h or 14) // 2, 2, b.fg or 0x00222222)
                        elseif b.type == 5 then -- image
                            local src = b.url and #b.url > 0 and resolve_url(b.url) or nil
                            local tex = src and fetch_image(src) or -1
                            if tex and tex >= 0 then
                                local iw, ih = chaos_gl.texture_get_size(tex)
                                local dw, dh = b.w or 200, b.h or 40
                                -- Scale to fit box
                                if iw and ih and iw > 0 and ih > 0 then
                                    local scale = math.min(dw / iw, dh / ih, 1)
                                    dw = math.floor(iw * scale)
                                    dh = math.floor(ih * scale)
                                end
                                chaos_gl.blit(bx, by, dw, dh, tex)
                            else
                                -- Placeholder
                                chaos_gl.rect(bx, by, b.w or 200, b.h or 40, 0x00F0F0F0)
                                chaos_gl.rect_outline(bx, by, b.w or 200, b.h or 40, 0x00CCCCCC, 1)
                                local label = b.text or "[IMG]"
                                local lw = chaos_gl.text_width(label)
                                chaos_gl.text(bx + ((b.w or 200) - lw) // 2,
                                              by + ((b.h or 40) - 12) // 2, label, 0x00999999, 0, 0)
                            end
                        end
                    end
                end
            end
        end
    end
    chaos_gl.pop_clip()

    -- Scrollbar (track + thumb)
    local sb_visible = page_height > content_h and page_height > 0
    local sb_x, sb_w = sw - 8, 6
    local sb_thumb_h, sb_thumb_y = 0, 0
    if sb_visible then
        sb_thumb_h = math.max(20, content_h * content_h // page_height)
        sb_thumb_y = CONTENT_Y + scroll_y * (content_h - sb_thumb_h) // math.max(1, page_height - content_h)
        local track_c = theme and theme.slider_track or 0x00333333
        local thumb_c = theme and theme.slider_thumb_color or 0x00888888
        chaos_gl.rect(sb_x, CONTENT_Y, sb_w, content_h, track_c)
        chaos_gl.rect(sb_x, sb_thumb_y, sb_w, sb_thumb_h, thumb_c)
    end

    -- Status bar at bottom
    local status_y = sh - STATUS_H
    chaos_gl.rect(0, status_y, sw, STATUS_H, toolbar_bg)
    if status_msg and #status_msg > 0 then
        chaos_gl.text(6, status_y + 3, status_msg, sec_c, 0, 0)
    end

    win:end_frame()

    -- ═══ Events ═══
    for _, event in ipairs(win:poll_events()) do
        -- Forward to toolbar widgets first
        local handled = false
        for _, w in ipairs(toolbar_widgets) do
            if w:on_input(event) then handled = true; break end
        end

        if not handled then
            -- Ctrl+L focuses URL bar
            if event.type == EVENT_KEY_DOWN and event.ctrl and event.key == 38 then
                url_bar.focused = true
                url_bar.cursor_pos = #url_bar.text
                url_bar._cursor_visible = true

            -- Page scrolling (when URL bar not focused)
            elseif event.type == EVENT_KEY_DOWN and not url_bar.focused then
                if event.key == 200 then scroll_y = math.max(0, scroll_y - 20)
                elseif event.key == 208 then scroll_y = math.min(math.max(0, page_height - (sh - CONTENT_Y)), scroll_y + 20)
                elseif event.key == 201 then scroll_y = math.max(0, scroll_y - (sh - CONTENT_Y))
                elseif event.key == 209 then scroll_y = math.min(math.max(0, page_height - (sh - CONTENT_Y)), scroll_y + (sh - CONTENT_Y))
                elseif event.key == 199 then scroll_y = 0
                elseif event.key == 207 then scroll_y = math.max(0, page_height - (sh - CONTENT_Y))
                end

            -- Click in content area
            elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
                local mx, my = event.mouse_x, event.mouse_y

                -- Scrollbar click-to-jump
                if sb_visible and mx >= sb_x and mx < sb_x + sb_w and my >= CONTENT_Y then
                    local ch2 = sh - CONTENT_Y
                    local click_frac = (my - CONTENT_Y) / ch2
                    scroll_y = math.floor(click_frac * math.max(0, page_height - ch2))
                    scroll_y = math.max(0, math.min(scroll_y, math.max(0, page_height - ch2)))

                -- Link clicking
                elseif my >= CONTENT_Y and page_boxes then
                    local ox, oy = 8, CONTENT_Y - scroll_y + 4
                    for i = 1, #page_boxes do
                        local b = page_boxes[i]
                        if not b then break end
                        if b.url and #b.url > 0 and b.type == 1 then
                            local bx, by = b.x + ox, b.y + oy
                            if mx >= bx and mx < bx + (b.w or 0) and
                               my >= by and my < by + (b.h or 14) then
                                url_bar.text = resolve_url(b.url) or b.url
                                go_to(url_bar.text)
                                break
                            end
                        end
                    end
                end

            -- Mouse wheel scroll
            elseif event.type == EVENT_MOUSE_WHEEL then
                local ch2 = sh - CONTENT_Y
                scroll_y = scroll_y + (event.wheel or 0) * -40
                scroll_y = math.max(0, math.min(scroll_y, math.max(0, page_height - ch2)))
            end
        end
    end
    aios.os.sleep(16)
end
free_doc(); win:destroy()
