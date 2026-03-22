-- AIOS v2 — Live System Monitor

local AppWindow = require("appwindow")
local win = AppWindow.new("System Monitor", 340, 380, {x=340, y=150})

local fh = chaos_gl.font_height(-1)
local TITLEBAR_H = AppWindow.TITLEBAR_H

-- History buffers (last 60 samples, ~1 per second)
local MAX_HISTORY = 60
local fps_history = {}
local cpu_history = {}
local render_history = {}
local mem_history = {}
for i = 1, MAX_HISTORY do
    fps_history[i] = 0
    cpu_history[i] = 0
    render_history[i] = 0
    mem_history[i] = 0
end
local history_idx = 0
local last_sample = aios.os.millis()
local frame_count = 0
local current_fps = 0
local current_cpu = 0

while win:is_running() do
    frame_count = frame_count + 1
    local sw, sh = win:get_size()

    -- Sample once per second
    local now = aios.os.millis()
    if now - last_sample >= 1000 then
        current_fps = math.floor(frame_count * 1000 / (now - last_sample))
        frame_count = 0
        last_sample = now

        history_idx = (history_idx % MAX_HISTORY) + 1
        fps_history[history_idx] = current_fps
        current_cpu = aios.task.cpu_usage()
        cpu_history[history_idx] = current_cpu
        local cs = chaos_gl.get_compose_stats()
        render_history[history_idx] = cs and (cs.compose_time_us or 0) or 0
        local info = aios.os.meminfo()
        if info and info.pmm_total_pages and info.pmm_total_pages > 0 then
            mem_history[history_idx] = math.floor((1.0 - info.pmm_free_pages / info.pmm_total_pages) * 100)
        end
    end

    win:begin_frame()

    local text_c = win:color("text_primary", 0x00E8E8F0)
    local sec_c = win:color("text_secondary", 0x009999AA)

    local y = TITLEBAR_H + 8
    local graph_w = sw - 24
    local graph_h = 36
    local bar_w = math.max(1, graph_w // MAX_HISTORY)

    local function draw_graph(label, value_str, history, max_val, color, gy)
        chaos_gl.text(12, gy, label, text_c, 0, 0)
        local vw = chaos_gl.text_width(value_str)
        chaos_gl.text(sw - 12 - vw, gy, value_str, color, 0, 0)
        gy = gy + fh + 2
        chaos_gl.rect(12, gy, graph_w, graph_h, 0x00222233)
        for i = 1, MAX_HISTORY do
            local idx = ((history_idx + i - 1) % MAX_HISTORY) + 1
            local pct = math.min(1.0, history[idx] / max_val)
            local bh = math.max(0, math.floor(graph_h * pct))
            if bh > 0 then
                chaos_gl.rect(12 + (i - 1) * bar_w, gy + graph_h - bh, bar_w, bh, color)
            end
        end
        chaos_gl.rect_outline(12, gy, graph_w, graph_h, 0x00444455, 1)
        return gy + graph_h + 6
    end

    y = draw_graph("FPS", tostring(current_fps), fps_history, 62, 0x0000CC00, y)
    y = draw_graph("CPU", current_cpu .. "%", cpu_history, 100, 0x00EE4444, y)

    local cs = chaos_gl.get_compose_stats()
    local render_us = cs and (cs.compose_time_us or 0) or 0
    y = draw_graph("Render", string.format("%.1fms", render_us / 1000), render_history, 16000, 0x00FF8800, y)

    local info = aios.os.meminfo()
    local mem_str = "N/A"
    if info and info.pmm_total_pages and info.pmm_total_pages > 0 then
        local mem_pct = math.floor((1.0 - info.pmm_free_pages / info.pmm_total_pages) * 100)
        local used_kb = (info.pmm_total_pages - info.pmm_free_pages) * 4
        local total_kb = info.pmm_total_pages * 4
        mem_str = string.format("%dMB / %dMB (%d%%)", used_kb // 1024, total_kb // 1024, mem_pct)
    end
    y = draw_graph("Memory", mem_str, mem_history, 100, 0x004488FF, y)

    if info then
        chaos_gl.text(12, y, string.format("Heap: %dKB used, %dKB free",
            (info.heap_used or 0) // 1024, (info.heap_free or 0) // 1024), sec_c, 0, 0)
    end

    win:end_frame()
    win:poll_events()  -- close/minimize/maximize/ESC handled automatically
    aios.os.sleep(32)
end

win:destroy()
