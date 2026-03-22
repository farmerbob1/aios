-- AIOS UI Toolkit — Checkbox Widget
local core = require("core")

local Checkbox = setmetatable({}, {__index = core.Widget})
Checkbox.__index = Checkbox

function Checkbox.new(label, on_change, opts)
    local self = setmetatable(core.Widget.new(opts), Checkbox)
    self.label = label or ""
    self.on_change = on_change
    self.checked = opts and opts.checked or false
    self.focusable = true
    return self
end

function Checkbox:get_size()
    if self.w > 0 and self.h > 0 then
        return self.w, self.h
    end
    local cs = self:get_style("checkbox_size") or 16
    local tw = chaos_gl.text_width(self.label)
    local fh = chaos_gl.font_height(-1)
    return cs + 8 + tw, math.max(cs, fh)
end

function Checkbox:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local cs = self:get_style("checkbox_size") or 16

    if self.checked then
        local bg = self:get_style("checkbox_checked_bg") or self:get_style("accent") or 0x00FF8800
        chaos_gl.rect(x, y, cs, cs, bg)
        local cc = self:get_style("checkbox_check_color") or 0x00FFFFFF
        chaos_gl.line(x + 3, y + cs // 2, x + cs // 2 - 1, y + cs - 4, cc)
        chaos_gl.line(x + cs // 2 - 1, y + cs - 4, x + cs - 3, y + 3, cc)
    else
        local bc = self:get_style("checkbox_border") or 0x00666666
        chaos_gl.rect_outline(x, y, cs, cs, bc, 1)
    end

    local fg = self:get_style("text_primary") or 0x00FFFFFF
    local fh = chaos_gl.font_height(-1)
    chaos_gl.text(x + cs + 8, y + (cs - fh) // 2, self.label, fg, 0, 0)

    if self.focused then
        local fc = self:get_style("focus_outline") or 0x00FF8800
        chaos_gl.rect_outline(x - 2, y - 2, cs + 4, cs + 4, fc, 2)
    end
end

function Checkbox:on_input(event)
    if event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            self.checked = not self.checked
            core.set_focus(self)
            if self.on_change then self.on_change(self.checked) end
            return true
        end
    elseif event.type == core.EVENT_KEY_DOWN then
        if self.focused and (event.key == core.KEY_ENTER) then
            self.checked = not self.checked
            if self.on_change then self.on_change(self.checked) end
            return true
        end
    end
    return false
end

return Checkbox
