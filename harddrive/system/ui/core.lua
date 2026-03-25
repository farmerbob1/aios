-- AIOS UI Toolkit — Core (Phase 8)
-- Widget base class, focus management, event dispatch, theme loading

local ui = {}

-- Global theme table
theme = {}

-- Event type constants — alias the kernel globals (set by lua_state.c)
-- so toolkit widgets can use core.EVENT_* or the bare globals interchangeably
ui.EVENT_KEY_DOWN    = EVENT_KEY_DOWN
ui.EVENT_KEY_UP      = EVENT_KEY_UP
ui.EVENT_MOUSE_MOVE  = EVENT_MOUSE_MOVE
ui.EVENT_MOUSE_DOWN  = EVENT_MOUSE_DOWN
ui.EVENT_MOUSE_UP    = EVENT_MOUSE_UP
ui.EVENT_MOUSE_WHEEL = EVENT_MOUSE_WHEEL
ui.EVENT_KEY_CHAR    = EVENT_KEY_CHAR

-- Key constants
ui.KEY_TAB       = 15
ui.KEY_ENTER     = 28
ui.KEY_ESCAPE    = 1
ui.KEY_BACKSPACE = 14
ui.KEY_DELETE    = 211
ui.KEY_LEFT      = 203
ui.KEY_RIGHT     = 205
ui.KEY_UP        = 200
ui.KEY_DOWN      = 208
ui.KEY_HOME      = 199
ui.KEY_END       = 207

-- ── Widget Base Class ──────────────────────────────

local Widget = {}
Widget.__index = Widget

function Widget.new(opts)
    local self = setmetatable({}, Widget)
    self.w = opts and opts.w or 0
    self.h = opts and opts.h or 0
    self.flex = opts and opts.flex or nil
    self.style = opts and opts.style or nil
    self.focused = false
    self.focusable = false
    self.visible = true
    self.children = nil
    self._layout_dirty = true
    self._layout_x = 0
    self._layout_y = 0
    self.parent = nil
    return self
end

function Widget:get_style(key)
    if self.style and self.style[key] ~= nil then
        return self.style[key]
    end
    return theme[key]
end

function Widget:draw(x, y)
    -- Override in subclasses
end

function Widget:on_input(event)
    -- Override in subclasses, return true if consumed
    return false
end

function Widget:on_focus()
    self.focused = true
end

function Widget:on_blur()
    self.focused = false
end

function Widget:get_size()
    return self.w, self.h
end

function Widget:set_size(w, h)
    self.w = w
    self.h = h
    self:invalidate()
end

function Widget:contains(px, py)
    return px >= self._layout_x and px < self._layout_x + self.w
       and py >= self._layout_y and py < self._layout_y + self.h
end

function Widget:invalidate()
    self._layout_dirty = true
    if self.parent then self.parent:invalidate() end
end

function Widget:collect_focusable(chain)
    if self.focusable and self.visible then
        chain[#chain + 1] = self
    end
    if self.children then
        for _, child in ipairs(self.children) do
            child:collect_focusable(chain)
        end
    end
end

ui.Widget = Widget

-- ── Focus Management ───────────────────────────────

local focus_chain = {}
local focus_index = 0

function ui.build_focus_chain(root)
    focus_chain = {}
    focus_index = 0
    root:collect_focusable(focus_chain)
    if #focus_chain > 0 then
        focus_index = 1
        focus_chain[1]:on_focus()
    end
end

function ui.focus_next()
    if #focus_chain == 0 then return end
    if focus_index > 0 and focus_index <= #focus_chain then
        focus_chain[focus_index]:on_blur()
    end
    focus_index = (focus_index % #focus_chain) + 1
    focus_chain[focus_index]:on_focus()
end

function ui.focus_prev()
    if #focus_chain == 0 then return end
    if focus_index > 0 and focus_index <= #focus_chain then
        focus_chain[focus_index]:on_blur()
    end
    focus_index = ((focus_index - 2) % #focus_chain) + 1
    focus_chain[focus_index]:on_focus()
end

function ui.set_focus(widget)
    for i, w in ipairs(focus_chain) do
        if w == widget then
            if focus_index > 0 and focus_index <= #focus_chain then
                focus_chain[focus_index]:on_blur()
            end
            focus_index = i
            widget:on_focus()
            return
        end
    end
end

function ui.get_focused()
    if focus_index > 0 and focus_index <= #focus_chain then
        return focus_chain[focus_index]
    end
    return nil
end

function ui.get_focus_chain()
    return focus_chain
end

-- ── Event Dispatch ─────────────────────────────────

function ui.dispatch_event(root, event)
    if not event then return end

    -- Keyboard events go to focused widget
    if event.type >= ui.EVENT_KEY_DOWN then
        if event.type == ui.EVENT_KEY_DOWN and event.key == ui.KEY_TAB then
            if event.shift then
                ui.focus_prev()
            else
                ui.focus_next()
            end
            return true
        end
        local focused = ui.get_focused()
        if focused then
            return focused:on_input(event)
        end
        return false
    end

    -- Mouse events: dispatch through tree
    return root:on_input(event)
end

-- ── Theme Loading ──────────────────────────────────

local prefs = require("lib/prefs")
local _theme_path = ""

function ui.load_theme(path)
    local fn, err = loadfile(path)
    if fn then
        theme = fn()
        _theme_path = path
    else
        print("[ui] Failed to load theme: " .. tostring(err))
    end
end

-- Save theme preference via prefs system (cross-task broadcast)
function ui.set_theme(path)
    ui.load_theme(path)
    prefs.put("theme", path)
end

-- Check if prefs changed (theme, etc.) — call from event loops
-- Returns true if theme actually changed
function ui.poll_theme()
    if prefs.poll() then
        local new_theme = prefs.get("theme")
        if new_theme and new_theme ~= _theme_path then
            ui.load_theme(new_theme)
            return true
        end
    end
    return false
end

-- Load saved theme on startup
local saved_theme = prefs.get("theme", "/system/themes/dark.lua")
ui.load_theme(saved_theme)

return ui
