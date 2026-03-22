-- AIOS UI Toolkit — FileItem Widget
local core = require("core")
local filetypes = require("filetypes")

local FileItem = setmetatable({}, {__index = core.Widget})
FileItem.__index = FileItem

function FileItem.type_from_path(path)
    return filetypes.get_type(path:match("[^/]+$") or path)
end

function FileItem.new(name, file_type, opts)
    local self = setmetatable(core.Widget.new(opts), FileItem)
    self.name = name or ""
    self.file_type = file_type or "file"
    self.icon = opts and opts.icon or nil
    self.on_click = opts and opts.on_click or nil
    self.on_double_click = opts and opts.on_double_click or nil
    self.selected = opts and opts.selected or false
    self.view = opts and opts.view or "grid"
    self.hovered = false
    self.focusable = false
    self._last_click_time = 0
    self.metadata = opts and opts.metadata or ""
    return self
end

function FileItem:_get_icon(size)
    local icons_mod = package.loaded["icons"]
    if not icons_mod then return -1 end
    if type(self.icon) == "number" then return self.icon end
    if type(self.icon) == "string" then return icons_mod.get(self.icon, size) end
    return icons_mod.get(self.file_type, size)
end

function FileItem:get_size()
    if self.view == "grid" then
        local cs = self:get_style("file_grid_cell") or 72
        return cs, cs
    else
        local ih = self:get_style("list_item_height") or 24
        return self.w > 0 and self.w or 300, ih
    end
end

function FileItem:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    -- Selection/hover highlight
    if self.selected then
        local sc = self:get_style("file_selection") or 0x00FF8800
        chaos_gl.rect(x, y, w, h, sc)
    elseif self.hovered then
        local hc = self:get_style("file_hover") or 0x003A3A3A
        chaos_gl.rect(x, y, w, h, hc)
    end

    if self.view == "grid" then
        -- Grid: icon centered, name below
        local tex = self:_get_icon(32)
        if tex >= 0 then
            local ix = x + (w - 32) // 2
            local iy = y + 4
            chaos_gl.blit_keyed(ix, iy, 32, 32, tex, 0x00FF00FF)
        end

        local fg = self:get_style("text_primary") or 0x00FFFFFF
        local name_display = self.name
        local max_chars = (w - 4) // 8
        if #name_display > max_chars then
            name_display = name_display:sub(1, max_chars - 3) .. "..."
        end
        local tw = chaos_gl.text_width(name_display)
        local tx = x + (w - tw) // 2
        chaos_gl.text(tx, y + 40, name_display, fg, 0, 0)
    else
        -- List: 16x16 icon + name + metadata
        local tex = self:_get_icon(16)
        if tex >= 0 then
            local iy = y + (h - 16) // 2
            chaos_gl.blit_keyed(x + 4, iy, 16, 16, tex, 0x00FF00FF)
        end

        local fg = self:get_style("text_primary") or 0x00FFFFFF
        chaos_gl.text(x + 28, y + (h - 16) // 2, self.name, fg, 0, 0)

        if self.metadata and #self.metadata > 0 then
            local sfg = self:get_style("text_secondary") or 0x00AAAAAA
            local mw = chaos_gl.text_width(self.metadata)
            chaos_gl.text(x + w - mw - 8, y + (h - 16) // 2, self.metadata, sfg, 0, 0)
        end
    end
end

function FileItem:on_input(event)
    if event.type == core.EVENT_MOUSE_MOVE then
        self.hovered = self:contains(event.mouse_x, event.mouse_y)
        return false
    elseif event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self:contains(event.mouse_x, event.mouse_y) then
            -- Double-click detection (frame counter approximation)
            local now = os.clock and os.clock() or 0
            if now - self._last_click_time < 0.4 then
                if self.on_double_click then self.on_double_click(self) end
                self._last_click_time = 0
            else
                self.selected = true
                if self.on_click then self.on_click(self) end
                self._last_click_time = now
            end
            return true
        end
    end
    return false
end

return FileItem
