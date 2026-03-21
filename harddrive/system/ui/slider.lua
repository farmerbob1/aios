-- AIOS UI Toolkit — Slider Widget
local core = require("core")

local Slider = setmetatable({}, {__index = core.Widget})
Slider.__index = Slider

function Slider.new(min_val, max_val, value, on_change, opts)
    local self = setmetatable(core.Widget.new(opts), Slider)
    self.min = min_val or 0
    self.max = max_val or 100
    self.value = value or self.min
    self.on_change = on_change
    self.step = opts and opts.step or 1
    self.show_value = opts and opts.show_value ~= nil and opts.show_value or true
    self.focusable = true
    self.dragging = false
    if self.w == 0 then self.w = 200 end
    if self.h == 0 then self.h = 20 end
    return self
end

function Slider:get_size()
    local w, h = self.w, self.h
    if self.show_value then
        w = w + 40
    end
    return w, h
end

function Slider:_track_rect()
    local th = self:get_style("slider_track_height") or 4
    local tr = self:get_style("slider_thumb_radius") or 8
    local tw = self.w - tr * 2
    local tx = self._layout_x + tr
    local ty = self._layout_y + self.h // 2 - th // 2
    return tx, ty, tw, th
end

function Slider:_thumb_x()
    local tx, _, tw, _ = self:_track_rect()
    local pct = 0
    if self.max > self.min then
        pct = (self.value - self.min) / (self.max - self.min)
    end
    return tx + math.floor(pct * tw)
end

function Slider:_snap(val)
    if self.step > 0 then
        val = math.floor((val - self.min) / self.step + 0.5) * self.step + self.min
    end
    return math.max(self.min, math.min(self.max, val))
end

function Slider:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y

    local track_x, track_y, track_w, track_h = self:_track_rect()
    local track_c = self:get_style("slider_track") or 0x00444444
    chaos_gl.rect(track_x, track_y, track_w, track_h, track_c)

    local tr = self:get_style("slider_thumb_radius") or 8
    local tc = self:get_style("slider_thumb_color") or self:get_style("accent") or 0x00FF8800
    local thumb_cx = self:_thumb_x()
    local thumb_cy = y + self.h // 2
    chaos_gl.circle(thumb_cx, thumb_cy, tr, tc)

    if self.show_value then
        local fg = self:get_style("text_primary") or 0x00FFFFFF
        chaos_gl.text(x + self.w + 4, y + (self.h - 16) // 2, tostring(math.floor(self.value)), fg, 0, 0)
    end

    if self.focused then
        local fc = self:get_style("focus_outline") or 0x00FF8800
        chaos_gl.rect_outline(x - 2, y - 2, self.w + 4, self.h + 4, fc, 2)
    end
end

function Slider:on_input(event)
    if event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            self.dragging = true
            core.set_focus(self)
            self:_update_from_mouse(event.mouse_x)
            return true
        end
    elseif event.type == core.EVENT_MOUSE_MOVE then
        if self.dragging then
            self:_update_from_mouse(event.mouse_x)
            return true
        end
    elseif event.type == core.EVENT_MOUSE_UP and event.button == 1 then
        if self.dragging then
            self.dragging = false
            return true
        end
    elseif event.type == core.EVENT_KEY_DOWN and self.focused then
        if event.key == core.KEY_LEFT then
            self.value = self:_snap(self.value - self.step)
            if self.on_change then self.on_change(self.value) end
            return true
        elseif event.key == core.KEY_RIGHT then
            self.value = self:_snap(self.value + self.step)
            if self.on_change then self.on_change(self.value) end
            return true
        end
    end
    return false
end

function Slider:_update_from_mouse(mx)
    local tx, _, tw, _ = self:_track_rect()
    if tw <= 0 then return end
    local pct = (mx - tx) / tw
    pct = math.max(0, math.min(1, pct))
    local new_val = self:_snap(self.min + pct * (self.max - self.min))
    if new_val ~= self.value then
        self.value = new_val
        if self.on_change then self.on_change(self.value) end
    end
end

return Slider
