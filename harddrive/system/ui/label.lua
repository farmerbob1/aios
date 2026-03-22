-- AIOS UI Toolkit — Label Widget
local core = require("core")

local Label = setmetatable({}, {__index = core.Widget})
Label.__index = Label

function Label.new(text, opts)
    local self = setmetatable(core.Widget.new(opts), Label)
    self.text = text or ""
    self.color = opts and opts.color or nil
    self.wrap = opts and opts.wrap or false
    self.align = opts and opts.align or "left"
    self.max_width = opts and opts.max_width or 0
    self.focusable = false
    return self
end

function Label:get_size()
    if self.w > 0 and self.h > 0 then
        return self.w, self.h
    end
    if self.wrap and self.max_width > 0 then
        local tw = chaos_gl.text_width(self.text)
        if tw > self.max_width then
            local h = chaos_gl.text_height_wrapped(self.max_width, self.text)
            return self.max_width, h
        end
    end
    local tw = chaos_gl.text_width(self.text)
    local fh = chaos_gl.font_height(-1)
    return tw > 0 and tw or 8, fh
end

function Label:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local fg = self.color or self:get_style("text_primary") or 0x00FFFFFF
    local w, h = self:get_size()

    if self.wrap and self.max_width > 0 then
        chaos_gl.text_wrapped(x, y, self.max_width, self.text, fg, 0, 0)
        return
    end

    local tx = x
    if self.align == "center" then
        local tw = chaos_gl.text_width(self.text)
        tx = x + math.floor((w - tw) / 2)
    elseif self.align == "right" then
        local tw = chaos_gl.text_width(self.text)
        tx = x + w - tw
    end
    chaos_gl.text(tx, y, self.text, fg, 0, 0)
end

return Label
