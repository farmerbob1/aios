-- AIOS UI — AppWindow helper
-- Eliminates boilerplate: surface create, WM register, titlebar, event loop, cleanup.
--
-- Usage:
--   local AppWindow = require("appwindow")
--   local win = AppWindow.new("My App", 400, 300, {x=100, y=80})
--   while win:is_running() do
--       win:begin_frame()
--       -- draw app content here (surface is bound, cleared, titlebar drawn)
--       win:end_frame()
--       for _, event in ipairs(win:poll_events()) do
--           -- handle app-specific events (close/minimize/maximize already handled)
--       end
--   end
--   win:destroy()

local tb = require("titlebar")
local ui = require("core")

local AppWindow = {}
AppWindow.__index = AppWindow

function AppWindow.new(title, w, h, opts)
    local self = setmetatable({}, AppWindow)
    self.title = title
    self.w = w
    self.h = h
    self._running = true

    local x = opts and opts.x or 100
    local y = opts and opts.y or 80

    self.surface = chaos_gl.surface_create(w, h, opts and opts.depth or false)
    chaos_gl.surface_set_position(self.surface, x, y)
    chaos_gl.surface_set_visible(self.surface, true)

    local reg_opts = {
        title = title,
        task_id = aios.task.self().id,
    }
    if opts and opts.icon then reg_opts.icon = opts.icon end
    aios.wm.register(self.surface, reg_opts)

    return self
end

function AppWindow:is_running()
    return self._running
end

function AppWindow:close()
    self._running = false
end

function AppWindow:set_title(title)
    self.title = title
    aios.wm.register(self.surface, {
        title = title,
        task_id = aios.task.self().id,
    })
end

function AppWindow:begin_frame()
    ui.poll_theme()
    chaos_gl.surface_bind(self.surface)
    local bg = theme and theme.window_bg or 0x00282830
    chaos_gl.surface_clear(self.surface, bg)
    local sw, sh = chaos_gl.surface_get_size(self.surface)
    tb.draw(sw, sh, self.title, self.surface)
end

function AppWindow:end_frame()
    chaos_gl.surface_present(self.surface)
end

function AppWindow:get_size()
    return chaos_gl.surface_get_size(self.surface)
end

function AppWindow:poll_events()
    local events = {}
    local event = aios.wm.poll_event(self.surface)
    while event do
        -- Handle universal events
        if event.type == EVENT_CLOSE then
            self._running = false
            return events
        elseif event.type == EVENT_MOUSE_DOWN and event.button == 1 then
            local btn = tb.handle_click(event.mouse_x, event.mouse_y, self.surface)
            if btn == "close" then
                self._running = false
                return events
            elseif btn then
                -- minimize/maximize handled by titlebar, skip
                goto next_event
            end
        elseif event.type == EVENT_KEY_DOWN and event.key == 0x01 then
            self._running = false
            return events
        end

        events[#events + 1] = event

        ::next_event::
        event = aios.wm.poll_event(self.surface)
    end
    return events
end

function AppWindow:destroy()
    aios.wm.unregister(self.surface)
    chaos_gl.surface_destroy(self.surface)
end

-- Theme color helpers
function AppWindow:color(key, fallback)
    return theme and theme[key] or fallback
end

AppWindow.TITLEBAR_H = tb.HEIGHT

return AppWindow
