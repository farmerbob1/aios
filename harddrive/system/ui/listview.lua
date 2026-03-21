-- AIOS UI Toolkit — ListView Widget (virtualised scrolling list)
local core = require("core")

local ListView = setmetatable({}, {__index = core.Widget})
ListView.__index = ListView

function ListView.new(opts)
    local self = setmetatable(core.Widget.new(opts), ListView)
    self.items = opts and opts.items or {}
    self.item_height = opts and opts.item_height or (self:get_style("list_item_height") or 24)
    self.render_item = opts and opts.render_item or nil
    self.on_select = opts and opts.on_select or nil
    self.multi_select = opts and opts.multi_select or false
    self.selected = {}
    self.scroll_y = 0
    self.focusable = false
    if self.w == 0 then self.w = 300 end
    if self.h == 0 then self.h = 200 end
    return self
end

function ListView:_max_scroll_y()
    local total = #self.items * self.item_height
    return math.max(0, total - self.h)
end

function ListView:_visible_range()
    local start = math.floor(self.scroll_y / self.item_height) + 1
    local count = math.ceil(self.h / self.item_height) + 1
    local finish = math.min(#self.items, start + count)
    return start, finish
end

function ListView:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    chaos_gl.push_clip(x, y, w, h)

    local vis_start, vis_end = self:_visible_range()
    for i = vis_start, vis_end do
        local iy = y + (i - 1) * self.item_height - self.scroll_y
        local is_selected = self.selected[i]

        -- Alternating stripes
        local stripe = self:get_style("list_stripe") or 0
        if stripe ~= 0 and i % 2 == 0 then
            chaos_gl.rect(x, iy, w, self.item_height, stripe)
        end

        if is_selected then
            local sel_c = self:get_style("list_selection") or 0x00FF8800
            chaos_gl.rect(x, iy, w, self.item_height, sel_c)
        end

        if self.render_item then
            self.render_item(self.items[i], i, is_selected, x, iy, w, self.item_height)
        else
            local fg = self:get_style("text_primary") or 0x00FFFFFF
            local text = tostring(self.items[i])
            chaos_gl.text(x + 4, iy + (self.item_height - 16) // 2, text, fg, 0, 0)
        end
    end

    chaos_gl.pop_clip()

    -- Scrollbar
    local total = #self.items * self.item_height
    if total > h then
        local sbw = self:get_style("scrollbar_width") or 10
        local track_c = self:get_style("scrollbar_track") or 0x00333333
        chaos_gl.rect(x + w - sbw, y, sbw, h, track_c)
        local thumb_h = math.max(20, math.floor(h * h / total))
        local max_sy = self:_max_scroll_y()
        local thumb_y = max_sy > 0 and math.floor(self.scroll_y / max_sy * (h - thumb_h)) or 0
        local thumb_c = self:get_style("scrollbar_thumb") or 0x00555555
        chaos_gl.rect(x + w - sbw, y + thumb_y, sbw, thumb_h, thumb_c)
    end
end

function ListView:on_input(event)
    if event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            local rel_y = event.mouse_y - self._layout_y + self.scroll_y
            local idx = math.floor(rel_y / self.item_height) + 1
            if idx >= 1 and idx <= #self.items then
                if not self.multi_select then
                    self.selected = {}
                end
                self.selected[idx] = not self.selected[idx]
                if self.on_select then
                    self.on_select(self.items[idx], idx)
                end
            end
            return true
        end
    elseif event.type == core.EVENT_MOUSE_WHEEL then
        if self:contains(event.mouse_x, event.mouse_y) then
            local speed = self:get_style("scroll_speed") or 48
            self.scroll_y = math.max(0, math.min(self:_max_scroll_y(),
                self.scroll_y - event.wheel * speed))
            return true
        end
    end
    return false
end

return ListView
