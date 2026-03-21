-- AIOS UI Toolkit — TextArea Widget (multi-line text input)
local core = require("core")

local TextArea = setmetatable({}, {__index = core.Widget})
TextArea.__index = TextArea

function TextArea.new(opts)
    local self = setmetatable(core.Widget.new(opts), TextArea)
    self.text = opts and opts.text or ""
    self.on_change = opts and opts.on_change or nil
    self.max_lines = opts and opts.max_lines or 100
    self.focusable = true
    self.cursor_pos = #self.text
    self.scroll_y = 0
    self._blink_counter = 0
    self._cursor_visible = true
    if self.w == 0 then self.w = 300 end
    if self.h == 0 then self.h = 120 end
    return self
end

function TextArea:_lines()
    local lines = {}
    for line in (self.text .. "\n"):gmatch("([^\n]*)\n") do
        lines[#lines + 1] = line
    end
    if #lines == 0 then lines[1] = "" end
    return lines
end

function TextArea:_cursor_line_col()
    local pos = 0
    local lines = self:_lines()
    for i, line in ipairs(lines) do
        if self.cursor_pos <= pos + #line then
            return i, self.cursor_pos - pos
        end
        pos = pos + #line + 1
    end
    return #lines, #lines[#lines]
end

function TextArea:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()
    local pad = 4

    local bg = self:get_style("field_bg") or 0x00222222
    chaos_gl.rect(x, y, w, h, bg)
    local bc = self.focused and (self:get_style("field_border_focus") or 0x00FF8800)
                             or (self:get_style("field_border") or 0x00555555)
    chaos_gl.rect_outline(x, y, w, h, bc, 1)

    chaos_gl.push_clip(x + pad, y + pad, w - pad * 2, h - pad * 2)

    local lines = self:_lines()
    local visible_lines = (h - pad * 2) // 16
    local start_line = math.floor(self.scroll_y / 16) + 1
    local fg = self:get_style("field_text") or 0x00FFFFFF

    for i = start_line, math.min(#lines, start_line + visible_lines) do
        local ly = y + pad + (i - start_line) * 16
        chaos_gl.text(x + pad, ly, lines[i], fg, 0, 0)
    end

    if self.focused then
        self._blink_counter = self._blink_counter + 1
        if self._blink_counter >= 30 then
            self._blink_counter = 0
            self._cursor_visible = not self._cursor_visible
        end
        if self._cursor_visible then
            local cl, cc = self:_cursor_line_col()
            local cx = x + pad + cc * 8
            local cy = y + pad + (cl - 1) * 16 - self.scroll_y
            local cursor_c = self:get_style("field_cursor") or 0x00FFFFFF
            chaos_gl.line(cx, cy, cx, cy + 15, cursor_c)
        end
    end

    chaos_gl.pop_clip()
end

function TextArea:on_input(event)
    if event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            core.set_focus(self)
            return true
        end
    elseif event.type == core.EVENT_KEY_CHAR and self.focused then
        local c = event.char
        if c and #c == 1 then
            self.text = self.text:sub(1, self.cursor_pos) .. c .. self.text:sub(self.cursor_pos + 1)
            self.cursor_pos = self.cursor_pos + 1
            if self.on_change then self.on_change(self.text) end
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
        elseif key == core.KEY_ENTER then
            self.text = self.text:sub(1, self.cursor_pos) .. "\n" .. self.text:sub(self.cursor_pos + 1)
            self.cursor_pos = self.cursor_pos + 1
            if self.on_change then self.on_change(self.text) end
            return true
        elseif key == core.KEY_LEFT then
            if self.cursor_pos > 0 then self.cursor_pos = self.cursor_pos - 1 end
            return true
        elseif key == core.KEY_RIGHT then
            if self.cursor_pos < #self.text then self.cursor_pos = self.cursor_pos + 1 end
            return true
        end
    elseif event.type == core.EVENT_MOUSE_WHEEL then
        if self:contains(event.mouse_x, event.mouse_y) then
            self.scroll_y = math.max(0, self.scroll_y - event.wheel * 48)
            return true
        end
    end
    return false
end

function TextArea:on_focus()
    self.focused = true
    self._blink_counter = 0
    self._cursor_visible = true
end

return TextArea
