local M = {}

function M.make_billboard(pos_x, pos_y, pos_z, w, h, camera)
    local rx, rz = camera.right_x, camera.right_z
    local half_w = w * 0.5

    local lx = pos_x - rx * half_w
    local lz = pos_z - rz * half_w
    local rx2 = pos_x + rx * half_w
    local rz2 = pos_z + rz * half_w

    -- 6 vertices for 2 triangles (billboard quad)
    return {
        { pos = {lx,  pos_y,     lz},  uv = {0, 1} },   -- BL
        { pos = {rx2, pos_y,     rz2}, uv = {1, 1} },   -- BR
        { pos = {rx2, pos_y + h, rz2}, uv = {1, 0} },   -- TR
        { pos = {lx,  pos_y,     lz},  uv = {0, 1} },   -- BL
        { pos = {rx2, pos_y + h, rz2}, uv = {1, 0} },   -- TR
        { pos = {lx,  pos_y + h, lz},  uv = {0, 0} },   -- TL
    }
end

function M.get_sprite_frame(entity_angle, cam_x, cam_z, ent_x, ent_z)
    local dx = cam_x - ent_x
    local dz = cam_z - ent_z
    local to_cam = math.atan(dx, dz)
    local rel = (to_cam - entity_angle) % (2 * math.pi)
    return math.floor((rel + math.pi / 8) / (math.pi / 4)) % 8
end

return M
