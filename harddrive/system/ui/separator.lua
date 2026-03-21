-- AIOS UI Toolkit — Separator Widget (extra)
local core = require("core")

local Separator = setmetatable({}, {__index = core.Widget})
Separator.__index = Separator

function Separator.new(opts)
    local self = setmetatable(core.Widget.new(opts), Separator)
    self.direction = opts and opts.direction or "horizontal"
    self.color = opts and opts.color or nil
    self.focusable = false
    if self.direction == "horizontal" then
        if self.w == 0 then self.w = 100 end
        if self.h == 0 then self.h = 1 end
    else
        if self.w == 0 then self.w = 1 end
        if self.h == 0 then self.h = 100 end
    end
    return self
end

function Separator:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local c = self.color or self:get_style("separator_color") or 0x00444444
    local w, h = self:get_size()
    chaos_gl.rect(x, y, w, h, c)
end

return Separator
