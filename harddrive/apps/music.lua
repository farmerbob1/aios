-- @app name="Music" icon="/system/icons/music_32.png"
-- AIOS v2 — Music Player
local AppWindow = require("appwindow")
local win = AppWindow.new("Music Player", 400, 340, {x=180, y=100})

local playlist = {}
local current_idx = 0
local current_id = nil
local volume = 80
local state = "stopped"

-- Scan for audio files
local function scan_music()
    playlist = {}
    local dirs = {"/sounds"}
    for _, dir in ipairs(dirs) do
        local ok, entries = pcall(aios.io.listdir, dir)
        if ok and entries then
            for _, e in ipairs(entries) do
                if e.name and (e.name:match("%.wav$") or e.name:match("%.mp3$") or e.name:match("%.mid$")) then
                    playlist[#playlist + 1] = {
                        path = dir .. "/" .. e.name,
                        name = e.name,
                    }
                end
            end
        end
    end
end

scan_music()
aios.audio.volume(volume)

local function play_track(idx)
    if current_id then
        aios.audio.stop(current_id)
        current_id = nil
    end
    if idx >= 1 and idx <= #playlist then
        current_idx = idx
        current_id = aios.audio.play(playlist[idx].path)
        state = current_id and "playing" or "stopped"
    end
end

local scroll_y = 0

while win:is_running() do
    -- Draw
    local bg = win:color("window_bg", 0x002D2D2D)
    local text_c = win:color("text_primary", 0x00FFFFFF)
    local sec_c = win:color("text_secondary", 0x00AAAAAA)
    local accent = win:color("accent", 0x00FF8800)

    win:begin_frame()

    -- Now playing
    local np_y = 32
    chaos_gl.rect(8, np_y, 384, 44, 0x00222233)
    if current_idx > 0 and playlist[current_idx] then
        chaos_gl.text(16, np_y + 6, playlist[current_idx].name, accent, 0, 0)
        local status_color = state == "playing" and 0x0044FF44 or sec_c
        chaos_gl.text(16, np_y + 24, state, status_color, 0, 0)
    else
        chaos_gl.text(16, np_y + 14, "No track selected", sec_c, 0, 0)
    end

    -- Transport controls
    local ctrl_y = np_y + 52
    local btn_w = 50
    local btn_gap = 6
    local btn_start = 80
    local buttons = {
        {label = "|<<",  action = "prev"},
        {label = state == "playing" and "||" or ">", action = "toggle"},
        {label = ">>|",  action = "next"},
        {label = "Stop", action = "stop"},
    }
    for i, b in ipairs(buttons) do
        local bx = btn_start + (i - 1) * (btn_w + btn_gap)
        b.x = bx
        chaos_gl.rect(bx, ctrl_y, btn_w, 26, 0x00444455)
        local tw = chaos_gl.text_width(b.label)
        chaos_gl.text(bx + (btn_w - tw) // 2, ctrl_y + 5, b.label, text_c, 0, 0)
    end

    -- Volume
    local vol_y = ctrl_y + 34
    chaos_gl.text(16, vol_y + 2, "Vol:", sec_c, 0, 0)
    chaos_gl.rect(60, vol_y + 4, 200, 8, 0x00333344)
    chaos_gl.rect(60, vol_y + 4, math.floor(200 * volume / 100), 8, accent)
    chaos_gl.text(268, vol_y + 2, tostring(volume) .. "%", sec_c, 0, 0)

    -- Playlist header
    local list_y = vol_y + 22
    chaos_gl.rect(8, list_y, 384, 1, 0x00444444)
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
        local item_bg = (i == current_idx) and 0x00444455 or bg
        chaos_gl.rect(8, iy, 384, item_h - 2, item_bg)
        local name_c = (i == current_idx) and accent or text_c
        chaos_gl.text(16, iy + 3, playlist[i].name, name_c, 0, 0)
    end

    win:end_frame()

    -- Events
    for _, event in ipairs(win:poll_events()) do
        if event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            local mx, my = event.mouse_x, event.mouse_y
            -- Transport buttons
            if my >= ctrl_y and my < ctrl_y + 26 then
                for _, b in ipairs(buttons) do
                    if mx >= b.x and mx < b.x + btn_w then
                        if b.action == "toggle" then
                            if state == "playing" then
                                aios.audio.pause(current_id)
                                state = "paused"
                            elseif state == "paused" then
                                aios.audio.resume(current_id)
                                state = "playing"
                            elseif current_idx > 0 then
                                play_track(current_idx)
                            elseif #playlist > 0 then
                                play_track(1)
                            end
                        elseif b.action == "stop" then
                            if current_id then aios.audio.stop(current_id) end
                            current_id = nil
                            state = "stopped"
                        elseif b.action == "next" then
                            local ni = current_idx + 1
                            if ni > #playlist then ni = 1 end
                            play_track(ni)
                        elseif b.action == "prev" then
                            local ni = current_idx - 1
                            if ni < 1 then ni = #playlist end
                            play_track(ni)
                        end
                    end
                end
            -- Volume slider
            elseif my >= vol_y and my < vol_y + 16 and mx >= 60 and mx < 260 then
                volume = math.floor((mx - 60) * 100 / 200)
                if volume < 0 then volume = 0 end
                if volume > 100 then volume = 100 end
                aios.audio.volume(volume)
            -- Playlist click
            elseif my >= list_y then
                local pi = math.floor((my - list_y) / item_h) + scroll_y + 1
                if pi >= 1 and pi <= #playlist then
                    play_track(pi)
                end
            end
        elseif event.type == EVENT_MOUSE_WHEEL then
            scroll_y = math.max(0, scroll_y + (event.wheel or 0) * -1)
            if scroll_y > math.max(0, #playlist - 5) then
                scroll_y = math.max(0, #playlist - 5)
            end
        end
    end

    -- Check if track finished (just stop, don't auto-advance)
    if current_id and state == "playing" then
        local st = aios.audio.status(current_id)
        if st == "stopped" or st == nil then
            state = "stopped"
            current_id = nil
        end
    end

    aios.os.sleep(33)
end

-- Cleanup
if current_id then aios.audio.stop(current_id) end
win:destroy()
