-- AIOS UI Toolkit — IconButton Widget
local core = require("core")

local IconButton = setmetatable({}, {__index = core.Widget})
IconButton.__index = IconButton

function IconButton.new(icon, on_click, opts)
    local self = setmetatable(core.Widget.new(opts), IconButton)
    self.icon = icon  -- texture handle or icon name string
    self.on_click = on_click
    self.tooltip = opts and opts.tooltip or nil
    self.icon_size = opts and opts.icon_size or 16
    self.enabled = opts and opts.enabled ~= nil and opts.enabled or true
    self.focusable = true
    self.hovered = false
    self.pressed = false
    if self.w == 0 then self.w = self.icon_size + 8 end
    if self.h == 0 then self.h = self.icon_size + 8 end
    return self
end

function IconButton:_resolve_icon()
    if type(self.icon) == "number" then
        return self.icon
    elseif type(self.icon) == "string" then
        local icons_mod = package.loaded["icons"]
        if icons_mod then
            return icons_mod.get(self.icon, self.icon_size)
        end
    end
    return -1
end

function IconButton:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    if self.hovered or self.pressed then
        local bg = self.pressed
            and (self:get_style("button_pressed") or 0x002A2A2A)
            or (self:get_style("button_hover") or 0x004A4A4A)
        local radius = self:get_style("button_radius") or 4
        chaos_gl.rect_rounded(x, y, w, h, radius, bg)
    end

    local tex = self:_resolve_icon()
    if tex >= 0 then
        local ix = x + (w - self.icon_size) // 2
        local iy = y + (h - self.icon_size) // 2
        chaos_gl.blit_keyed(ix, iy, self.icon_size, self.icon_size, tex, 0x00FF00FF)
    end

    if self.focused then
        local fc = self:get_style("focus_outline") or 0x00FF8800
        chaos_gl.rect_outline(x - 1, y - 1, w + 2, h + 2, fc, 2)
    end
end

function IconButton:on_input(event)
    if not self.enabled then return false end
    if event.type == core.EVENT_MOUSE_MOVE then
        self.hovered = self:contains(event.mouse_x, event.mouse_y)
        return self.hovered
    elseif event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            self.pressed = true
            core.set_focus(self)
            return true
        end
    elseif event.type == core.EVENT_MOUSE_UP and event.button == 1 then
        if self.pressed then
            self.pressed = false
            if self:contains(event.mouse_x, event.mouse_y) and self.on_click then
                self.on_click()
            end
            return true
        end
    end
    return false
end

return IconButton
