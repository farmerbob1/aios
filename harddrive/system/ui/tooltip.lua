-- AIOS UI Toolkit — Tooltip Widget (extra)
local core = require("core")

local Tooltip = setmetatable({}, {__index = core.Widget})
Tooltip.__index = Tooltip

function Tooltip.new(text, opts)
    local self = setmetatable(core.Widget.new(opts), Tooltip)
    self.text = text or ""
    self.focusable = false
    self.visible = false
    self.surface = nil
    self._hover_frames = 0
    self._delay_frames = opts and opts.delay_frames or 45  -- ~750ms at 60fps
    return self
end

function Tooltip:show(x, y)
    local pad = 4
    local tw = chaos_gl.text_width(self.text)
    local w = tw + pad * 2
    local h = 16 + pad * 2
    self.w = w
    self.h = h

    -- Keep on screen
    if x + w > 1024 then x = 1024 - w end
    if y + h > 768 then y = y - h - 4 end

    self.surface = chaos_gl.surface_create(w, h, false)
    chaos_gl.surface_set_position(self.surface, x, y)
    chaos_gl.surface_set_zorder(self.surface, 250)
    chaos_gl.surface_set_visible(self.surface, true)
    self.visible = true
end

function Tooltip:hide()
    if self.surface then
        chaos_gl.surface_destroy(self.surface)
        self.surface = nil
    end
    self.visible = false
    self._hover_frames = 0
end

function Tooltip:draw()
    if not self.visible or not self.surface then return end
    chaos_gl.surface_bind(self.surface)
    local bg = self:get_style("tooltip_bg") or 0x00444444
    chaos_gl.surface_clear(self.surface, bg)
    local bc = self:get_style("tooltip_border") or 0x00666666
    chaos_gl.rect_outline(0, 0, self.w, self.h, bc, 1)
    local fg = self:get_style("tooltip_text") or 0x00FFFFFF
    chaos_gl.text(4, 4, self.text, fg, 0, 0)
    chaos_gl.surface_present(self.surface)
end

function Tooltip:tick(is_hovering, mouse_x, mouse_y)
    if is_hovering then
        self._hover_frames = self._hover_frames + 1
        if not self.visible and self._hover_frames >= self._delay_frames then
            self:show(mouse_x + 8, mouse_y + 16)
        end
    else
        if self.visible then self:hide() end
        self._hover_frames = 0
    end
end

return Tooltip
