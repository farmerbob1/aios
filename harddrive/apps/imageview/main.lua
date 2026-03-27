-- AIOS v2 — Image Viewer
local AppWindow = require("appwindow")
local Button = require("button")
local Label = require("label")

local win = AppWindow.new("Image Viewer", 500, 400, {x=150, y=80})
local TITLEBAR_H = AppWindow.TITLEBAR_H
local TOOLBAR_H = 28

local file_path = nil
local tex = -1
local img_w, img_h = 0, 0
local offset_x, offset_y = 0, 0
local zoom = 1
local status_text = "No image loaded"

-- Toolbar buttons (using real Button widgets)
local function do_zoom_in() zoom = math.min(8, zoom * 1.5) end
local function do_zoom_out() zoom = math.max(0.125, zoom / 1.5) end
local function do_fit()
    if tex < 0 then return end
    local sw, sh = win:get_size()
    local ch = sh - TITLEBAR_H - TOOLBAR_H
    zoom = math.min(sw / img_w, ch / img_h, 1)
    offset_x = math.max(0, (sw - math.floor(img_w * zoom)) // 2)
    offset_y = math.max(0, (ch - math.floor(img_h * zoom)) // 2)
end
local function do_reset()
    zoom = 1
    local sw, sh = win:get_size()
    local ch = sh - TITLEBAR_H - TOOLBAR_H
    offset_x = math.max(0, (sw - img_w) // 2)
    offset_y = math.max(0, (ch - img_h) // 2)
end

local btn_zoom_in  = Button.new("Zoom+", do_zoom_in, {h=20})
local btn_zoom_out = Button.new("Zoom-", do_zoom_out, {h=20})
local btn_fit      = Button.new("Fit", do_fit, {h=20})
local btn_reset    = Button.new("1:1", do_reset, {h=20})
local status_label = Label.new(status_text)

local toolbar_widgets = {btn_zoom_in, btn_zoom_out, btn_fit, btn_reset}

-- Get file path from args
if arg and arg.file then file_path = arg.file end

local function load_image(path)
    if tex >= 0 then chaos_gl.free_texture(tex); tex = -1 end
    tex = chaos_gl.load_texture(path)
    if tex >= 0 then
        img_w, img_h = chaos_gl.texture_get_size(tex)
        file_path = path
        status_text = path:match("[^/]+$") .. " (" .. img_w .. "x" .. img_h .. ")"
        status_label.text = status_text
        local sw, sh = win:get_size()
        local ch = sh - TITLEBAR_H - TOOLBAR_H
        offset_x = math.max(0, (sw - img_w) // 2)
        offset_y = math.max(0, (ch - img_h) // 2)
        zoom = 1
        win:set_title("Image Viewer - " .. (path:match("[^/]+$") or path))
    else
        status_text = "Failed to load: " .. path
        status_label.text = status_text
    end
end

if file_path then load_image(file_path) end

while win:is_running() do
    local sw, sh = win:get_size()
    win:begin_frame()
    chaos_gl.rect(0, TITLEBAR_H, sw, sh - TITLEBAR_H, 0x00181818)

    -- Toolbar background
    local toolbar_bg = theme and theme.tab_bg or 0x00353535
    chaos_gl.rect(0, TITLEBAR_H, sw, TOOLBAR_H, toolbar_bg)

    -- Draw toolbar widgets
    local bx = 4
    for _, btn in ipairs(toolbar_widgets) do
        btn:draw(bx, TITLEBAR_H + 4)
        local bw = btn:get_size()
        bx = bx + bw + 4
    end
    status_label:draw(bx + 8, TITLEBAR_H + 6)

    -- Image area
    local content_y = TITLEBAR_H + TOOLBAR_H
    local content_h = sh - content_y
    chaos_gl.push_clip(0, content_y, sw, content_h)

    if tex >= 0 then
        local dx, dy = offset_x, content_y + offset_y
        if chaos_gl.texture_has_alpha(tex) then
            chaos_gl.blit_alpha(dx, dy, img_w, img_h, tex)
        else
            chaos_gl.blit(dx, dy, img_w, img_h, tex)
        end
    else
        local sec_c = theme and theme.text_secondary or 0x00AAAAAA
        local msg = "Drop or open an image file"
        chaos_gl.text((sw - chaos_gl.text_width(msg)) // 2, content_y + content_h // 2 - 8, msg, sec_c, 0, 0)
    end
    chaos_gl.pop_clip()
    win:end_frame()

    -- Events — forward to widgets first
    for _, event in ipairs(win:poll_events()) do
        local handled = false
        for _, btn in ipairs(toolbar_widgets) do
            if btn:on_input(event) then handled = true; break end
        end
        if not handled then
            if event.type == EVENT_KEY_DOWN then
                if event.key == 78 then do_zoom_in()
                elseif event.key == 74 then do_zoom_out()
                elseif event.key == 71 then do_reset()
                end
            elseif event.type == EVENT_MOUSE_WHEEL then
                offset_y = offset_y + (event.wheel or 0) * 16
            end
        end
    end
    aios.os.sleep(32)
end

if tex >= 0 then chaos_gl.free_texture(tex) end
win:destroy()
