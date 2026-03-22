-- AIOS UI Toolkit — Dialog Widget (modal)
local core = require("core")

local Dialog = setmetatable({}, {__index = core.Widget})
Dialog.__index = Dialog

function Dialog.new(opts)
    local self = setmetatable(core.Widget.new(opts), Dialog)
    self.title = opts and opts.title or "Dialog"
    self.message = opts and opts.message or ""
    self.buttons = opts and opts.buttons or {{ label = "OK" }}
    self.modal = opts and opts.modal ~= nil and opts.modal or true
    self.focusable = false
    self.visible = false
    self.surface = nil
    self.overlay_surface = nil
    self._hovered_btn = 0
    return self
end

function Dialog:show(screen_w, screen_h)
    screen_w = screen_w or 1024
    screen_h = screen_h or 768

    -- Calculate dialog size
    local pad = 16
    local msg_w = math.min(400, chaos_gl.text_width(self.message) + pad * 2)
    if msg_w < 200 then msg_w = 200 end
    local msg_h = chaos_gl.text_height_wrapped(msg_w - pad * 2, self.message)
    local btn_h = 32
    local title_h = 28
    local dlg_w = msg_w
    local dlg_h = title_h + msg_h + pad * 3 + btn_h

    self.w = dlg_w
    self.h = dlg_h

    -- Modal overlay
    if self.modal then
        self.overlay_surface = chaos_gl.surface_create(screen_w, screen_h, false)
        chaos_gl.surface_set_position(self.overlay_surface, 0, 0)
        chaos_gl.surface_set_zorder(self.overlay_surface, 190)
        chaos_gl.surface_set_visible(self.overlay_surface, true)
        local alpha = self:get_style("dialog_overlay_alpha") or 128
        chaos_gl.surface_set_alpha(self.overlay_surface, alpha)
        chaos_gl.surface_bind(self.overlay_surface)
        chaos_gl.surface_clear(self.overlay_surface, 0x00000000)
        chaos_gl.surface_present(self.overlay_surface)
    end

    -- Dialog surface
    local dx = (screen_w - dlg_w) // 2
    local dy = (screen_h - dlg_h) // 2
    self.surface = chaos_gl.surface_create(dlg_w, dlg_h, false)
    chaos_gl.surface_set_position(self.surface, dx, dy)
    chaos_gl.surface_set_zorder(self.surface, 195)
    chaos_gl.surface_set_visible(self.surface, true)
    self._screen_x = dx
    self._screen_y = dy
    self.visible = true
end

function Dialog:dismiss()
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

function Dialog:draw()
    if not self.visible or not self.surface then return end

    chaos_gl.surface_bind(self.surface)
    local w, h = self:get_size()
    local pad = 16
    local title_h = 28

    local bg = self:get_style("dialog_bg") or self:get_style("window_bg") or 0x003A3A3A
    chaos_gl.surface_clear(self.surface, bg)

    local bc = self:get_style("window_border") or 0x00555555
    chaos_gl.rect_outline(0, 0, w, h, bc, 1)

    -- Title
    local tbg = self:get_style("titlebar_bg") or 0x003C3C3C
    chaos_gl.rect(0, 0, w, title_h, tbg)
    local tfg = self:get_style("titlebar_text") or 0x00FFFFFF
    local fh = chaos_gl.font_height(-1)
    chaos_gl.text(8, (title_h - fh) // 2, self.title, tfg, 0, 0)

    -- Message
    local msg_fg = self:get_style("text_primary") or 0x00FFFFFF
    chaos_gl.text_wrapped(pad, title_h + pad, w - pad * 2, self.message, msg_fg, 0, 0)

    -- Buttons (right-aligned at bottom)
    local btn_y = h - 32 - pad
    local btn_x = w - pad
    for i = #self.buttons, 1, -1 do
        local btn = self.buttons[i]
        local bw = chaos_gl.text_width(btn.label) + 24
        btn_x = btn_x - bw
        local bbg = (i == self._hovered_btn)
            and (self:get_style("button_hover") or 0x004A4A4A)
            or (self:get_style("button_normal") or 0x003A3A3A)
        if btn.style == "primary" then
            bbg = (i == self._hovered_btn)
                and (self:get_style("accent_hover") or 0x00FFA040)
                or (self:get_style("accent") or 0x00FF8800)
        end
        local radius = self:get_style("button_radius") or 4
        chaos_gl.rect_rounded(btn_x, btn_y, bw, 32, radius, bbg)
        local bfg = self:get_style("button_text") or 0x00FFFFFF
        local tw = chaos_gl.text_width(btn.label)
        local bfh = chaos_gl.font_height(-1)
        chaos_gl.text(btn_x + (bw - tw) // 2, btn_y + (32 - bfh) // 2, btn.label, bfg, 0, 0)

        btn._x = btn_x
        btn._w = bw
        btn_x = btn_x - 8
    end

    chaos_gl.surface_present(self.surface)
end

function Dialog:on_input(event)
    if not self.visible then return false end

    if event.type == core.EVENT_MOUSE_MOVE then
        local mx = event.mouse_x - (self._screen_x or 0)
        local my = event.mouse_y - (self._screen_y or 0)
        local btn_y = self.h - 32 - 16
        self._hovered_btn = 0
        for i, btn in ipairs(self.buttons) do
            if btn._x and mx >= btn._x and mx < btn._x + btn._w
               and my >= btn_y and my < btn_y + 32 then
                self._hovered_btn = i
            end
        end
        return true
    elseif event.type == core.EVENT_MOUSE_DOWN and event.button == 1 then
        if self._hovered_btn > 0 then
            local btn = self.buttons[self._hovered_btn]
            if btn.on_click then btn.on_click() end
            self:dismiss()
            return true
        end
        return true  -- modal: consume click on overlay
    elseif event.type == core.EVENT_KEY_DOWN then
        if event.key == core.KEY_ESCAPE then
            self:dismiss()
            return true
        end
    end
    return self.modal  -- modal consumes all events
end

return Dialog
