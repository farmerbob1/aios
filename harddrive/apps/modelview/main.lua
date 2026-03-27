-- AIOS v2 — 3D Model Viewer
local AppWindow = require("appwindow")
local Button = require("button")
local Label = require("label")

local win = AppWindow.new("3D Viewer", 500, 400, {x=160, y=90, depth=true})
local TITLEBAR_H = AppWindow.TITLEBAR_H
local TOOLBAR_H = 28

local file_path = nil
local model = nil
local status = "No model loaded"

-- Camera
local rot_x, rot_y = 25, 45
local distance = 4
local dragging = false
local drag_start_x, drag_start_y = 0, 0
local drag_start_rx, drag_start_ry = 0, 0
local render_mode = "gouraud"

-- Toolbar buttons (real widgets)
local btn_gouraud = Button.new("Gouraud", function() render_mode = "gouraud" end, {h=20})
local btn_flat    = Button.new("Flat",    function() render_mode = "flat" end, {h=20})
local btn_wire    = Button.new("Wire",    function() render_mode = "wireframe" end, {h=20})
local status_label = Label.new(status)
local toolbar_btns = {btn_gouraud, btn_flat, btn_wire}

if arg and arg.file then file_path = arg.file end

local function load_model_file(path)
    if model then chaos_gl.free_model(model); model = nil end
    model = chaos_gl.load_model(path)
    if model then
        file_path = path
        status = path:match("[^/]+$") or path
        status_label.text = status
        win:set_title("3D Viewer - " .. status)
    else
        status = "Failed to load: " .. path
        status_label.text = status
    end
end

if file_path then load_model_file(file_path) end

while win:is_running() do
    local sw, sh = win:get_size()
    win:begin_frame()
    chaos_gl.rect(0, TITLEBAR_H, sw, sh - TITLEBAR_H, 0x00202030)

    -- Toolbar
    local toolbar_bg = theme and theme.tab_bg or 0x00353535
    chaos_gl.rect(0, TITLEBAR_H, sw, TOOLBAR_H, toolbar_bg)

    -- Highlight active mode button
    btn_gouraud.pressed = (render_mode == "gouraud")
    btn_flat.pressed    = (render_mode == "flat")
    btn_wire.pressed    = (render_mode == "wireframe")

    local bx = 4
    for _, btn in ipairs(toolbar_btns) do
        btn:draw(bx, TITLEBAR_H + 4)
        local bw = btn:get_size()
        bx = bx + bw + 4
    end
    status_label:draw(bx + 8, TITLEBAR_H + 6)

    -- 3D viewport
    local vp_y = TITLEBAR_H + TOOLBAR_H
    local vp_h = sh - vp_y
    chaos_gl.push_clip(0, vp_y, sw, vp_h)

    if model then
        local aspect = sw / vp_h
        chaos_gl.set_perspective(45, aspect, 0.1, 100)
        local rad_x = rot_x * 3.14159 / 180
        local rad_y = rot_y * 3.14159 / 180
        local cam_x = distance * math.sin(rad_y) * math.cos(rad_x)
        local cam_y = distance * math.sin(rad_x)
        local cam_z = distance * math.cos(rad_y) * math.cos(rad_x)
        chaos_gl.set_camera(cam_x, cam_y, cam_z, 0, 0, 0, 0, 1, 0)
        chaos_gl.set_transform(0, 0, 0, 0, 0, 0, 1, 1, 1)

        if render_mode == "wireframe" then
            chaos_gl.draw_wireframe(model, 0x0000FF88)
        elseif render_mode == "flat" then
            chaos_gl.draw_model(model, "flat", {color = 0x006688AA})
        else
            chaos_gl.draw_model(model, "gouraud", {
                light_dir_x = 0.5, light_dir_y = -0.7, light_dir_z = 0.5,
                ambient = 0.15, color = 0x006688CC,
            })
        end
        chaos_gl.set_transform(0, 0, 0, 0, 0, 0, 0.5, 0.5, 0.5)
    else
        local sec_c = theme and theme.text_secondary or 0x00AAAAAA
        local msg = "Open a .cobj model file"
        chaos_gl.text((sw - chaos_gl.text_width(msg)) // 2, vp_y + vp_h // 2 - 8, msg, sec_c, 0, 0)
    end
    chaos_gl.pop_clip()
    win:end_frame()

    -- Events
    for _, event in ipairs(win:poll_events()) do
        local handled = false
        for _, btn in ipairs(toolbar_btns) do
            if btn:on_input(event) then handled = true; break end
        end
        if not handled then
            if event.type == EVENT_MOUSE_MOVE then
                if dragging then
                    rot_y = drag_start_ry + (event.mouse_x - drag_start_x) * 0.5
                    rot_x = drag_start_rx + (event.mouse_y - drag_start_y) * 0.5
                    if rot_x > 89 then rot_x = 89 end
                    if rot_x < -89 then rot_x = -89 end
                end
            elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
                if event.mouse_y >= vp_y then
                    dragging = true
                    drag_start_x = event.mouse_x; drag_start_y = event.mouse_y
                    drag_start_rx = rot_x; drag_start_ry = rot_y
                end
            elseif event.type == EVENT_MOUSE_UP then
                dragging = false
            elseif event.type == EVENT_MOUSE_WHEEL then
                distance = distance - (event.wheel or 0) * 0.5
                distance = math.max(1, math.min(20, distance))
            end
        end
    end

    aios.os.sleep(16)
end

if model then chaos_gl.free_model(model) end
win:destroy()
