-- AIOS UI Toolkit — TabView Widget
local core = require("core")

local TabView = setmetatable({}, {__index = core.Widget})
TabView.__index = TabView

function TabView.new(tabs, opts)
    local self = setmetatable(core.Widget.new(opts), TabView)
    self.tabs = tabs or {}
    self.active_tab = 1
    self.focusable = true
    self.children = {}
    for _, tab in ipairs(self.tabs) do
        if tab.content then
            self.children[#self.children + 1] = tab.content
            tab.content.parent = self
        end
    end
    if self.w == 0 then self.w = 400 end
    if self.h == 0 then self.h = 300 end
    return self
end

function TabView:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()
    local tab_h = self:get_style("tab_height") or 32

    -- Tab bar background
    local tab_bg = self:get_style("tab_bg") or 0x00333333
    chaos_gl.rect(x, y, w, tab_h, tab_bg)

    -- Tab labels
    local tx = x
    for i, tab in ipairs(self.tabs) do
        local tw = chaos_gl.text_width(tab.label) + 24
        local is_active = (i == self.active_tab)

        if is_active then
            local abg = self:get_style("tab_active_bg") or 0x002D2D2D
            chaos_gl.rect(tx, y, tw, tab_h, abg)
            local ind_c = self:get_style("tab_indicator") or 0x00FF8800
            local ind_h = self:get_style("tab_indicator_height") or 2
            chaos_gl.rect(tx, y + tab_h - ind_h, tw, ind_h, ind_c)
        end

        local fg = is_active
            and (self:get_style("tab_text_active") or 0x00FFFFFF)
            or (self:get_style("tab_text") or 0x00AAAAAA)
        chaos_gl.text(tx + 12, y + (tab_h - 16) // 2, tab.label, fg, 0, 0)

        tab._x = tx
        tab._w = tw
        tx = tx + tw
    end

    -- Active content
    local content_y = y + tab_h
    local content_h = h - tab_h
    if self.active_tab >= 1 and self.active_tab <= #self.tabs then
        local content = self.tabs[self.active_tab].content
        if content then
            chaos_gl.push_clip(x, content_y, w, content_h)
            content:draw(x, content_y)
            chaos_gl.pop_clip()
        end
    end
end

function TabView:on_input(event)
    if event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        local tab_h = self:get_style("tab_height") or 32
        if event.mouse_y >= self._layout_y and event.mouse_y < self._layout_y + tab_h then
            for i, tab in ipairs(self.tabs) do
                if tab._x and event.mouse_x >= tab._x and event.mouse_x < tab._x + tab._w then
                    self.active_tab = i
                    core.set_focus(self)
                    return true
                end
            end
        end
    elseif event.type == core.EVENT_KEY_DOWN and self.focused then
        if event.key == core.KEY_LEFT then
            if self.active_tab > 1 then self.active_tab = self.active_tab - 1 end
            return true
        elseif event.key == core.KEY_RIGHT then
            if self.active_tab < #self.tabs then self.active_tab = self.active_tab + 1 end
            return true
        end
    end

    -- Forward to active content
    if self.active_tab >= 1 and self.active_tab <= #self.tabs then
        local content = self.tabs[self.active_tab].content
        if content then return content:on_input(event) end
    end
    return false
end

function TabView:collect_focusable(chain)
    if self.focusable and self.visible then
        chain[#chain + 1] = self
    end
    if self.active_tab >= 1 and self.active_tab <= #self.tabs then
        local content = self.tabs[self.active_tab].content
        if content and content.collect_focusable then
            content:collect_focusable(chain)
        end
    end
end

return TabView
