-- AIOS UI Toolkit — Panel Widget (container)
local core = require("core")

local Panel = setmetatable({}, {__index = core.Widget})
Panel.__index = Panel

function Panel.new(opts)
    local self = setmetatable(core.Widget.new(opts), Panel)
    self.children = opts and opts.children or {}
    self.bg_color = opts and opts.bg_color or nil
    self.border = opts and opts.border or false
    self.border_color = opts and opts.border_color or nil
    self.focusable = false
    for _, child in ipairs(self.children) do
        child.parent = self
    end
    return self
end

function Panel:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    if self.bg_color then
        chaos_gl.rect(x, y, w, h, self.bg_color)
    end
    if self.border then
        local bc = self.border_color or self:get_style("window_border") or 0x00555555
        chaos_gl.rect_outline(x, y, w, h, bc, 1)
    end

    for _, child in ipairs(self.children) do
        local cx, cy = child._layout_x, child._layout_y
        if cx == 0 and cy == 0 then cx, cy = x, y end
        child:draw(cx, cy)
    end
end

function Panel:on_input(event)
    for i = #self.children, 1, -1 do
        if self.children[i]:on_input(event) then return true end
    end
    return false
end

function Panel:collect_focusable(chain)
    for _, child in ipairs(self.children) do
        child:collect_focusable(chain)
    end
end

return Panel
