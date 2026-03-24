local M = {}
local Assets = require("lib/assets")
local Billboard = require("lib/billboard")
local Sector = require("lib/sector")
local mu = require("lib/math_util")

local RENDER_W = 320
local RENDER_H = 200
local KEY_COLOR = 0xFFFF00FF  -- magenta in BGRA (alpha=0xFF from stb_image)

-- Billboard model (reused for all sprites)
local billboard_model = nil

local function ensure_billboard_model()
    if billboard_model then return end
    billboard_model = chaos_gl.model_create(6, 2)
    -- Faces stay fixed; vertices updated per-sprite
    chaos_gl.model_set_face(billboard_model, 1, 1, 3, 2)
    chaos_gl.model_set_face(billboard_model, 2, 4, 6, 5)
end

function M.draw_sky(surface)
    local sky_tex = Assets.get_texture("sky1")
    if sky_tex then
        chaos_gl.blit_keyed(0, 0, 320, 160, sky_tex, 0)
    else
        chaos_gl.rect(0, 0, RENDER_W, 160, mu.CHAOS_GL_RGB(40, 40, 60))
    end
end

function M.draw_sectors(level, visible)
    chaos_gl.set_transform(0, 0, 0, 0, 0, 0, 1, 1, 1)

    for id, _ in pairs(visible) do
        local sector = level.sectors[id]
        if sector and sector._models then
            for _, m in ipairs(sector._models) do
                local uniforms = {
                    texture = m.texture,
                    light_dir_x = 0.3,
                    light_dir_y = -0.8,
                    light_dir_z = 0.5,
                    ambient = m.light or 0.3,
                }
                chaos_gl.draw_model(m.model, "diffuse", uniforms)
            end
        end
    end
end

local function get_sprite_row(ent)
    local state = ent.anim_state or ent.ai_state or "idle"
    if state == "idle" then return 0
    elseif state == "chase" then return ent.anim_frame == 0 and 1 or 2
    elseif state == "attack" then return ent.anim_frame == 0 and 3 or 4
    elseif state == "pain" then return 5
    elseif state == "dying" then return 6
    elseif state == "dead" then return 7
    end
    return 0
end

local function get_entity_texture(ent)
    if ent.type == "imp" then return Assets.get_texture("imp") end
    if ent.type == "health" then return Assets.get_texture("health") end
    if ent.type == "ammo" then return Assets.get_texture("ammo") end
    if ent.type == "key_red" then return Assets.get_texture("key_red") end
    if ent.type == "key_blue" then return Assets.get_texture("key_blue") end
    if ent.type == "barrel" then return Assets.get_texture("barrel") end
    return nil
end

function M.draw_entities(entities, visible, player, camera, level)
    ensure_billboard_model()

    for _, ent in ipairs(entities) do
        if not ent.alive and ent.type ~= "imp" then goto continue end
        if ent.type == "imp" and ent.ai_state == "dead" then
            -- Still draw dead corpse
        elseif not ent.alive then
            goto continue
        end

        if not visible[ent.sector_id] then goto continue end

        local tex = get_entity_texture(ent)
        if not tex then goto continue end

        -- Build billboard
        local bb = Billboard.make_billboard(ent.x, ent.y, ent.z, ent.sprite_w, ent.sprite_h, camera)

        -- Update billboard model vertices
        for vi = 1, 6 do
            chaos_gl.model_set_vertex(billboard_model, vi, bb[vi].pos[1], bb[vi].pos[2], bb[vi].pos[3])
            chaos_gl.model_set_normal(billboard_model, vi, 0, 0, 1)
            chaos_gl.model_set_uv(billboard_model, vi, bb[vi].uv[1], bb[vi].uv[2])
        end

        -- Compute UV sub-rect for sprite sheet
        local u0, v0, u1, v1 = 0, 0, 1, 1
        if ent.type == "imp" then
            local col = Billboard.get_sprite_frame(ent.angle, camera.eye_x, camera.eye_z, ent.x, ent.z)
            local row = get_sprite_row(ent)
            u0 = col / 8
            v0 = row / 8
            u1 = (col + 1) / 8
            v1 = (row + 1) / 8
        end

        local sector = level.sectors[ent.sector_id]
        local light = sector and (sector.light or 0.8) or 0.8

        chaos_gl.set_transform(0, 0, 0, 0, 0, 0, 1, 1, 1)
        chaos_gl.draw_model(billboard_model, "sprite", {
            texture = tex,
            key_color = KEY_COLOR,
            light = light,
            u0 = u0, v0 = v0, u1 = u1, v1 = v1,
        })

        ::continue::
    end
end

function M.rebuild_sector(level, id)
    local sector = level.sectors[id]
    if not sector then return end
    Sector.free_sector_models(sector)
    sector._models = Sector.generate_geometry(sector, level)
end

function M.cleanup()
    if billboard_model then
        chaos_gl.free_model(billboard_model)
        billboard_model = nil
    end
end

return M
