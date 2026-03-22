-- @app name="3D Viewer" icon="/system/icons/cobj_32.raw"
-- AIOS v2 — 3D Model Viewer

local W, H = 480, 400
local surface = chaos_gl.surface_create(W, H, true)  -- depth buffer enabled
chaos_gl.surface_set_position(surface, 140, 70)
chaos_gl.surface_set_visible(surface, true)
aios.wm.register(surface, {
    title = "3D Viewer",
    task_id = aios.task.self().id,
})

local file_path = nil
local model = nil
local running = true
local status = "No model loaded"

-- Camera
local rot_x = 25
local rot_y = 45
local distance = 4
local dragging = false
local drag_start_x, drag_start_y = 0, 0
local drag_start_rx, drag_start_ry = 0, 0

-- Render mode
local render_mode = "gouraud"  -- "gouraud", "flat", "wireframe"

-- Get file path from args
if arg and arg.file then
    file_path = arg.file
end

local function load_model_file(path)
    if model then
        chaos_gl.free_model(model)
        model = nil
    end
    model = chaos_gl.load_model(path)
    if model then
        file_path = path
        local name = path:match("[^/]+$") or path
        status = name
        aios.wm.register(surface, {
            title = "3D Viewer - " .. name,
            task_id = aios.task.self().id,
        })
    else
        status = "Failed to load: " .. path
    end
end

if file_path then
    load_model_file(file_path)
end

local TITLEBAR_H = 28
local TOOLBAR_H = 28

while running do
    local sw, sh = chaos_gl.surface_get_size(surface)
    chaos_gl.surface_bind(surface)
    chaos_gl.surface_clear(surface, 0x00202030)

    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local titlebar_bg = theme and theme.titlebar_bg or 0x003C3C3C

    -- Title bar
    chaos_gl.rect(0, 0, sw, TITLEBAR_H, titlebar_bg)
    chaos_gl.text(8, 6, "3D Viewer", text_c, 0, 0)
    chaos_gl.rect(sw - 28, 0, 28, TITLEBAR_H, 0x00FF4444)
    chaos_gl.text(sw - 20, 6, "X", 0x00FFFFFF, 0, 0)

    -- Toolbar
    chaos_gl.rect(0, TITLEBAR_H, sw, TOOLBAR_H, 0x00353535)
    local btns = {}
    local bx = 4
    local modes = {
        {label = "Gouraud", mode = "gouraud"},
        {label = "Flat",    mode = "flat"},
        {label = "Wire",    mode = "wireframe"},
    }
    for _, m in ipairs(modes) do
        local bw = chaos_gl.text_width(m.label) + 16
        local bg_c = (render_mode == m.mode) and 0x00666688 or 0x00444444
        chaos_gl.rect_rounded(bx, TITLEBAR_H + 4, bw, 20, 3, bg_c)
        chaos_gl.text(bx + 8, TITLEBAR_H + 6, m.label, text_c, 0, 0)
        btns[#btns + 1] = {x = bx, w = bw, mode = m.mode}
        bx = bx + bw + 4
    end
    -- Status
    chaos_gl.text(bx + 8, TITLEBAR_H + 6, status, sec_c, 0, 0)

    -- 3D viewport
    local vp_y = TITLEBAR_H + TOOLBAR_H
    local vp_h = sh - vp_y
    chaos_gl.push_clip(0, vp_y, sw, vp_h)

    if model then
        -- Set up camera
        local aspect = sw / vp_h
        chaos_gl.set_perspective(45, aspect, 0.1, 100)

        local rad_x = rot_x * 3.14159 / 180
        local rad_y = rot_y * 3.14159 / 180
        local cam_x = distance * math.sin(rad_y) * math.cos(rad_x)
        local cam_y = distance * math.sin(rad_x)
        local cam_z = distance * math.cos(rad_y) * math.cos(rad_x)
        chaos_gl.set_camera(cam_x, cam_y, cam_z, 0, 0, 0, 0, 1, 0)

        -- Model transform (identity)
        chaos_gl.set_transform(0, 0, 0, 0, 0, 0, 1, 1, 1)

        -- Draw
        if render_mode == "wireframe" then
            chaos_gl.draw_wireframe(model, 0x0000FF88)
        elseif render_mode == "flat" then
            chaos_gl.draw_model(model, "flat", {color = 0x006688AA})
        else
            chaos_gl.draw_model(model, "gouraud", {
                light_dir_x = 0.5,
                light_dir_y = -0.7,
                light_dir_z = 0.5,
                ambient = 0.15,
                color = 0x006688CC,
            })
        end

        -- Draw axes at origin
        chaos_gl.set_transform(0, 0, 0, 0, 0, 0, 0.5, 0.5, 0.5)
    else
        local msg = "Open a .cobj model file"
        local mw = chaos_gl.text_width(msg)
        chaos_gl.text((sw - mw) // 2, vp_y + vp_h // 2 - 8, msg, sec_c, 0, 0)
    end

    chaos_gl.pop_clip()
    chaos_gl.surface_present(surface)

    -- Events
    local event = aios.wm.poll_event(surface)
    while event do
        if event.type == EVENT_CLOSE then
            running = false
        elseif event.type == EVENT_KEY_DOWN then
            if event.key == 1 then running = false end
        elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            local mx, my = event.mouse_x, event.mouse_y
            if mx >= sw - 28 and my < TITLEBAR_H then
                running = false
            elseif my >= TITLEBAR_H and my < TITLEBAR_H + TOOLBAR_H then
                for _, b in ipairs(btns) do
                    if mx >= b.x and mx < b.x + b.w then
                        render_mode = b.mode
                        break
                    end
                end
            elseif my >= vp_y then
                dragging = true
                drag_start_x = mx
                drag_start_y = my
                drag_start_rx = rot_x
                drag_start_ry = rot_y
            end
        elseif event.type == EVENT_MOUSE_UP then
            dragging = false
        elseif event.type == EVENT_MOUSE_MOVE and dragging then
            local mx, my = event.mouse_x, event.mouse_y
            local dx = mx - drag_start_x
            local dy = my - drag_start_y
            rot_y = drag_start_ry + dx * 0.5
            rot_x = drag_start_rx + dy * 0.5
            if rot_x > 89 then rot_x = 89 end
            if rot_x < -89 then rot_x = -89 end
        elseif event.type == EVENT_MOUSE_WHEEL then
            distance = distance - (event.wheel or 0) * 0.5
            if distance < 1 then distance = 1 end
            if distance > 20 then distance = 20 end
        end
        event = aios.wm.poll_event(surface)
    end

    aios.os.sleep(32)
end

-- Cleanup
if model then chaos_gl.free_model(model) end
aios.wm.unregister(surface)
chaos_gl.surface_destroy(surface)
