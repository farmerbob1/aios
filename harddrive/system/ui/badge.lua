-- AIOS UI Toolkit — Badge Widget (extra, standalone notification badge)
local core = require("core")

local Badge = setmetatable({}, {__index = core.Widget})
Badge.__index = Badge

function Badge.new(count, opts)
    local self = setmetatable(core.Widget.new(opts), Badge)
    self.count = count or 0
    self.focusable = false
    self.w = 14
    self.h = 14
    return self
end

function Badge:draw(x, y)
    if not self.visible or self.count <= 0 then return end
    self._layout_x = x
    self._layout_y = y

    local bg = self:get_style("badge_bg") or 0x000000FF
    chaos_gl.circle(x + 7, y + 7, 7, bg)

    local fg = self:get_style("badge_text") or 0x00FFFFFF
    local text = self.count > 99 and "99+" or tostring(self.count)
    local tw = chaos_gl.text_width(text)
    chaos_gl.text(x + 7 - tw // 2, y, text, fg, 0, 0)
end

return Badge
