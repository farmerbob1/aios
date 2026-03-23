local M = {}

M.textures = {}
M.sounds = {}

function M.load_all(level_data)
    for name, path in pairs(level_data.textures) do
        local h = chaos_gl.load_texture(path)
        if h then
            M.textures[name] = h
        end
    end

    -- Load extra textures not in level texture map
    local extras = {
        sky1          = "/apps/rip/data/textures/sky1.png",
        imp           = "/apps/rip/data/textures/imp.png",
        health        = "/apps/rip/data/textures/health.png",
        ammo          = "/apps/rip/data/textures/ammo.png",
        key_red       = "/apps/rip/data/textures/key_red.png",
        key_blue      = "/apps/rip/data/textures/key_blue.png",
        barrel        = "/apps/rip/data/textures/barrel.png",
        shotgun_idle  = "/apps/rip/data/textures/shotgun_idle.png",
        shotgun_fire  = "/apps/rip/data/textures/shotgun_fire.png",
    }
    for name, path in pairs(extras) do
        local h = chaos_gl.load_texture(path)
        if h then M.textures[name] = h end
    end

    -- Sound paths (played directly, no pre-load)
    M.sounds = {
        shotgun_fire = "/apps/rip/data/sounds/shotgun.wav",
        imp_sight    = "/apps/rip/data/sounds/imp_sight.wav",
        imp_pain     = "/apps/rip/data/sounds/imp_pain.wav",
        imp_death    = "/apps/rip/data/sounds/imp_death.wav",
        pickup       = "/apps/rip/data/sounds/pickup.wav",
        door_open    = "/apps/rip/data/sounds/door.wav",
        player_pain  = "/apps/rip/data/sounds/pain.wav",
    }
end

function M.play_sound(name)
    local path = M.sounds[name]
    if path and aios.audio then
        aios.audio.play(path)
    end
end

function M.get_texture(name)
    return M.textures[name]
end

function M.free_all()
    for _, h in pairs(M.textures) do
        chaos_gl.free_texture(h)
    end
    M.textures = {}
end

return M
