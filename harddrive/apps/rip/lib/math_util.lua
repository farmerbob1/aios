local M = {}

local PI = math.pi
local TWO_PI = PI * 2

function M.CHAOS_GL_RGB(r, g, b)
    return (b) + (g * 256) + (r * 65536)
end

function M.normalize_angle(a)
    a = a % TWO_PI
    if a < 0 then a = a + TWO_PI end
    return a
end

function M.lerp(a, b, t)
    return a + (b - a) * t
end

function M.clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

function M.distance_2d(x1, z1, x2, z2)
    local dx = x2 - x1
    local dz = z2 - z1
    return math.sqrt(dx * dx + dz * dz)
end

function M.line_segments_intersect(ax, az, bx, bz, cx, cz, dx, dz)
    local denom = (bx - ax) * (dz - cz) - (bz - az) * (dx - cx)
    if math.abs(denom) < 0.0001 then return false end
    local t = ((cx - ax) * (dz - cz) - (cz - az) * (dx - cx)) / denom
    local u = ((cx - ax) * (bz - az) - (cz - az) * (bx - ax)) / denom
    return t >= 0 and t <= 1 and u >= 0 and u <= 1
end

function M.ray_circle_intersect(ox, oz, dx, dz, cx, cz, r)
    local fx = ox - cx
    local fz = oz - cz
    local a = dx * dx + dz * dz
    local b = 2 * (fx * dx + fz * dz)
    local c = fx * fx + fz * fz - r * r
    local disc = b * b - 4 * a * c
    if disc < 0 then return nil end
    disc = math.sqrt(disc)
    local t = (-b - disc) / (2 * a)
    if t >= 0 then return t end
    t = (-b + disc) / (2 * a)
    if t >= 0 then return t end
    return nil
end

return M
