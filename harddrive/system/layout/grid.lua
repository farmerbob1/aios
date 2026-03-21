-- AIOS UI Toolkit — Grid Layout Engine
local core = require("core")

local grid = {}

local GridContainer = setmetatable({}, {__index = core.Widget})
GridContainer.__index = GridContainer

function GridContainer.new(children, opts)
    local self = setmetatable(core.Widget.new(opts), GridContainer)
    self.children = children or {}
    self.cols = opts and opts.cols or 3
    self.row_height = opts and opts.row_height or 32
    self.col_width = opts and opts.col_width or "equal"
    self.spacing = opts and opts.spacing or { x = 8, y = 8 }
    self.padding = opts and opts.padding or 8
    self.focusable = false
    self._layout_dirty = true
    if type(self.spacing) == "number" then
        self.spacing = { x = self.spacing, y = self.spacing }
    end
    for _, child in ipairs(self.children) do
        child.parent = self
    end
    return self
end

function GridContainer:get_size()
    if self.w > 0 and self.h > 0 and not self._layout_dirty then
        return self.w, self.h
    end
    local rows = math.ceil(#self.children / self.cols)
    local pad = self.padding
    local h = pad * 2 + rows * self.row_height + math.max(0, rows - 1) * self.spacing.y
    local w = self.w > 0 and self.w or 400
    return w, h
end

function GridContainer:_get_col_width(container_w)
    local pad = self.padding
    local available = container_w - pad * 2 - math.max(0, self.cols - 1) * self.spacing.x
    if type(self.col_width) == "table" then
        return self.col_width
    end
    -- "equal" distribution
    local cw = math.floor(available / self.cols)
    local widths = {}
    for i = 1, self.cols do widths[i] = cw end
    return widths
end

function GridContainer:reflow(x, y, container_w, container_h)
    local pad = self.padding
    local col_widths = self:_get_col_width(container_w)

    for i, child in ipairs(self.children) do
        local col = (i - 1) % self.cols
        local row = math.floor((i - 1) / self.cols)

        local cx = x + pad
        for c = 1, col do
            cx = cx + col_widths[c] + self.spacing.x
        end
        local cy = y + pad + row * (self.row_height + self.spacing.y)

        child._layout_x = cx
        child._layout_y = cy
        child.w = col_widths[col + 1] or col_widths[1]
        child.h = self.row_height
    end
end

function GridContainer:draw(x, y)
    if not self.visible then return end
    self._layout_x = x
    self._layout_y = y
    local w, h = self:get_size()

    if self._layout_dirty then
        self:reflow(x, y, w, h)
        self._layout_dirty = false
    end

    for _, child in ipairs(self.children) do
        child:draw(child._layout_x, child._layout_y)
    end
end

function GridContainer:on_input(event)
    for i = #self.children, 1, -1 do
        if self.children[i]:on_input(event) then return true end
    end
    return false
end

function GridContainer:collect_focusable(chain)
    for _, child in ipairs(self.children) do
        child:collect_focusable(chain)
    end
end

function grid.new(children, opts)
    return GridContainer.new(children, opts)
end

grid.GridContainer = GridContainer

return grid
