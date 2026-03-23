local M = {}
local Assets = require("lib/assets")

local function add_tri(group, p0, p1, p2, n, uv0, uv1, uv2)
    local base = #group.vx
    group.vx[base + 1] = p0[1]; group.vy[base + 1] = p0[2]; group.vz[base + 1] = p0[3]
    group.vx[base + 2] = p1[1]; group.vy[base + 2] = p1[2]; group.vz[base + 2] = p1[3]
    group.vx[base + 3] = p2[1]; group.vy[base + 3] = p2[2]; group.vz[base + 3] = p2[3]
    group.nx[base + 1] = n[1]; group.ny[base + 1] = n[2]; group.nz[base + 1] = n[3]
    group.nx[base + 2] = n[1]; group.ny[base + 2] = n[2]; group.nz[base + 2] = n[3]
    group.nx[base + 3] = n[1]; group.ny[base + 3] = n[2]; group.nz[base + 3] = n[3]
    group.u[base + 1] = uv0[1]; group.v[base + 1] = uv0[2]
    group.u[base + 2] = uv1[1]; group.v[base + 2] = uv1[2]
    group.u[base + 3] = uv2[1]; group.v[base + 3] = uv2[2]
    local fi = #group.faces + 1
    group.faces[fi] = {base + 1, base + 2, base + 3}
end

local function get_group(groups, tex_name)
    if not groups[tex_name] then
        groups[tex_name] = {
            vx = {}, vy = {}, vz = {},
            nx = {}, ny = {}, nz = {},
            u = {}, v = {},
            faces = {},
        }
    end
    return groups[tex_name]
end

local function add_wall_quad(group, v0, v1, bottom, top)
    local dx = v1.x - v0.x
    local dz = v1.z - v0.z
    local len = math.sqrt(dx * dx + dz * dz)
    local wall_h = top - bottom
    -- Normal pointing inward (perpendicular to wall, left-hand rule for CCW vertices)
    local nx = -dz / len
    local nz = dx / len
    local n = {nx, 0, nz}
    local u_scale = len / 4.0
    local v_scale = wall_h / 4.0

    add_tri(group,
        {v0.x, bottom, v0.z}, {v1.x, bottom, v1.z}, {v1.x, top, v1.z},
        n, {0, v_scale}, {u_scale, v_scale}, {u_scale, 0})
    add_tri(group,
        {v0.x, bottom, v0.z}, {v1.x, top, v1.z}, {v0.x, top, v0.z},
        n, {0, v_scale}, {u_scale, 0}, {0, 0})
end

function M.generate_geometry(sector, level)
    local groups = {}
    local verts = sector.vertices
    local nv = #verts

    -- Floor (fan from vertex 0)
    local fg = get_group(groups, sector.floor_tex)
    for i = 2, nv - 1 do
        add_tri(fg,
            {verts[1].x, sector.floor_h, verts[1].z},
            {verts[i].x, sector.floor_h, verts[i].z},
            {verts[i+1].x, sector.floor_h, verts[i+1].z},
            {0, 1, 0},
            {verts[1].x / 4, verts[1].z / 4},
            {verts[i].x / 4, verts[i].z / 4},
            {verts[i+1].x / 4, verts[i+1].z / 4})
    end

    -- Ceiling (reversed winding)
    local cg = get_group(groups, sector.ceil_tex)
    for i = 2, nv - 1 do
        add_tri(cg,
            {verts[1].x, sector.ceil_h, verts[1].z},
            {verts[i+1].x, sector.ceil_h, verts[i+1].z},
            {verts[i].x, sector.ceil_h, verts[i].z},
            {0, -1, 0},
            {verts[1].x / 4, verts[1].z / 4},
            {verts[i+1].x / 4, verts[i+1].z / 4},
            {verts[i].x / 4, verts[i].z / 4})
    end

    -- Edges
    for i = 1, nv do
        local j = (i % nv) + 1
        local v0, v1 = verts[i], verts[j]
        local edge = sector.edges[i]

        if edge.type == "wall" then
            local wg = get_group(groups, edge.tex or sector.wall_tex)
            add_wall_quad(wg, v0, v1, sector.floor_h, sector.ceil_h)

        elseif edge.type == "portal" then
            local target = level.sectors[edge.target_sector]
            if target then
                local overlap_floor = math.max(sector.floor_h, target.floor_h)
                local overlap_ceil  = math.min(sector.ceil_h, target.ceil_h)
                local p_floor = edge.portal_floor or overlap_floor
                local p_ceil  = edge.portal_ceil  or overlap_ceil
                local tex = edge.step_tex or sector.wall_tex

                -- Floor step wall
                if p_floor > sector.floor_h then
                    local wg = get_group(groups, tex)
                    add_wall_quad(wg, v0, v1, sector.floor_h, p_floor)
                end
                -- Ceiling step wall
                if p_ceil < sector.ceil_h then
                    local wg = get_group(groups, tex)
                    add_wall_quad(wg, v0, v1, p_ceil, sector.ceil_h)
                end
            end
        end
    end

    -- Build ChaosGL models from groups
    local models = {}
    for tex_name, g in pairs(groups) do
        local nv_count = #g.vx
        local nf_count = #g.faces
        if nv_count > 0 and nf_count > 0 then
            local model = chaos_gl.model_create(nv_count, nf_count)
            if model then
                for vi = 1, nv_count do
                    chaos_gl.model_set_vertex(model, vi, g.vx[vi], g.vy[vi], g.vz[vi])
                    chaos_gl.model_set_normal(model, vi, g.nx[vi], g.ny[vi], g.nz[vi])
                    chaos_gl.model_set_uv(model, vi, g.u[vi], g.v[vi])
                end
                for fi = 1, nf_count do
                    local f = g.faces[fi]
                    chaos_gl.model_set_face(model, fi, f[1], f[2], f[3])
                end
                models[#models + 1] = {
                    model   = model,
                    texture = Assets.get_texture(tex_name),
                    light   = sector.light or 0.8,
                }
            end
        end
    end
    return models
end

function M.generate_all(level)
    for id, sector in pairs(level.sectors) do
        sector._models = M.generate_geometry(sector, level)
    end
end

function M.free_sector_models(sector)
    if sector._models then
        for _, m in ipairs(sector._models) do
            chaos_gl.free_model(m.model)
        end
        sector._models = nil
    end
end

return M
