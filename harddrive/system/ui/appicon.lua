-- AIOS UI Toolkit — AppIcon Widget
local core = require("core")

local AppIcon = setmetatable({}, {__index = core.Widget})
AppIcon.__index = AppIcon

function AppIcon.new(name, icon, on_launch, opts)
    local self = setmetatable(core.Widget.new(opts), AppIcon)
    self.name = name or ""
    self.icon = icon  -- texture handle or icon name string
    self.on_launch = on_launch
    self.tooltip = opts and opts.tooltip or nil
    self.badge = opts and opts.badge or nil
    self.pinned = opts and opts.pinned or false
    self.active = opts and opts.active or false
    self.mode = opts and opts.mode or "desktop"  -- "desktop" or "taskbar"
    self.hovered = false
    self.focusable = false
    self._last_click_time = 0
    return self
end

function AppIcon:_resolve_icon(size)
    if type(self.icon) == "number" then return self.icon end
    if type(self.icon) == "string" then
        local icons_mod = package.loaded["icons"]
        if icons_mod then return icons_mod.get(self.icon, size) end
    end
    return -1
end

function AppIcon:get_size()
    if self.mode == "taskbar" then
        local ts = self:get_style("app_taskbar_size") or 24
        return ts + 4, ts + 6
    else
        local cs = self:get_style("app_cell_size") or 80
        return cs, cs
    end
end

function AppIcon:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    if self.mode == "taskbar" then
        local ts = self:get_style("app_taskbar_size") or 24
        if self.hovered then
            chaos_gl.rect(x, y, w, h, self:get_style("button_hover") or 0x004A4A4A)
        end

        local tex = self:_resolve_icon(ts)
        if tex >= 0 then
            local ix = x + (w - ts) // 2
            chaos_gl.blit_keyed(ix, y + 1, ts, ts, tex, 0x00FF00FF)
        end

        if self.active then
            local ac = self:get_style("app_active_indicator") or self:get_style("accent") or 0x00FF8800
            chaos_gl.rect(x + 2, y + h - 2, w - 4, 2, ac)
        end
    else
        -- Desktop mode
        if self.hovered then
            chaos_gl.rect(x, y, w, h, 0x20FFFFFF)
        end

        local tex = self:_resolve_icon(48)
        if tex >= 0 then
            local ix = x + (w - 48) // 2
            chaos_gl.blit_keyed(ix, y + 4, 48, 48, tex, 0x00FF00FF)
        end

        -- Name with shadow
        local shadow_c = self:get_style("file_text_shadow") or 0x00000000
        local fg = self:get_style("text_primary") or 0x00FFFFFF
        local tw = chaos_gl.text_width(self.name)
        local tx = x + (w - tw) // 2
        if shadow_c ~= 0 then
            chaos_gl.text(tx + 1, y + 57, self.name, shadow_c, 0, 0)
        end
        chaos_gl.text(tx, y + 56, self.name, fg, 0, 0)
    end

    -- Badge
    if self.badge and self.badge > 0 then
        local badge_bg = self:get_style("badge_bg") or 0x000000FF
        local badge_fg = self:get_style("badge_text") or 0x00FFFFFF
        local badge_str = self.badge > 99 and "99+" or tostring(self.badge)
        local bx = x + w - 14
        local by = y
        chaos_gl.circle(bx + 7, by + 7, 7, badge_bg)
        local btw = chaos_gl.text_width(badge_str)
        chaos_gl.text(bx + 7 - btw // 2, by, badge_str, badge_fg, 0, 0)
    end
end

function AppIcon:on_input(event)
    if event.type == core.EVENT_MOUSE_MOVE then
        self.hovered = self:contains(event.mouse_x, event.mouse_y)
        return false
    elseif event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            if self.mode == "taskbar" then
                if self.on_launch then self.on_launch() end
                return true
            end
            -- Desktop: double-click to launch
            local now = os.clock and os.clock() or 0
            if now - self._last_click_time < 0.4 then
                if self.on_launch then self.on_launch() end
                self._last_click_time = 0
            else
                self._last_click_time = now
            end
            return true
        end
    end
    return false
end

return AppIcon
