-- AIOS UI — File Dialog (Save As / Open)
-- Reusable file picker with directory navigation and filename input.
--
-- Usage:
--   local FileDialog = require("filedialog")
--   local dlg = FileDialog.new({
--       title = "Save As",
--       mode = "save",              -- "save" or "open"
--       default_name = "untitled.lua",
--       default_path = "/",
--       owner_surface = win.surface,
--       on_confirm = function(full_path) ... end,
--       on_cancel = function() ... end,
--   })
--   dlg:show()
--   -- In event loop: dlg:update(event), dlg:draw()

local core = require("core")

local FileDialog = {}
FileDialog.__index = FileDialog

local DLG_W = 380
local DLG_H = 340
local ITEM_H = 20
local PAD = 12
local TITLE_H = 28
local PATH_H = 22
local LIST_TOP = TITLE_H + PATH_H
local INPUT_H = 26
local BTN_H = 30
local BTN_AREA = BTN_H + PAD

function FileDialog.new(opts)
    local self = setmetatable({}, FileDialog)
    self.title = opts.title or "Save As"
    self.mode = opts.mode or "save"
    self.current_path = opts.default_path or "/"
    self.filename = opts.default_name or ""
    self.on_confirm = opts.on_confirm
    self.on_cancel = opts.on_cancel
    self._owner_surface = opts.owner_surface
    self.visible = false
    self.surface = nil
    self.overlay_surface = nil
    self.entries = {}
    self.scroll_y = 0
    self.selected = 0
    self._hovered_btn = 0
    self._cursor_pos = #self.filename
    self._screen_x = 0
    self._screen_y = 0
    return self
end

local function list_dir(path)
    local result = {}
    local ok, list = pcall(aios.io.listdir, path)
    if ok and list then
        for _, e in ipairs(list) do
            if e.name ~= "." and e.name ~= ".." then
                result[#result + 1] = e
            end
        end
    end
    -- Sort: dirs first, then by name
    table.sort(result, function(a, b)
        if a.is_dir and not b.is_dir then return true end
        if not a.is_dir and b.is_dir then return false end
        return (a.name or ""):lower() < (b.name or ""):lower()
    end)
    return result
end

function FileDialog:refresh()
    self.entries = list_dir(self.current_path)
    self.scroll_y = 0
    self.selected = 0
end

function FileDialog:navigate(path)
    self.current_path = path
    self:refresh()
end

function FileDialog:go_up()
    if self.current_path == "/" then return end
    local parent = self.current_path:match("^(.+)/[^/]+$") or "/"
    self:navigate(parent)
end

function FileDialog:show()
    -- Position relative to owner window so clicks are within its bounds
    local ox, oy, ow, oh = 0, 0, 1024, 768
    if self._owner_surface then
        ox, oy = chaos_gl.surface_get_position(self._owner_surface)
        ow, oh = chaos_gl.surface_get_size(self._owner_surface)
    end
    local dx = ox + (ow - DLG_W) // 2
    local dy = oy + (oh - DLG_H) // 2

    -- Overlay covers owner window area
    self.overlay_surface = chaos_gl.surface_create(ow, oh, false)
    chaos_gl.surface_set_position(self.overlay_surface, ox, oy)
    chaos_gl.surface_set_zorder(self.overlay_surface, 190)
    chaos_gl.surface_set_visible(self.overlay_surface, true)
    chaos_gl.surface_set_alpha(self.overlay_surface, 128)
    chaos_gl.surface_bind(self.overlay_surface)
    chaos_gl.surface_clear(self.overlay_surface, 0x00000000)
    chaos_gl.surface_present(self.overlay_surface)

    -- Dialog surface
    self.surface = chaos_gl.surface_create(DLG_W, DLG_H, false)
    chaos_gl.surface_set_position(self.surface, dx, dy)
    chaos_gl.surface_set_zorder(self.surface, 195)
    chaos_gl.surface_set_visible(self.surface, true)
    self._screen_x = dx
    self._screen_y = dy
    self.visible = true
    self:refresh()
end

function FileDialog:dismiss()
    if self.surface then
        chaos_gl.surface_destroy(self.surface)
        self.surface = nil
    end
    if self.overlay_surface then
        chaos_gl.surface_destroy(self.overlay_surface)
        self.overlay_surface = nil
    end
    self.visible = false
end

function FileDialog:confirm()
    if #self.filename == 0 then return end
    local path = self.current_path
    if path == "/" then
        path = "/" .. self.filename
    else
        path = path .. "/" .. self.filename
    end
    if self.on_confirm then self.on_confirm(path) end
    self:dismiss()
end

function FileDialog:_to_local(ex, ey)
    local sx, sy = 0, 0
    if self._owner_surface then
        sx, sy = chaos_gl.surface_get_position(self._owner_surface)
    end
    return (ex + sx) - self._screen_x, (ey + sy) - self._screen_y
end

function FileDialog:draw()
    if not self.visible or not self.surface then return end
    chaos_gl.surface_bind(self.surface)

    local bg = theme and theme.dialog_bg or theme and theme.window_bg or 0x00333333
    local title_bg = theme and theme.titlebar_bg or 0x003C3C3C
    local text_c = theme and theme.text_primary or 0x00FFFFFF
    local sec_c = theme and theme.text_secondary or 0x00AAAAAA
    local accent = theme and theme.accent or 0x00FF8800
    local field_bg = theme and theme.field_bg or 0x00222222
    local btn_bg = theme and theme.button_normal or 0x00444444
    local btn_r = theme and theme.button_radius or 4
    local border = theme and theme.border or 0x00555555
    local sel_bg = theme and theme.list_selection or 0x00334466
    local hover_bg = theme and theme.list_hover or 0x00383838

    chaos_gl.surface_clear(self.surface, bg)
    chaos_gl.rect_outline(0, 0, DLG_W, DLG_H, border, 1)

    -- Title bar
    chaos_gl.rect(0, 0, DLG_W, TITLE_H, title_bg)
    chaos_gl.text(PAD, (TITLE_H - chaos_gl.font_height(-1)) // 2, self.title, text_c, 0, 0)

    -- Path bar with Up button
    chaos_gl.rect(0, TITLE_H, DLG_W, PATH_H, field_bg)
    local up_w = chaos_gl.text_width("Up") + 12
    chaos_gl.rect_rounded(4, TITLE_H + 2, up_w, 18, 3, btn_bg)
    chaos_gl.text(10, TITLE_H + 4, "Up", sec_c, 0, 0)
    chaos_gl.text(up_w + 10, TITLE_H + 4, self.current_path, sec_c, 0, 0)

    -- File list
    local list_h = DLG_H - LIST_TOP - INPUT_H - BTN_AREA - PAD
    chaos_gl.push_clip(0, LIST_TOP, DLG_W, list_h)
    for i, entry in ipairs(self.entries) do
        local y = LIST_TOP + (i - 1) * ITEM_H - self.scroll_y
        if y + ITEM_H > LIST_TOP and y < LIST_TOP + list_h then
            if i == self.selected then
                chaos_gl.rect(0, y, DLG_W, ITEM_H, sel_bg)
            elseif i % 2 == 0 then
                chaos_gl.rect(0, y, DLG_W, ITEM_H, hover_bg)
            end
            local icon = entry.is_dir and "/" or ""
            local name_c = entry.is_dir and accent or text_c
            chaos_gl.text(PAD, y + 3, icon .. entry.name, name_c, 0, 0)
            if not entry.is_dir and entry.size then
                local sz = entry.size < 1024 and (entry.size .. " B")
                    or string.format("%.1f KB", entry.size / 1024)
                local szw = chaos_gl.text_width(sz)
                chaos_gl.text(DLG_W - szw - PAD, y + 3, sz, sec_c, 0, 0)
            end
        end
    end
    chaos_gl.pop_clip()

    -- Filename input
    local input_y = DLG_H - INPUT_H - BTN_AREA - PAD
    chaos_gl.text(PAD, input_y, "Name:", sec_c, 0, 0)
    local field_x = PAD + chaos_gl.text_width("Name:") + 8
    local field_w = DLG_W - field_x - PAD
    chaos_gl.rect(field_x, input_y - 2, field_w, INPUT_H, field_bg)
    chaos_gl.rect_outline(field_x, input_y - 2, field_w, INPUT_H, border, 1)
    chaos_gl.text(field_x + 4, input_y + 2, self.filename, text_c, 0, 0)
    -- Cursor
    if (aios.os.millis() // 500) % 2 == 0 then
        local cx = field_x + 4 + chaos_gl.text_width(self.filename:sub(1, self._cursor_pos))
        chaos_gl.rect(cx, input_y, 2, INPUT_H - 4, text_c)
    end

    -- Buttons
    local btn_y = DLG_H - BTN_AREA
    local cancel_w = chaos_gl.text_width("Cancel") + 24
    local confirm_label = self.mode == "save" and "Save" or "Open"
    local confirm_w = chaos_gl.text_width(confirm_label) + 24

    local cbx = DLG_W - PAD - confirm_w
    chaos_gl.rect_rounded(cbx, btn_y, confirm_w, BTN_H, btn_r, accent)
    local ctw = chaos_gl.text_width(confirm_label)
    chaos_gl.text(cbx + (confirm_w - ctw) // 2, btn_y + 8, confirm_label,
                  theme and theme.accent_text or 0x00FFFFFF, 0, 0)

    local cax = cbx - cancel_w - 8
    chaos_gl.rect_rounded(cax, btn_y, cancel_w, BTN_H, btn_r, btn_bg)
    local catw = chaos_gl.text_width("Cancel")
    chaos_gl.text(cax + (cancel_w - catw) // 2, btn_y + 8, "Cancel", text_c, 0, 0)

    self._btn_cancel = {x = cax, w = cancel_w, y = btn_y}
    self._btn_confirm = {x = cbx, w = confirm_w, y = btn_y}

    chaos_gl.surface_present(self.surface)
end

function FileDialog:on_input(event)
    if not self.visible then return false end

    local mx, my = 0, 0
    if event.mouse_x then
        mx, my = self:_to_local(event.mouse_x, event.mouse_y)
    end

    if event.type == EVENT_MOUSE_DOWN and event.button == 1 then
        -- Up button
        if my >= TITLE_H and my < TITLE_H + PATH_H and mx >= 4 and mx < 40 then
            self:go_up()
            return true
        end

        -- File list click
        local list_h = DLG_H - LIST_TOP - INPUT_H - BTN_AREA - PAD
        if my >= LIST_TOP and my < LIST_TOP + list_h then
            local idx = math.floor((my - LIST_TOP + self.scroll_y) / ITEM_H) + 1
            if idx >= 1 and idx <= #self.entries then
                local entry = self.entries[idx]
                if entry.is_dir then
                    local path = self.current_path
                    if path == "/" then
                        path = "/" .. entry.name
                    else
                        path = path .. "/" .. entry.name
                    end
                    self:navigate(path)
                else
                    self.selected = idx
                    self.filename = entry.name
                    self._cursor_pos = #self.filename
                end
            end
            return true
        end

        -- Cancel button
        local bc = self._btn_cancel
        if bc and mx >= bc.x and mx < bc.x + bc.w and my >= bc.y and my < bc.y + BTN_H then
            if self.on_cancel then self.on_cancel() end
            self:dismiss()
            return true
        end

        -- Confirm button
        local bf = self._btn_confirm
        if bf and mx >= bf.x and mx < bf.x + bf.w and my >= bf.y and my < bf.y + BTN_H then
            self:confirm()
            return true
        end

        return true -- modal: consume

    elseif event.type == EVENT_KEY_DOWN then
        if event.key == 1 then -- Escape
            if self.on_cancel then self.on_cancel() end
            self:dismiss()
            return true
        elseif event.key == 28 then -- Enter
            self:confirm()
            return true
        elseif event.key == 14 then -- Backspace
            if self._cursor_pos > 0 then
                self.filename = self.filename:sub(1, self._cursor_pos - 1)
                    .. self.filename:sub(self._cursor_pos + 1)
                self._cursor_pos = self._cursor_pos - 1
            end
            return true
        elseif event.key == 203 then -- Left
            self._cursor_pos = math.max(0, self._cursor_pos - 1)
            return true
        elseif event.key == 205 then -- Right
            self._cursor_pos = math.min(#self.filename, self._cursor_pos + 1)
            return true
        elseif event.key == 199 then -- Home
            self._cursor_pos = 0
            return true
        elseif event.key == 207 then -- End
            self._cursor_pos = #self.filename
            return true
        end

    elseif event.type == EVENT_KEY_CHAR then
        local ch = event.char
        if ch and #ch > 0 and ch:byte() >= 32 and ch ~= "/" then
            self.filename = self.filename:sub(1, self._cursor_pos)
                .. ch .. self.filename:sub(self._cursor_pos + 1)
            self._cursor_pos = self._cursor_pos + 1
            return true
        end

    elseif event.type == EVENT_MOUSE_WHEEL then
        local list_h = DLG_H - LIST_TOP - INPUT_H - BTN_AREA - PAD
        local max_scroll = math.max(0, #self.entries * ITEM_H - list_h)
        self.scroll_y = math.max(0, math.min(max_scroll,
            self.scroll_y + (event.wheel or 0) * -3 * ITEM_H))
        return true
    end

    return true -- modal
end

return FileDialog
