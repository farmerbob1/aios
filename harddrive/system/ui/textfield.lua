-- AIOS UI Toolkit — TextField Widget
local core = require("core")

local TextField = setmetatable({}, {__index = core.Widget})
TextField.__index = TextField

function TextField.new(opts)
    local self = setmetatable(core.Widget.new(opts), TextField)
    self.text = opts and opts.text or ""
    self.placeholder = opts and opts.placeholder or ""
    self.on_change = opts and opts.on_change or nil
    self.on_submit = opts and opts.on_submit or nil
    self.max_length = opts and opts.max_length or 256
    self.password = opts and opts.password or false
    self.focusable = true
    self.cursor_pos = #self.text
    self.scroll_offset = 0
    self._blink_counter = 0
    self._cursor_visible = true
    if self.w == 0 then self.w = 200 end
    if self.h == 0 then self.h = self:get_style("field_height") or 28 end
    return self
end

function TextField:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()
    local pad = self:get_style("field_padding") or 6

    local bg = self:get_style("field_bg") or 0x00222222
    chaos_gl.rect(x, y, w, h, bg)

    local bc = self.focused and (self:get_style("field_border_focus") or 0x00FF8800)
                             or (self:get_style("field_border") or 0x00555555)
    chaos_gl.rect_outline(x, y, w, h, bc, 1)

    local inner_w = w - pad * 2
    local display_text = self.text
    if self.password then
        display_text = string.rep("*", #self.text)
    end

    -- Scroll to keep cursor visible
    local cursor_px = chaos_gl.text_width(display_text:sub(1, self.cursor_pos))
    if cursor_px - self.scroll_offset > inner_w then
        self.scroll_offset = cursor_px - inner_w
    elseif cursor_px < self.scroll_offset then
        self.scroll_offset = cursor_px
    end

    chaos_gl.push_clip(x + pad, y, inner_w, h)

    local text_x = x + pad - self.scroll_offset
    local fh = chaos_gl.font_height(-1)
    local text_y = y + (h - fh) // 2

    if #display_text == 0 and not self.focused then
        local pc = self:get_style("text_placeholder") or 0x00666666
        chaos_gl.text(x + pad, text_y, self.placeholder, pc, 0, 0)
    else
        local fg = self:get_style("field_text") or 0x00FFFFFF
        chaos_gl.text(text_x, text_y, display_text, fg, 0, 0)
    end

    -- Cursor blink
    if self.focused then
        self._blink_counter = self._blink_counter + 1
        if self._blink_counter >= 30 then
            self._blink_counter = 0
            self._cursor_visible = not self._cursor_visible
        end
        if self._cursor_visible then
            local cx = text_x + chaos_gl.text_width(display_text:sub(1, self.cursor_pos))
            local cc = self:get_style("field_cursor") or 0x00FFFFFF
            chaos_gl.line(cx, y + 4, cx, y + h - 4, cc)
        end
    end

    chaos_gl.pop_clip()
end

function TextField:on_input(event)
    if event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            core.set_focus(self)
            local pad = self:get_style("field_padding") or 6
            local rel_x = event.mouse_x - self._layout_x - pad + self.scroll_offset
            -- Find cursor position closest to click
            local best = 0
            for i = 0, #self.text do
                local tw = chaos_gl.text_width(self.text:sub(1, i))
                if math.abs(tw - rel_x) < math.abs(chaos_gl.text_width(self.text:sub(1, best)) - rel_x) then
                    best = i
                end
            end
            self.cursor_pos = best
            self._blink_counter = 0
            self._cursor_visible = true
            return true
        end
    elseif event.type == core.EVENT_KEY_CHAR and self.focused then
        if #self.text < self.max_length then
            local c = event.char
            if c and #c == 1 then
                self.text = self.text:sub(1, self.cursor_pos) .. c .. self.text:sub(self.cursor_pos + 1)
                self.cursor_pos = self.cursor_pos + 1
                self._blink_counter = 0
                self._cursor_visible = true
                if self.on_change then self.on_change(self.text) end
            end
        end
        return true
    elseif event.type == core.EVENT_KEY_DOWN and self.focused then
        local key = event.key
        if key == core.KEY_BACKSPACE then
            if self.cursor_pos > 0 then
                self.text = self.text:sub(1, self.cursor_pos - 1) .. self.text:sub(self.cursor_pos + 1)
                self.cursor_pos = self.cursor_pos - 1
                if self.on_change then self.on_change(self.text) end
            end
            return true
        elseif key == core.KEY_DELETE then
            if self.cursor_pos < #self.text then
                self.text = self.text:sub(1, self.cursor_pos) .. self.text:sub(self.cursor_pos + 2)
                if self.on_change then self.on_change(self.text) end
            end
            return true
        elseif key == core.KEY_LEFT then
            if self.cursor_pos > 0 then self.cursor_pos = self.cursor_pos - 1 end
            self._blink_counter = 0
            self._cursor_visible = true
            return true
        elseif key == core.KEY_RIGHT then
            if self.cursor_pos < #self.text then self.cursor_pos = self.cursor_pos + 1 end
            self._blink_counter = 0
            self._cursor_visible = true
            return true
        elseif key == core.KEY_HOME then
            self.cursor_pos = 0
            self._blink_counter = 0
            self._cursor_visible = true
            return true
        elseif key == core.KEY_END then
            self.cursor_pos = #self.text
            self._blink_counter = 0
            self._cursor_visible = true
            return true
        elseif key == core.KEY_ENTER then
            if self.on_submit then self.on_submit(self.text) end
            return true
        end
    end
    return false
end

function TextField:on_focus()
    self.focused = true
    self._blink_counter = 0
    self._cursor_visible = true
end

return TextField
