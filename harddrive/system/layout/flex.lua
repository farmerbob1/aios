-- AIOS UI Toolkit — Flex Layout Engine
local core = require("core")

local flex = {}

-- ── Flex Container ──────────────────────────────────

local FlexContainer = setmetatable({}, {__index = core.Widget})
FlexContainer.__index = FlexContainer

local function parse_padding(pad)
    if type(pad) == "table" then
        return pad[1] or 0, pad[2] or 0, pad[3] or 0, pad[4] or 0
    end
    pad = pad or 0
    return pad, pad, pad, pad
end

function FlexContainer.new(direction, children, opts)
    local self = setmetatable(core.Widget.new(opts), FlexContainer)
    self.direction = direction  -- "row" or "col"
    self.children = children or {}
    self.spacing = opts and opts.spacing or 0
    self.padding = opts and opts.padding or 0
    self.align = opts and opts.align or "start"
    self.justify = opts and opts.justify or "start"
    self.focusable = false
    self._layout_dirty = true
    for _, child in ipairs(self.children) do
        child.parent = self
    end
    return self
end

function FlexContainer:get_size()
    if self.w > 0 and self.h > 0 and not self._layout_dirty then
        return self.w, self.h
    end
    local pt, pr, pb, pl = parse_padding(self.padding)
    local total_main = 0
    local max_cross = 0
    local n = #self.children
    for _, child in ipairs(self.children) do
        local cw, ch = child:get_size()
        if self.direction == "row" then
            total_main = total_main + cw
            if ch > max_cross then max_cross = ch end
        else
            total_main = total_main + ch
            if cw > max_cross then max_cross = cw end
        end
    end
    total_main = total_main + math.max(0, n - 1) * self.spacing

    if self.direction == "row" then
        local w = self.w > 0 and self.w or (total_main + pl + pr)
        local h = self.h > 0 and self.h or (max_cross + pt + pb)
        return w, h
    else
        local w = self.w > 0 and self.w or (max_cross + pl + pr)
        local h = self.h > 0 and self.h or (total_main + pt + pb)
        return w, h
    end
end

function FlexContainer:reflow(x, y, container_w, container_h)
    local pt, pr, pb, pl = parse_padding(self.padding)
    local n = #self.children

    -- Measure children and identify flex items
    local sizes = {}
    local total_fixed = 0
    local total_flex_weight = 0
    for i, child in ipairs(self.children) do
        local cw, ch = child:get_size()
        sizes[i] = { w = cw, h = ch }
        local f = child.flex or 0
        if f > 0 then
            total_flex_weight = total_flex_weight + f
        else
            if self.direction == "row" then
                total_fixed = total_fixed + cw
            else
                total_fixed = total_fixed + ch
            end
        end
    end

    local spacing_total = math.max(0, n - 1) * self.spacing

    -- Available space for flex items
    local available
    if self.direction == "row" then
        available = container_w - pl - pr - spacing_total - total_fixed
    else
        available = container_h - pt - pb - spacing_total - total_fixed
    end
    if available < 0 then available = 0 end

    -- Assign flex sizes
    if total_flex_weight > 0 then
        for i, child in ipairs(self.children) do
            local f = child.flex or 0
            if f > 0 then
                local flex_size = math.floor(available * f / total_flex_weight)
                if self.direction == "row" then
                    sizes[i].w = flex_size
                else
                    sizes[i].h = flex_size
                end
            end
        end
    end

    -- Compute total content size for justify
    local total_content = 0
    for i = 1, n do
        if self.direction == "row" then
            total_content = total_content + sizes[i].w
        else
            total_content = total_content + sizes[i].h
        end
    end
    total_content = total_content + spacing_total

    local main_space
    if self.direction == "row" then
        main_space = container_w - pl - pr
    else
        main_space = container_h - pt - pb
    end

    local start_offset = 0
    local extra_spacing = 0
    if self.justify == "center" then
        start_offset = math.max(0, (main_space - total_content) // 2)
    elseif self.justify == "end" then
        start_offset = math.max(0, main_space - total_content)
    elseif self.justify == "space_between" then
        if n > 1 then
            local remaining = main_space - total_content + spacing_total
            extra_spacing = remaining / (n - 1)
        end
    end

    -- Position children
    local cursor
    if self.direction == "row" then
        cursor = x + pl + start_offset
    else
        cursor = y + pt + start_offset
    end

    for i, child in ipairs(self.children) do
        local cw, ch = sizes[i].w, sizes[i].h

        local child_x, child_y
        if self.direction == "row" then
            child_x = cursor
            -- Cross axis (vertical)
            if self.align == "center" then
                child_y = y + pt + (container_h - pt - pb - ch) // 2
            elseif self.align == "end" then
                child_y = y + container_h - pb - ch
            elseif self.align == "stretch" then
                child_y = y + pt
                ch = container_h - pt - pb
            else  -- "start"
                child_y = y + pt
            end
            cursor = cursor + cw + (self.justify == "space_between" and extra_spacing or self.spacing)
        else
            child_y = cursor
            -- Cross axis (horizontal)
            if self.align == "center" then
                child_x = x + pl + (container_w - pl - pr - cw) // 2
            elseif self.align == "end" then
                child_x = x + container_w - pr - cw
            elseif self.align == "stretch" then
                child_x = x + pl
                cw = container_w - pl - pr
            else  -- "start"
                child_x = x + pl
            end
            cursor = cursor + ch + (self.justify == "space_between" and extra_spacing or self.spacing)
        end

        child._layout_x = child_x
        child._layout_y = child_y
        -- Update child size if stretched or flex
        if (child.flex and child.flex > 0) or self.align == "stretch" then
            child.w = cw
            child.h = ch
        end
    end
end

function FlexContainer:draw(x, y)
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

function FlexContainer:on_input(event)
    for i = #self.children, 1, -1 do
        if self.children[i]:on_input(event) then return true end
    end
    return false
end

function FlexContainer:collect_focusable(chain)
    for _, child in ipairs(self.children) do
        child:collect_focusable(chain)
    end
end

-- ── Public API ──────────────────────────────────────

function flex.row(children, opts)
    return FlexContainer.new("row", children, opts)
end

function flex.col(children, opts)
    return FlexContainer.new("col", children, opts)
end

flex.FlexContainer = FlexContainer

return flex
