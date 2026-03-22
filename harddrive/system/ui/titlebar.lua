-- AIOS UI — Shared Titlebar Drawing
-- macOS-style traffic light buttons + centered title + window border
-- Hover state is tracked centrally by the WM — no per-app code needed.

local titlebar = {}

local BTN_RADIUS = 6
local BTN_Y_CENTER = 14
local BTN_X_START = 12
local BTN_SPACING = 20
local TITLEBAR_H = 28

-- Colors
local CLR_CLOSE     = 0x004444FF
local CLR_CLOSE_H   = 0x006666FF
local CLR_MIN       = 0x0000CCFF
local CLR_MIN_H     = 0x0040DDFF
local CLR_MAX       = 0x0000CC44
local CLR_MAX_H     = 0x0030EE66

function titlebar.draw(sw, sh, title, surface)
    local tbg = theme and theme.titlebar_bg or 0x00363640
    local tfg = theme and theme.titlebar_text or 0x00E8E8F0
    local fh = chaos_gl.font_height(-1)

    -- Query hover state from WM (centrally tracked)
    local wm = require("wm")
    local hover_btn = surface and wm.get_titlebar_hover(surface) or nil

    -- Window border (rounded outline)
    local bc = theme and theme.window_border_active or 0x00555565
    chaos_gl.rect_rounded_outline(0, 0, sw, sh, 4, bc, 1)

    -- Titlebar background
    chaos_gl.rect(1, 1, sw - 2, TITLEBAR_H - 1, tbg)

    -- Separator under titlebar
    chaos_gl.rect(1, TITLEBAR_H - 1, sw - 2, 1, 0x00222228)

    -- Traffic light buttons with hover effects
    local btn_cy = BTN_Y_CENTER
    local cc = hover_btn == "close" and CLR_CLOSE_H or CLR_CLOSE
    local mc = hover_btn == "minimize" and CLR_MIN_H or CLR_MIN
    local xc = hover_btn == "maximize" and CLR_MAX_H or CLR_MAX

    chaos_gl.circle(BTN_X_START, btn_cy, BTN_RADIUS, cc)
    chaos_gl.circle(BTN_X_START + BTN_SPACING, btn_cy, BTN_RADIUS, mc)
    chaos_gl.circle(BTN_X_START + BTN_SPACING * 2, btn_cy, BTN_RADIUS, xc)

    -- Button symbols on hover
    if hover_btn == "close" then
        chaos_gl.line(BTN_X_START - 3, btn_cy - 3, BTN_X_START + 3, btn_cy + 3, 0x00000040)
        chaos_gl.line(BTN_X_START + 3, btn_cy - 3, BTN_X_START - 3, btn_cy + 3, 0x00000040)
    elseif hover_btn == "minimize" then
        chaos_gl.line(BTN_X_START + BTN_SPACING - 3, btn_cy,
                      BTN_X_START + BTN_SPACING + 3, btn_cy, 0x00000040)
    elseif hover_btn == "maximize" then
        local mx = BTN_X_START + BTN_SPACING * 2
        chaos_gl.line(mx - 3, btn_cy, mx + 3, btn_cy, 0x00000040)
        chaos_gl.line(mx, btn_cy - 3, mx, btn_cy + 3, 0x00000040)
    end

    -- Centered title
    local tw = chaos_gl.text_width(title)
    chaos_gl.text((sw - tw) // 2, (TITLEBAR_H - fh) // 2, title, tfg, 0, 0)

    -- Resize handle (bottom-right)
    local gc = theme and theme.text_secondary or 0x009999AA
    chaos_gl.line(sw - 4, sh - 2, sw - 2, sh - 4, gc)
    chaos_gl.line(sw - 7, sh - 2, sw - 2, sh - 7, gc)
    chaos_gl.line(sw - 10, sh - 2, sw - 2, sh - 10, gc)
end

function titlebar.hit_test(mx, my)
    local btn_cy = BTN_Y_CENTER
    if my > TITLEBAR_H then return nil end

    if (mx - BTN_X_START)^2 + (my - btn_cy)^2 <= (BTN_RADIUS + 2)^2 then
        return "close"
    end
    if (mx - (BTN_X_START + BTN_SPACING))^2 + (my - btn_cy)^2 <= (BTN_RADIUS + 2)^2 then
        return "minimize"
    end
    if (mx - (BTN_X_START + BTN_SPACING * 2))^2 + (my - btn_cy)^2 <= (BTN_RADIUS + 2)^2 then
        return "maximize"
    end
    return nil
end

function titlebar.handle_click(mx, my, surface)
    local btn = titlebar.hit_test(mx, my)
    if btn == "close" then
        return "close"
    elseif btn == "minimize" then
        if surface then aios.wm.minimize(surface) end
        return "minimize"
    elseif btn == "maximize" then
        if surface then aios.wm.maximize(surface) end
        return "maximize"
    end
    return nil
end

titlebar.HEIGHT = TITLEBAR_H

return titlebar
