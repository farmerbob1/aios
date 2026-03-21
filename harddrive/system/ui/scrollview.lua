-- AIOS UI Toolkit — ScrollView Widget
local core = require("core")

local ScrollView = setmetatable({}, {__index = core.Widget})
ScrollView.__index = ScrollView

function ScrollView.new(child, opts)
    local self = setmetatable(core.Widget.new(opts), ScrollView)
    self.child = child
    self.children = child and {child} or {}
    if child then child.parent = self end
    self.scroll_x = opts and opts.scroll_x or 0
    self.scroll_y = opts and opts.scroll_y or 0
    self.show_scrollbar = opts and opts.show_scrollbar or "auto"
    self.focusable = false
    self._scrollbar_hovered = false
    self._scrollbar_dragging = false
    self._drag_start_y = 0
    self._drag_start_scroll = 0
    if self.w == 0 then self.w = 300 end
    if self.h == 0 then self.h = 200 end
    return self
end

function ScrollView:_content_size()
    if self.child then
        return self.child:get_size()
    end
    return 0, 0
end

function ScrollView:_max_scroll_y()
    local _, ch = self:_content_size()
    return math.max(0, ch - self.h)
end

function ScrollView:_scrollbar_visible()
    if self.show_scrollbar == "never" then return false end
    if self.show_scrollbar == "always" then return true end
    local _, ch = self:_content_size()
    return ch > self.h
end

function ScrollView:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    chaos_gl.push_clip(x, y, w, h)

    if self.child then
        self.child:draw(x - self.scroll_x, y - self.scroll_y)
    end

    chaos_gl.pop_clip()

    -- Scrollbar
    if self:_scrollbar_visible() then
        local sbw = self:get_style("scrollbar_width") or 10
        local track_c = self:get_style("scrollbar_track") or 0x00333333
        chaos_gl.rect(x + w - sbw, y, sbw, h, track_c)

        local _, ch = self:_content_size()
        if ch > 0 then
            local thumb_h = math.max(20, math.floor(h * h / ch))
            local max_sy = self:_max_scroll_y()
            local thumb_y = 0
            if max_sy > 0 then
                thumb_y = math.floor(self.scroll_y / max_sy * (h - thumb_h))
            end
            local thumb_c = self._scrollbar_hovered
                and (self:get_style("scrollbar_thumb_hover") or 0x00777777)
                or (self:get_style("scrollbar_thumb") or 0x00555555)
            chaos_gl.rect(x + w - sbw, y + thumb_y, sbw, thumb_h, thumb_c)
        end
    end
end

function ScrollView:on_input(event)
    if event.type == core.EVENT_MOUSE_WHEEL then
        if self:contains(event.mouse_x, event.mouse_y) then
            local speed = self:get_style("scroll_speed") or 48
            self.scroll_y = math.max(0, math.min(self:_max_scroll_y(),
                self.scroll_y - event.wheel * speed))
            return true
        end
    elseif event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        -- Check scrollbar area
        if self:_scrollbar_visible() then
            local sbw = self:get_style("scrollbar_width") or 10
            local w, _ = self:get_size()
            if event.mouse_x >= self._layout_x + w - sbw and
               event.mouse_x < self._layout_x + w and
               event.mouse_y >= self._layout_y and
               event.mouse_y < self._layout_y + self.h then
                self._scrollbar_dragging = true
                self._drag_start_y = event.mouse_y
                self._drag_start_scroll = self.scroll_y
                return true
            end
        end
    elseif event.type == core.EVENT_MOUSE_MOVE then
        if self._scrollbar_dragging then
            local dy = event.mouse_y - self._drag_start_y
            local _, ch = self:_content_size()
            local ratio = ch / self.h
            self.scroll_y = math.max(0, math.min(self:_max_scroll_y(),
                self._drag_start_scroll + dy * ratio))
            return true
        end
        -- Hover detection for scrollbar
        if self:_scrollbar_visible() then
            local sbw = self:get_style("scrollbar_width") or 10
            local w, _ = self:get_size()
            self._scrollbar_hovered = event.mouse_x >= self._layout_x + w - sbw and
                event.mouse_x < self._layout_x + w and
                event.mouse_y >= self._layout_y and
                event.mouse_y < self._layout_y + self.h
        end
    elseif event.type == core.EVENT_MOUSE_UP then
        if self._scrollbar_dragging then
            self._scrollbar_dragging = false
            return true
        end
    end

    -- Forward to child with adjusted coordinates
    if self.child then
        return self.child:on_input(event)
    end
    return false
end

function ScrollView:collect_focusable(chain)
    if self.child then
        self.child:collect_focusable(chain)
    end
end

return ScrollView
