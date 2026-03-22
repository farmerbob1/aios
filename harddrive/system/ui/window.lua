-- AIOS UI Toolkit — Window Widget
local core = require("core")

local Window = setmetatable({}, {__index = core.Widget})
Window.__index = Window

function Window.new(title, opts)
    local self = setmetatable(core.Widget.new(opts), Window)
    self.title = title or "Window"
    self.content = opts and opts.content or nil
    self.closable = opts and opts.closable ~= nil and opts.closable or true
    self.resizable = opts and opts.resizable ~= nil and opts.resizable or true
    self.on_close = opts and opts.on_close or nil
    self.on_resize = opts and opts.on_resize or nil
    self.min_w = opts and opts.min_w or 200
    self.min_h = opts and opts.min_h or 150
    self.surface = opts and opts.surface or nil
    self.focusable = false
    self.active = true
    self.maximized = false
    self._dragging = false
    self._resizing = false
    self._drag_start_x = 0
    self._drag_start_y = 0
    self._drag_base_w = 0
    self._drag_base_h = 0
    self.children = self.content and {self.content} or {}
    if self.content then self.content.parent = self end
    if self.w == 0 then self.w = 400 end
    if self.h == 0 then self.h = 300 end
    return self
end

function Window:_titlebar_height()
    return self:get_style("titlebar_height") or 28
end

-- Traffic light button constants
local BTN_RADIUS = 6
local BTN_Y_CENTER = 14  -- vertical center in titlebar
local BTN_X_START = 12   -- left margin
local BTN_SPACING = 20   -- center-to-center

function Window:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()
    local th = self:_titlebar_height()

    -- Window background with rounded corners
    local bg = self:get_style("window_bg") or 0x002D2D2D
    chaos_gl.rect_rounded(x, y, w, h, 4, bg)

    -- Border
    local bc = self.active
        and (self:get_style("window_border_active") or 0x00555565)
        or (self:get_style("window_border") or 0x00444450)
    chaos_gl.rect_rounded_outline(x, y, w, h, 4, bc, 1)

    -- Titlebar
    local tbg = self.active
        and (self:get_style("titlebar_bg") or 0x003C3C3C)
        or (self:get_style("titlebar_bg_inactive") or 0x00333333)
    chaos_gl.rect_rounded(x, y, w, th, 4, tbg)
    -- Square off bottom corners of titlebar
    chaos_gl.rect(x, y + th - 4, w, 4, tbg)

    -- Separator line under titlebar
    chaos_gl.rect(x + 1, y + th - 1, w - 2, 1, 0x00222228)

    -- Traffic light buttons (left side)
    local btn_cy = y + BTN_Y_CENTER
    if self.active then
        -- Close (red)
        chaos_gl.circle(x + BTN_X_START, btn_cy, BTN_RADIUS, 0x004444FF)
        -- Minimize (yellow)
        chaos_gl.circle(x + BTN_X_START + BTN_SPACING, btn_cy, BTN_RADIUS, 0x0000BBFF)
        -- Maximize (green)
        chaos_gl.circle(x + BTN_X_START + BTN_SPACING * 2, btn_cy, BTN_RADIUS, 0x0000CC44)
    else
        -- Inactive: all grey
        local gc = 0x00555555
        chaos_gl.circle(x + BTN_X_START, btn_cy, BTN_RADIUS, gc)
        chaos_gl.circle(x + BTN_X_START + BTN_SPACING, btn_cy, BTN_RADIUS, gc)
        chaos_gl.circle(x + BTN_X_START + BTN_SPACING * 2, btn_cy, BTN_RADIUS, gc)
    end

    -- Title (centered)
    local tfg = self:get_style("titlebar_text") or 0x00FFFFFF
    local fh = chaos_gl.font_height(-1)
    local tw = chaos_gl.text_width(self.title)
    local title_x = x + (w - tw) // 2
    chaos_gl.text(title_x, y + (th - fh) // 2, self.title, tfg, 0, 0)

    -- Resize handle
    if self.resizable and not self.maximized then
        local rx = x + w - 12
        local ry = y + h - 12
        local gc = self:get_style("text_secondary") or 0x00AAAAAA
        chaos_gl.line(rx + 2, ry + 10, rx + 10, ry + 2, gc)
        chaos_gl.line(rx + 5, ry + 10, rx + 10, ry + 5, gc)
        chaos_gl.line(rx + 8, ry + 10, rx + 10, ry + 8, gc)
    end

    -- Content
    if self.content then
        local cy = y + th
        local ch = h - th
        chaos_gl.push_clip(x + 1, cy, w - 2, ch - 1)
        self.content:draw(x + 1, cy)
        chaos_gl.pop_clip()
    end
end

function Window:_hit_button(mx, my)
    local btn_cy = self._layout_y + BTN_Y_CENTER
    local bx_close = self._layout_x + BTN_X_START
    local bx_min = self._layout_x + BTN_X_START + BTN_SPACING
    local bx_max = self._layout_x + BTN_X_START + BTN_SPACING * 2

    if (mx - bx_close)^2 + (my - btn_cy)^2 <= (BTN_RADIUS + 2)^2 then return "close" end
    if (mx - bx_min)^2 + (my - btn_cy)^2 <= (BTN_RADIUS + 2)^2 then return "minimize" end
    if (mx - bx_max)^2 + (my - btn_cy)^2 <= (BTN_RADIUS + 2)^2 then return "maximize" end
    return nil
end

function Window:on_input(event)
    local w, h = self:get_size()
    local th = self:_titlebar_height()

    if event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        local mx, my = event.mouse_x, event.mouse_y

        -- Traffic light buttons
        local btn = self:_hit_button(mx, my)
        if btn == "close" then
            if self.on_close then self.on_close() end
            return true
        elseif btn == "minimize" then
            if self.surface then aios.wm.minimize(self.surface) end
            return true
        elseif btn == "maximize" then
            if self.surface then
                if self.maximized then
                    aios.wm.restore_size(self.surface)
                    self.maximized = false
                else
                    aios.wm.maximize(self.surface)
                    self.maximized = true
                end
            end
            return true
        end

        -- Resize handle (not when maximized)
        if self.resizable and not self.maximized and
           mx >= self._layout_x + w - 12 and my >= self._layout_y + h - 12 then
            self._resizing = true
            self._drag_start_x = mx
            self._drag_start_y = my
            self._drag_base_w = w
            self._drag_base_h = h
            return true
        end

        -- Titlebar drag (not when maximized)
        if not self.maximized and
           my >= self._layout_y and my < self._layout_y + th and
           mx >= self._layout_x and mx < self._layout_x + w then
            self._dragging = true
            self._drag_start_x = mx
            self._drag_start_y = my
            return true
        end

        -- Forward to content
        if self.content and my >= self._layout_y + th then
            return self.content:on_input(event)
        end
    elseif event.type == core.EVENT_MOUSE_MOVE then
        if self._dragging and self.surface then
            local dx = event.mouse_x - self._drag_start_x
            local dy = event.mouse_y - self._drag_start_y
            local sx, sy = chaos_gl.surface_get_position(self.surface)
            chaos_gl.surface_set_position(self.surface, sx + dx, sy + dy)
            return true
        end
        if self._resizing then
            local dx = event.mouse_x - self._drag_start_x
            local dy = event.mouse_y - self._drag_start_y
            local new_w = math.max(self.min_w, self._drag_base_w + dx)
            local new_h = math.max(self.min_h, self._drag_base_h + dy)
            self.w = new_w
            self.h = new_h
            if self.surface then
                chaos_gl.surface_resize(self.surface, new_w, new_h)
            end
            if self.on_resize then self.on_resize(new_w, new_h) end
            self:invalidate()
            return true
        end
        if self.content then
            return self.content:on_input(event)
        end
    elseif event.type == core.EVENT_MOUSE_UP then
        if self._dragging then
            self._dragging = false
            return true
        end
        if self._resizing then
            self._resizing = false
            return true
        end
        if self.content then
            return self.content:on_input(event)
        end
    else
        if self.content then
            return self.content:on_input(event)
        end
    end
    return false
end

function Window:collect_focusable(chain)
    if self.content then
        self.content:collect_focusable(chain)
    end
end

return Window
