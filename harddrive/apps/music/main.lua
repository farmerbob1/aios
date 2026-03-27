-- AIOS v2 — Music Player
local AppWindow = require("appwindow")
local Button = require("button")
local Slider = require("slider")
local Label = require("label")

local win = AppWindow.new("Music Player", 400, 340, {x=180, y=100})

local playlist = {}
local current_idx = 0
local current_id = nil
local volume = 80
local state = "stopped"
local scroll_y = 0

local function scan_music()
    playlist = {}
    local dirs = {"/sounds"}
    for _, dir in ipairs(dirs) do
        local ok, entries = pcall(aios.io.listdir, dir)
        if ok and entries then
            for _, e in ipairs(entries) do
                if e.name and (e.name:match("%.wav$") or e.name:match("%.mp3$") or e.name:match("%.mid$")) then
                    playlist[#playlist + 1] = { path = dir .. "/" .. e.name, name = e.name }
                end
            end
        end
    end
end

scan_music()
aios.audio.volume(volume)

local function play_track(idx)
    if current_id then aios.audio.stop(current_id); current_id = nil end
    if idx >= 1 and idx <= #playlist then
        current_idx = idx
        current_id = aios.audio.play(playlist[idx].path)
        state = current_id and "playing" or "stopped"
    end
end

-- Transport button callbacks
local function do_prev()
    local ni = current_idx - 1; if ni < 1 then ni = #playlist end
    if ni > 0 then play_track(ni) end
end
local function do_toggle()
    if state == "playing" then aios.audio.pause(current_id); state = "paused"
    elseif state == "paused" then aios.audio.resume(current_id); state = "playing"
    elseif current_idx > 0 then play_track(current_idx)
    elseif #playlist > 0 then play_track(1) end
end
local function do_next()
    local ni = current_idx + 1; if ni > #playlist then ni = 1 end
    if ni > 0 then play_track(ni) end
end
local function do_stop()
    if current_id then aios.audio.stop(current_id) end
    current_id = nil; state = "stopped"
end

-- Widgets
local btn_prev   = Button.new("|<<", do_prev, {w=50, h=26})
local btn_toggle = Button.new(">",   do_toggle, {w=50, h=26})
local btn_next   = Button.new(">>|", do_next, {w=50, h=26})
local btn_stop   = Button.new("Stop", do_stop, {w=50, h=26})
local vol_slider = Slider.new(0, 100, volume, function(v)
    volume = math.floor(v); aios.audio.volume(volume)
end, {w=200, h=16})
local transport_widgets = {btn_prev, btn_toggle, btn_next, btn_stop}

while win:is_running() do
    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800
    local bg = theme and theme.window_bg or 0x002D2D2D

    -- Update toggle button text
    btn_toggle.text = (state == "playing") and "||" or ">"

    win:begin_frame()

    -- Now playing
    local np_y = 32
    local np_bg = theme and theme.field_bg or 0x00222233
    chaos_gl.rect(8, np_y, 384, 44, np_bg)
    if current_idx > 0 and playlist[current_idx] then
        chaos_gl.text(16, np_y + 6, playlist[current_idx].name, accent, 0, 0)
        local status_color = state == "playing" and 0x0044FF44 or sec_c
        chaos_gl.text(16, np_y + 24, state, status_color, 0, 0)
    else
        chaos_gl.text(16, np_y + 14, "No track selected", sec_c, 0, 0)
    end

    -- Transport buttons (widgets)
    local ctrl_y = np_y + 52
    local bx = 80
    for _, btn in ipairs(transport_widgets) do
        btn:draw(bx, ctrl_y)
        local bw = btn:get_size()
        bx = bx + bw + 6
    end

    -- Volume slider (widget)
    local vol_y = ctrl_y + 34
    chaos_gl.text(16, vol_y + 2, "Vol:", sec_c, 0, 0)
    vol_slider:draw(60, vol_y)

    -- Playlist header
    local list_y = vol_y + 26
    local sep_c = theme and theme.border or 0x00444444
    chaos_gl.rect(8, list_y, 384, 1, sep_c)
    list_y = list_y + 4
    chaos_gl.text(8, list_y, "Playlist (" .. #playlist .. " tracks)", sec_c, 0, 0)
    list_y = list_y + 18

    -- Playlist items
    local item_h = 22
    local visible = math.floor((340 - list_y) / item_h)
    local start_i = scroll_y + 1
    for i = start_i, math.min(#playlist, start_i + visible - 1) do
        local iy = list_y + (i - start_i) * item_h
        if iy + item_h > 340 then break end
        local item_bg = (i == current_idx) and (theme and theme.list_selection or 0x00444455) or bg
        chaos_gl.rect(8, iy, 384, item_h - 2, item_bg)
        local name_c = (i == current_idx) and accent or text_c
        chaos_gl.text(16, iy + 3, playlist[i].name, name_c, 0, 0)
    end

    win:end_frame()

    -- Events
    for _, event in ipairs(win:poll_events()) do
        local handled = false
        for _, btn in ipairs(transport_widgets) do
            if btn:on_input(event) then handled = true; break end
        end
        if not handled and vol_slider:on_input(event) then handled = true end

        if not handled then
            if event.type == EVENT_MOUSE_DOWN and event.button == 1 then
                local my = event.mouse_y
                if my >= list_y then
                    local pi = math.floor((my - list_y) / item_h) + scroll_y + 1
                    if pi >= 1 and pi <= #playlist then play_track(pi) end
                end
            elseif event.type == EVENT_MOUSE_WHEEL then
                scroll_y = math.max(0, scroll_y + (event.wheel or 0) * -1)
                scroll_y = math.min(scroll_y, math.max(0, #playlist - 5))
            end
        end
    end

    if current_id and state == "playing" then
        local st = aios.audio.status(current_id)
        if st == "stopped" or st == nil then state = "stopped"; current_id = nil end
    end

    aios.os.sleep(33)
end

if current_id then aios.audio.stop(current_id) end
win:destroy()
