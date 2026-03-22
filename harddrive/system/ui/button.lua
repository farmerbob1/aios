-- AIOS UI Toolkit — Button Widget
local core = require("core")

local Button = setmetatable({}, {__index = core.Widget})
Button.__index = Button

function Button.new(text, on_click, opts)
    local self = setmetatable(core.Widget.new(opts), Button)
    self.text = text or ""
    self.on_click = on_click
    self.enabled = opts and opts.enabled ~= nil and opts.enabled or true
    self.focusable = true
    self.hovered = false
    self.pressed = false
    return self
end

function Button:get_size()
    if self.w > 0 and self.h > 0 then
        return self.w, self.h
    end
    local pad = self:get_style("button_padding_h") or 12
    local tw = chaos_gl.text_width(self.text)
    local bh = self:get_style("button_height") or 32
    return tw + pad * 2, bh
end

function Button:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    local bg
    if not self.enabled then
        bg = self:get_style("button_disabled")
    elseif self.pressed then
        bg = self:get_style("button_pressed")
    elseif self.hovered then
        bg = self:get_style("button_hover")
    else
        bg = self:get_style("button_normal")
    end

    local radius = self:get_style("button_radius") or 4
    chaos_gl.rect_rounded(x, y, w, h, radius, bg)

    local fg
    if not self.enabled then
        fg = self:get_style("button_text_disabled")
    else
        fg = self:get_style("button_text")
    end

    local tw = chaos_gl.text_width(self.text)
    local tx = x + math.floor((w - tw) / 2)
    local fh = chaos_gl.font_height(-1)
    local ty = y + math.floor((h - fh) / 2)
    chaos_gl.text(tx, ty, self.text, fg, 0, 0)

    if self.focused then
        local fc = self:get_style("focus_outline") or 0x00FF8800
        local fw = self:get_style("focus_outline_width") or 2
        chaos_gl.rect_outline(x - fw, y - fw, w + fw * 2, h + fw * 2, fc, fw)
    end
end

function Button:on_input(event)
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
    elseif event.type == core.EVENT_KEY_DOWN then
        if self.focused and (event.key == core.KEY_ENTER) then
            if self.on_click then self.on_click() end
            return true
        end
    end
    return false
end

return Button
