local M = {}
local Collision = require("lib/collision")

function M.new(level)
    return {
        x = level.spawn_x or 2.0,
        y = 0,
        z = level.spawn_z or 3.0,
        angle       = (level.spawn_angle or 0) * math.pi / 180,
        sector_id   = level.spawn_sector or 1,
        current_zone = "",

        health = 100, max_health = 100,
        ammo = 50, max_ammo = 99,
        has_key_red = false, has_key_blue = false,
        alive = true,

        weapon_state = "idle",
        weapon_timer = 0,

        eye_height = 1.6,
        move_speed = 4.0,
        turn_speed = 3.0,
        mouse_sens = 0.003,
        radius     = 0.3,
        height     = 1.8,
    }
end

function M.update(player, dt, input, level)
    if not player.alive then return end

    -- Turning (keyboard)
    if input.turn_left  then player.angle = player.angle + player.turn_speed * dt / 1000 end
    if input.turn_right then player.angle = player.angle - player.turn_speed * dt / 1000 end

    -- Mouse turning
    player.angle = player.angle - input.mouse_dx * player.mouse_sens
    player.angle = player.angle % (2 * math.pi)

    -- Direction vectors
    local fx = math.sin(player.angle)
    local fz = math.cos(player.angle)
    local rx, rz = fz, -fx

    -- Accumulate movement
    local mx, mz = 0, 0
    if input.forward  then mx = mx + fx; mz = mz + fz end
    if input.backward then mx = mx - fx; mz = mz - fz end
    if input.strafe_l then mx = mx - rx; mz = mz - rz end
    if input.strafe_r then mx = mx + rx; mz = mz + rz end

    local len = math.sqrt(mx * mx + mz * mz)
    if len > 0 then
        local speed = player.move_speed * dt / 1000
        mx = mx / len * speed
        mz = mz / len * speed
        Collision.move_player(player, player.x + mx, player.z + mz, level)
    end

    -- Update floor height
    local sector = level.sectors[player.sector_id]
    if sector then
        player.y = Collision.get_floor_at(sector, player.x, player.z)
    end
end

function M.set_camera(player)
    local eye_y = player.y + player.eye_height
    local dx = math.sin(player.angle)
    local dz = math.cos(player.angle)

    chaos_gl.set_camera(
        player.x, eye_y, player.z,
        player.x + dx, eye_y, player.z + dz,
        0, 1, 0
    )
    chaos_gl.set_perspective(60, 0, 0.1, 50.0)

    -- Compute focal length for portal projection
    local fov_rad = 60 * math.pi / 180
    local focal_x = (320 / 2) / math.tan(fov_rad / 2)
    local focal_y = focal_x  -- square pixels at this aspect

    return {
        eye_x = player.x, eye_y = eye_y, eye_z = player.z,
        right_x = dz, right_z = -dx,
        forward_x = dx, forward_z = dz,
        z_near = 0.1,
        focal_x = focal_x,
        focal_y = focal_y,
    }
end

function M.damage(player, amount)
    if not player.alive then return end
    player.health = player.health - amount
    if player.health <= 0 then
        player.health = 0
        player.alive = false
    end
end

return M
