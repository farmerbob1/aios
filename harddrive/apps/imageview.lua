-- @app name="Image Viewer" icon="/system/icons/image_32.raw"
-- AIOS v2 — Image Viewer

local W, H = 500, 400
local surface = chaos_gl.surface_create(W, H, false)
chaos_gl.surface_set_position(surface, 120, 80)
chaos_gl.surface_set_visible(surface, true)
aios.wm.register(surface, {
    title = "Image Viewer",
    task_id = aios.task.self().id,
})

local file_path = nil
local tex = -1
local img_w, img_h = 0, 0
local offset_x, offset_y = 0, 0
local zoom = 1
local running = true
local status = "No image loaded"

-- Get file path from args
if arg and arg.file then
    file_path = arg.file
end

local function load_image(path)
    if tex >= 0 then
        chaos_gl.free_texture(tex)
        tex = -1
    end
    tex = chaos_gl.load_texture(path)
    if tex >= 0 then
        img_w, img_h = chaos_gl.texture_get_size(tex)
        file_path = path
        status = path:match("[^/]+$") .. " (" .. img_w .. "x" .. img_h .. ")"
        -- Center the image
        local sw, sh = chaos_gl.surface_get_size(surface)
        local content_h = sh - 28 - 28  -- titlebar + toolbar
        offset_x = math.max(0, (sw - img_w) // 2)
        offset_y = math.max(0, (content_h - img_h) // 2)
        zoom = 1
        -- Update window title
        local name = path:match("[^/]+$") or path
        aios.wm.register(surface, {
            title = "Image Viewer - " .. name,
            task_id = aios.task.self().id,
        })
    else
        status = "Failed to load: " .. path
    end
end

if file_path then
    load_image(file_path)
end

local TITLEBAR_H = 28
local TOOLBAR_H = 28

while running do
    local sw, sh = chaos_gl.surface_get_size(surface)
    chaos_gl.surface_bind(surface)

    local bg = theme and theme.window_bg or 0x002D2D2D
    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local titlebar_bg = theme and theme.titlebar_bg or 0x003C3C3C

    chaos_gl.surface_clear(surface, 0x00181818)

    -- Title bar
    chaos_gl.rect(0, 0, sw, TITLEBAR_H, titlebar_bg)
    chaos_gl.text(8, 6, "Image Viewer", text_c, 0, 0)
    chaos_gl.rect(sw - 28, 0, 28, TITLEBAR_H, 0x00FF4444)
    chaos_gl.text(sw - 20, 6, "X", 0x00FFFFFF, 0, 0)

    -- Toolbar
    chaos_gl.rect(0, TITLEBAR_H, sw, TOOLBAR_H, 0x00353535)
    local btns = {
        {label = "Zoom+", x = 4},
        {label = "Zoom-", x = 0},
        {label = "Fit", x = 0},
        {label = "1:1", x = 0},
    }
    local bx = 4
    for _, b in ipairs(btns) do
        local bw = chaos_gl.text_width(b.label) + 16
        b.x = bx
        b.w = bw
        chaos_gl.rect_rounded(bx, TITLEBAR_H + 4, bw, 20, 3, 0x00444444)
        chaos_gl.text(bx + 8, TITLEBAR_H + 6, b.label, text_c, 0, 0)
        bx = bx + bw + 4
    end

    -- Status
    chaos_gl.text(bx + 8, TITLEBAR_H + 6, status, sec_c, 0, 0)

    -- Image area
    local content_y = TITLEBAR_H + TOOLBAR_H
    local content_h = sh - content_y
    chaos_gl.push_clip(0, content_y, sw, content_h)

    if tex >= 0 then
        local draw_w = math.floor(img_w * zoom)
        local draw_h = math.floor(img_h * zoom)
        local dx = offset_x
        local dy = content_y + offset_y

        if zoom == 1 then
            -- Native size: use appropriate blit
            if chaos_gl.texture_has_alpha(tex) then
                chaos_gl.blit_alpha(dx, dy, img_w, img_h, tex)
            else
                chaos_gl.blit(dx, dy, img_w, img_h, tex)
            end
        else
            -- For zoomed view, draw at native size for now
            -- (no scaling support yet — show at 1:1 with offset)
            if chaos_gl.texture_has_alpha(tex) then
                chaos_gl.blit_alpha(dx, dy, img_w, img_h, tex)
            else
                chaos_gl.blit(dx, dy, img_w, img_h, tex)
            end
        end
    else
        local msg = "Drop or open an image file"
        local mw = chaos_gl.text_width(msg)
        chaos_gl.text((sw - mw) // 2, content_y + content_h // 2 - 8, msg, sec_c, 0, 0)
    end

    chaos_gl.pop_clip()
    chaos_gl.surface_present(surface)

    -- Events
    local event = aios.wm.poll_event(surface)
    while event do
        if event.type == EVENT_CLOSE then
            running = false
        elseif event.type == EVENT_KEY_DOWN then
            if event.key == 1 then -- ESC
                running = false
            elseif event.key == 78 then -- + (numpad)
                zoom = math.min(8, zoom * 1.5)
            elseif event.key == 74 then -- - (numpad)
                zoom = math.max(0.125, zoom / 1.5)
            elseif event.key == 71 then -- Home
                zoom = 1
                local content_h2 = sh - content_y
                offset_x = math.max(0, (sw - img_w) // 2)
                offset_y = math.max(0, (content_h2 - img_h) // 2)
            end
        elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            local mx, my = event.mouse_x, event.mouse_y
            -- Close button
            if mx >= sw - 28 and my < TITLEBAR_H then
                running = false
            -- Toolbar buttons
            elseif my >= TITLEBAR_H and my < TITLEBAR_H + TOOLBAR_H then
                for _, b in ipairs(btns) do
                    if mx >= b.x and mx < b.x + b.w then
                        if b.label == "Zoom+" then
                            zoom = math.min(8, zoom * 1.5)
                        elseif b.label == "Zoom-" then
                            zoom = math.max(0.125, zoom / 1.5)
                        elseif b.label == "1:1" then
                            zoom = 1
                            local ch = sh - content_y
                            offset_x = math.max(0, (sw - img_w) // 2)
                            offset_y = math.max(0, (ch - img_h) // 2)
                        elseif b.label == "Fit" and tex >= 0 then
                            local ch = sh - content_y
                            local scale_x = sw / img_w
                            local scale_y = ch / img_h
                            zoom = math.min(scale_x, scale_y, 1)
                            offset_x = math.max(0, (sw - img_w) // 2)
                            offset_y = math.max(0, (ch - img_h) // 2)
                        end
                        break
                    end
                end
            end
        elseif event.type == EVENT_MOUSE_WHEEL then
            -- Scroll to pan
            offset_y = offset_y + (event.wheel or 0) * 16
        end
        event = aios.wm.poll_event(surface)
    end

    aios.os.sleep(32)
end

-- Cleanup
if tex >= 0 then chaos_gl.free_texture(tex) end
aios.wm.unregister(surface)
chaos_gl.surface_destroy(surface)
