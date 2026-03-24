-- ChaosRIP Level 1: "Facility"
-- Plus-shaped layout: 5 rooms, center + N/S/E/W
--
--          ┌───┐
--          │ N │ (3)
--     ┌────┼───┼────┐
--     │ W  │ C │ E  │
--     └────┼───┼────┘
--          │ S │ (2)
--          └───┘
--
-- All rooms 6x6, center at origin. Portals on shared edges.

return {
    name = "Facility",
    spawn_sector = 1, spawn_x = 3.0, spawn_z = 3.0, spawn_angle = 0,

    zones = {
        hub   = { name = "Central Hub",   sectors = {1},       light = 0.8 },
        south = { name = "South Hall",    sectors = {2},       light = 0.7 },
        north = { name = "North Hall",    sectors = {3},       light = 0.6 },
        east  = { name = "East Wing",     sectors = {4},       light = 0.9 },
        west  = { name = "West Wing",     sectors = {5},       light = 0.4 },
    },

    textures = {
        brick1 = "/apps/rip/data/textures/brick1.png",
        brick2 = "/apps/rip/data/textures/brick2.png",
        metal1 = "/apps/rip/data/textures/metal1.png",
        floor1 = "/apps/rip/data/textures/floor1.png",
        ceil1  = "/apps/rip/data/textures/ceil1.png",
        door1  = "/apps/rip/data/textures/door1.png",
    },

    sectors = {
        -- [1] Center room (spawn)
        [1] = {
            vertices = { {x=0,z=0}, {x=6,z=0}, {x=6,z=6}, {x=0,z=6} },
            floor_h = 0.0, ceil_h = 3.5,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick1",
            edges = {
                { type = "portal", target_sector = 2 },     -- S: (0,0)-(6,0)
                { type = "portal", target_sector = 4 },     -- E: (6,0)-(6,6)
                { type = "portal", target_sector = 3 },     -- N: (6,6)-(0,6)
                { type = "portal", target_sector = 5 },     -- W: (0,6)-(0,0)
            },
        },

        -- [2] South room
        [2] = {
            vertices = { {x=0,z=-6}, {x=6,z=-6}, {x=6,z=0}, {x=0,z=0} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "wall" },                          -- S: (0,-6)-(6,-6)
                { type = "wall" },                          -- E: (6,-6)-(6,0)
                { type = "portal", target_sector = 1 },     -- N: (6,0)-(0,0)
                { type = "wall" },                          -- W: (0,0)-(0,-6)
            },
        },

        -- [3] North room
        [3] = {
            vertices = { {x=0,z=6}, {x=6,z=6}, {x=6,z=12}, {x=0,z=12} },
            floor_h = 0.0, ceil_h = 4.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            is_exit = true,
            edges = {
                { type = "portal", target_sector = 1 },     -- S: (0,6)-(6,6)
                { type = "wall" },                          -- E: (6,6)-(6,12)
                { type = "wall" },                          -- N: (6,12)-(0,12)
                { type = "wall" },                          -- W: (0,12)-(0,6)
            },
        },

        -- [4] East room
        [4] = {
            vertices = { {x=6,z=0}, {x=12,z=0}, {x=12,z=6}, {x=6,z=6} },
            floor_h = 0.0, ceil_h = 3.5,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick1",
            edges = {
                { type = "wall" },                          -- S: (6,0)-(12,0)
                { type = "wall" },                          -- E: (12,0)-(12,6)
                { type = "wall" },                          -- N: (12,6)-(6,6)
                { type = "portal", target_sector = 1 },     -- W: (6,6)-(6,0)
            },
        },

        -- [5] West room
        [5] = {
            vertices = { {x=-6,z=0}, {x=0,z=0}, {x=0,z=6}, {x=-6,z=6} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "wall" },                          -- S: (-6,0)-(0,0)
                { type = "portal", target_sector = 1 },     -- E: (0,0)-(0,6)
                { type = "wall" },                          -- N: (0,6)-(-6,6)
                { type = "wall" },                          -- W: (-6,6)-(-6,0)
            },
        },
    },

    entities = {
        { type = "ammo",     sector = 1, x = 4,  z = 2 },
        { type = "health",   sector = 2, x = 3,  z = -3 },
        { type = "imp",      sector = 4, x = 9,  z = 3, angle = 180 },
        { type = "imp",      sector = 5, x = -3, z = 3, angle = 0 },
        { type = "imp",      sector = 3, x = 3,  z = 9, angle = 180 },
        { type = "key_red",  sector = 2, x = 3,  z = -4 },
    },
}
