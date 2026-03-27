-- AIOS UI Toolkit — Menu Widget (dropdown/context)
local core = require("core")

local Menu = setmetatable({}, {__index = core.Widget})
Menu.__index = Menu

function Menu.new(items, opts)
    local self = setmetatable(core.Widget.new(opts), Menu)
    self.items = items or {}
    self.focusable = false
    self.visible = false
    self.hovered_index = 0
    self.surface = nil
    self._item_height = 24
    self._sep_height = 8
    return self
end

function Menu:_calc_size()
    local max_label_w = 0
    local max_shortcut_w = 0
    local total_h = 4  -- top padding
    for _, item in ipairs(self.items) do
        if item.separator then
            total_h = total_h + self._sep_height
        else
            local lw = chaos_gl.text_width(item.label or "")
            if lw > max_label_w then max_label_w = lw end
            if item.shortcut then
                local sw = chaos_gl.text_width(item.shortcut)
                if sw > max_shortcut_w then max_shortcut_w = sw end
            end
            total_h = total_h + self._item_height
        end
    end
    total_h = total_h + 4  -- bottom padding
    local total_w = max_label_w + max_shortcut_w + 40
    return total_w, total_h
end

function Menu:show(x, y)
    local w, h = self:_calc_size()
    self.w = w
    self.h = h
    self.surface = chaos_gl.surface_create(w, h, false)
    chaos_gl.surface_set_position(self.surface, x, y)
    chaos_gl.surface_set_zorder(self.surface, 200)
    chaos_gl.surface_set_visible(self.surface, true)
    self.visible = true
    self._screen_x = x
    self._screen_y = y
end

function Menu:dismiss()
    if self.surface then
        chaos_gl.surface_destroy(self.surface)
        self.surface = nil
    end
    self.visible = false
end

function Menu:draw(x, y)
    if not self.visible or not self.surface then return end
    self._layout_x = x
    self._layout_y = y

    chaos_gl.surface_bind(self.surface)
    local w, h = self:get_size()

    local bg = self:get_style("menu_bg") or 0x003A3A3A
    chaos_gl.surface_clear(self.surface, bg)

    local bc = self:get_style("menu_border") or 0x00555555
    chaos_gl.rect_outline(0, 0, w, h, bc, 1)

    local cy = 4
    for i, item in ipairs(self.items) do
        if item.separator then
            local sc = self:get_style("menu_separator") or 0x00444444
            chaos_gl.rect(4, cy + 3, w - 8, 1, sc)
            cy = cy + self._sep_height
        else
            if i == self.hovered_index then
                local hc = self:get_style("menu_hover") or 0x00FF8800
                chaos_gl.rect(2, cy, w - 4, self._item_height, hc)
            end

            local fg = self:get_style("menu_text") or 0x00FFFFFF
            local fh = chaos_gl.font_height(-1)
            chaos_gl.text(8, cy + (self._item_height - fh) // 2, item.label or "", fg, 0, 0)

            if item.shortcut then
                local sfg = self:get_style("menu_shortcut_text") or 0x00888888
                local sw = chaos_gl.text_width(item.shortcut)
                chaos_gl.text(w - sw - 8, cy + (self._item_height - fh) // 2, item.shortcut, sfg, 0, 0)
            end
            cy = cy + self._item_height
        end
    end

    chaos_gl.surface_present(self.surface)
end

-- Convert screen-space mouse coords to menu-local, update hovered_index
function Menu:_hit_test(mx, my)
    local lx = mx - (self._screen_x or 0)
    local ly = my - (self._screen_y or 0)
    self.hovered_index = 0
    if lx < 0 or lx >= self.w or ly < 0 or ly >= self.h then
        return false  -- outside menu
    end
    local cy = 4
    for i, item in ipairs(self.items) do
        if item.separator then
            cy = cy + self._sep_height
        else
            if ly >= cy and ly < cy + self._item_height then
                self.hovered_index = i
            end
            cy = cy + self._item_height
        end
    end
    return true  -- inside menu
end

function Menu:on_input(event)
    if not self.visible then return false end

    if event.type == core.EVENT_MOUSE_MOVE then
        self:_hit_test(event.mouse_x, event.mouse_y)
        return true
    elseif event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        local inside = self:_hit_test(event.mouse_x, event.mouse_y)
        if not inside then
            self:dismiss()
            return true
        end
        if self.hovered_index > 0 then
            local item = self.items[self.hovered_index]
            if item and item.on_click and not item.separator then
                item.on_click()
            end
            self:dismiss()
            return true
        end
        return true  -- inside menu but on separator/empty space, consume event
    elseif event.type == core.EVENT_KEY_DOWN then
        if event.key == core.KEY_ESCAPE then
            self:dismiss()
            return true
        end
    end
    return false
end

return Menu
