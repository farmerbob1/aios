-- AIOS UI Toolkit — ProgressBar Widget
local core = require("core")

local ProgressBar = setmetatable({}, {__index = core.Widget})
ProgressBar.__index = ProgressBar

function ProgressBar.new(opts)
    local self = setmetatable(core.Widget.new(opts), ProgressBar)
    self.value = opts and opts.value or 0
    self.max = opts and opts.max or 100
    self.color = opts and opts.color or nil
    self.focusable = false
    if self.w == 0 then self.w = 200 end
    if self.h == 0 then self.h = self:get_style("progress_height") or 16 end
    return self
end

function ProgressBar:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    local bg = self:get_style("progress_bg") or 0x00333333
    chaos_gl.rect(x, y, w, h, bg)

    if self.value > 0 and self.max > 0 then
        local fill_w = math.floor(w * math.min(self.value, self.max) / self.max)
        local fill_c = self.color or self:get_style("progress_fill") or self:get_style("accent") or 0x00FF8800
        if fill_w > 0 then
            chaos_gl.rect(x, y, fill_w, h, fill_c)
        end
    end
end

return ProgressBar
