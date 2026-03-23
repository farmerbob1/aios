-- ChaosRIP Level 1: "Facility"
-- Rules: every portal edge is shared exactly between two sectors.
-- All sectors are simple rectangles (4 vertices, 4 edges).
-- Edges are CCW from above: south, east, north, west.

return {
    name = "Facility",
    spawn_sector = 1, spawn_x = 3.0, spawn_z = 3.0, spawn_angle = 0,

    zones = {
        hub            = { name = "Central Hub",   sectors = {1,2,3,4,5},             light = 0.8 },
        armory         = { name = "The Armory",    sectors = {10,11,12,13,15},         light = 0.5 },
        bridge         = { name = "The Crossing",  sectors = {20,21,22,23},            light = 1.0 },
        exit_corridor  = { name = "Exit",          sectors = {30,31,32,33,34,35},      light = 0.3 },
    },

    textures = {
        brick1 = "/apps/rip/data/textures/brick1.png",
        brick2 = "/apps/rip/data/textures/brick2.png",
        metal1 = "/apps/rip/data/textures/metal1.png",
        floor1 = "/apps/rip/data/textures/floor1.png",
        ceil1  = "/apps/rip/data/textures/ceil1.png",
        door1  = "/apps/rip/data/textures/door1.png",
    },

    -- Grid layout (each cell = one sector, 6 units wide):
    --
    --  z=18 ┌─────┬─────┐
    --       │ 12  │ 13→exit
    --  z=12 ├─────┼─────┤
    --       │ 11  │     │
    --  z=9  ├──┬──┤     │
    --       │15│10│     │
    --  z=6  ├──┴──┼─────┤
    --       │  5  │ 21  │
    --  z=3  ├─────┼─────┤
    --       │  4  │ 20  │
    --  z=0  ├─────┼─────┤
    --       │  1  │  2  │         x: 0  3  6  9  12  15  18
    -- z=-3  ├─────┼─────┤              │     │     │      │
    --       │  3  │ 22  │
    -- z=-6  └─────┴─────┘

    sectors = {
        -- ═══════════════════════════════════════════════════
        -- HUB ZONE: simple grid of rooms
        -- ═══════════════════════════════════════════════════

        -- [1] Spawn room
        [1] = {
            vertices = { {x=0,z=0}, {x=6,z=0}, {x=6,z=6}, {x=0,z=6} },
            floor_h = 0.0, ceil_h = 3.5,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick1",
            edges = {
                { type = "portal", target_sector = 3 },     -- S: (0,0)-(6,0)
                { type = "portal", target_sector = 2 },     -- E: (6,0)-(6,6)
                { type = "portal", target_sector = 4 },     -- N: (6,6)-(0,6)
                { type = "wall" },                          -- W: (0,6)-(0,0)
            },
        },

        -- [2] East of spawn
        [2] = {
            vertices = { {x=6,z=0}, {x=12,z=0}, {x=12,z=6}, {x=6,z=6} },
            floor_h = 0.0, ceil_h = 3.5,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick1",
            edges = {
                { type = "portal", target_sector = 22 },    -- S: (6,0)-(12,0)
                { type = "portal", target_sector = 20 },    -- E: (12,0)-(12,6)
                { type = "portal", target_sector = 5 },     -- N: (12,6)-(6,6)
                { type = "portal", target_sector = 1 },     -- W: (6,6)-(6,0)
            },
        },

        -- [3] South of spawn
        [3] = {
            vertices = { {x=0,z=-6}, {x=6,z=-6}, {x=6,z=0}, {x=0,z=0} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "wall" },                          -- S
                { type = "portal", target_sector = 22 },    -- E: (6,-6)-(6,0)
                { type = "portal", target_sector = 1 },     -- N: (6,0)-(0,0)
                { type = "wall" },                          -- W
            },
        },

        -- [4] North corridor (hub to armory)
        [4] = {
            vertices = { {x=0,z=6}, {x=6,z=6}, {x=6,z=9}, {x=0,z=9} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick1",
            edges = {
                { type = "portal", target_sector = 1 },     -- S: (0,6)-(6,6)
                { type = "portal", target_sector = 5 },     -- E: (6,6)-(6,9)
                { type = "portal", target_sector = 15 },    -- N: (6,9)-(0,9)
                { type = "wall" },                          -- W
            },
        },

        -- [5] East corridor
        [5] = {
            vertices = { {x=6,z=6}, {x=12,z=6}, {x=12,z=9}, {x=6,z=9} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick1",
            edges = {
                { type = "portal", target_sector = 2 },     -- S: (6,6)-(12,6)
                { type = "portal", target_sector = 21 },    -- E: (12,6)-(12,9)
                { type = "wall" },                          -- N: (12,9)-(6,9)
                { type = "portal", target_sector = 4 },     -- W: (6,9)-(6,6)
            },
        },

        -- ═══════════════════════════════════════════════════
        -- ARMORY ZONE
        -- ═══════════════════════════════════════════════════

        -- [15] Red key door
        [15] = {
            vertices = { {x=0,z=9}, {x=6,z=9}, {x=6,z=10}, {x=0,z=10} },
            floor_h = 0.0, ceil_h = 0.0, door_target_h = 3.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "door1",
            is_door = true, key_required = "red",
            edges = {
                { type = "portal", target_sector = 4 },     -- S: (0,9)-(6,9)
                { type = "wall" },                          -- E
                { type = "portal", target_sector = 10 },    -- N: (6,10)-(0,10)
                { type = "wall" },                          -- W
            },
        },

        -- [10] Armory entrance
        [10] = {
            vertices = { {x=0,z=10}, {x=6,z=10}, {x=6,z=12}, {x=0,z=12} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            edges = {
                { type = "portal", target_sector = 15 },    -- S: (0,10)-(6,10)
                { type = "portal", target_sector = 11 },    -- E: (6,10)-(6,12)
                { type = "portal", target_sector = 12 },    -- N: (6,12)-(0,12)
                { type = "wall" },                          -- W
            },
        },

        -- [11] Armory side room
        [11] = {
            vertices = { {x=6,z=10}, {x=12,z=10}, {x=12,z=12}, {x=6,z=12} },
            floor_h = 0.0, ceil_h = 3.5,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            edges = {
                { type = "wall" },                          -- S
                { type = "wall" },                          -- E
                { type = "wall" },                          -- N
                { type = "portal", target_sector = 10 },    -- W: (6,12)-(6,10)
            },
        },

        -- [12] Armory back room
        [12] = {
            vertices = { {x=0,z=12}, {x=6,z=12}, {x=6,z=18}, {x=0,z=18} },
            floor_h = 0.0, ceil_h = 4.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            edges = {
                { type = "portal", target_sector = 10 },    -- S: (0,12)-(6,12)
                { type = "wall" },                          -- E
                { type = "portal", target_sector = 13 },    -- N: (6,18)-(0,18)
                { type = "wall" },                          -- W
            },
        },

        -- [13] Armory -> exit connector
        [13] = {
            vertices = { {x=0,z=18}, {x=6,z=18}, {x=6,z=21}, {x=0,z=21} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            edges = {
                { type = "portal", target_sector = 12 },    -- S: (0,18)-(6,18)
                { type = "wall" },                          -- E
                { type = "portal", target_sector = 30 },    -- N: (6,21)-(0,21)
                { type = "wall" },                          -- W
            },
        },

        -- ═══════════════════════════════════════════════════
        -- BRIDGE ZONE
        -- ═══════════════════════════════════════════════════

        -- [20] Bridge ground level
        [20] = {
            vertices = { {x=12,z=0}, {x=18,z=0}, {x=18,z=6}, {x=12,z=6} },
            floor_h = 0.0, ceil_h = 7.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "wall" },                          -- S
                { type = "portal", target_sector = 23 },    -- E: (18,0)-(18,6)
                { type = "portal", target_sector = 21 },    -- N: (18,6)-(12,6)
                { type = "portal", target_sector = 2 },     -- W: (12,6)-(12,0)
            },
        },

        -- [21] Bridge upper approach
        [21] = {
            vertices = { {x=12,z=6}, {x=18,z=6}, {x=18,z=9}, {x=12,z=9} },
            floor_h = 0.0, ceil_h = 7.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "portal", target_sector = 20 },    -- S: (12,6)-(18,6)
                { type = "wall" },                          -- E
                { type = "wall" },                          -- N
                { type = "portal", target_sector = 5 },     -- W: (12,9)-(12,6)
            },
        },

        -- [22] South passage
        [22] = {
            vertices = { {x=6,z=-6}, {x=12,z=-6}, {x=12,z=0}, {x=6,z=0} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "floor1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "wall" },                          -- S
                { type = "wall" },                          -- E
                { type = "portal", target_sector = 2 },     -- N: (12,0)-(6,0)
                { type = "portal", target_sector = 3 },     -- W: (6,0)-(6,-6)
            },
        },

        -- [23] Bridge east room (key room, elevated)
        [23] = {
            vertices = { {x=18,z=0}, {x=24,z=0}, {x=24,z=6}, {x=18,z=6} },
            floor_h = 1.0, ceil_h = 7.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            edges = {
                { type = "wall" },                          -- S
                { type = "wall" },                          -- E
                { type = "wall" },                          -- N
                { type = "portal", target_sector = 20 },    -- W: (18,6)-(18,0)
            },
        },

        -- ═══════════════════════════════════════════════════
        -- EXIT CORRIDOR
        -- ═══════════════════════════════════════════════════

        -- [30] Exit entrance
        [30] = {
            vertices = { {x=0,z=21}, {x=6,z=21}, {x=6,z=24}, {x=0,z=24} },
            floor_h = 0.0, ceil_h = 2.8,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "portal", target_sector = 13 },    -- S: (0,21)-(6,21)
                { type = "wall" },                          -- E
                { type = "portal", target_sector = 31 },    -- N: (6,24)-(0,24)
                { type = "wall" },                          -- W
            },
        },

        -- [31] Exit corridor
        [31] = {
            vertices = { {x=0,z=24}, {x=6,z=24}, {x=6,z=27}, {x=0,z=27} },
            floor_h = 0.0, ceil_h = 2.5,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "portal", target_sector = 30 },    -- S
                { type = "portal", target_sector = 32 },    -- E: (6,24)-(6,27)
                { type = "wall" },                          -- N
                { type = "wall" },                          -- W
            },
        },

        -- [32] Exit bend
        [32] = {
            vertices = { {x=6,z=24}, {x=12,z=24}, {x=12,z=27}, {x=6,z=27} },
            floor_h = 0.0, ceil_h = 2.5,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "wall" },                          -- S
                { type = "portal", target_sector = 33 },    -- E: (12,24)-(12,27)
                { type = "wall" },                          -- N
                { type = "portal", target_sector = 31 },    -- W: (6,27)-(6,24)
            },
        },

        -- [33] Pre-exit room
        [33] = {
            vertices = { {x=12,z=24}, {x=18,z=24}, {x=18,z=30}, {x=12,z=30} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "wall" },                          -- S
                { type = "wall" },                          -- E
                { type = "portal", target_sector = 34 },    -- N: (18,30)-(12,30)
                { type = "portal", target_sector = 32 },    -- W: (12,27)-(12,24)
            },
        },

        -- [34] Exit antechamber
        [34] = {
            vertices = { {x=12,z=30}, {x=18,z=30}, {x=18,z=33}, {x=12,z=33} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "brick2",
            edges = {
                { type = "portal", target_sector = 33 },    -- S
                { type = "wall" },                          -- E
                { type = "portal", target_sector = 35 },    -- N: (18,33)-(12,33)
                { type = "wall" },                          -- W
            },
        },

        -- [35] EXIT trigger
        [35] = {
            vertices = { {x=12,z=33}, {x=18,z=33}, {x=18,z=36}, {x=12,z=36} },
            floor_h = 0.0, ceil_h = 3.0,
            floor_tex = "metal1", ceil_tex = "ceil1", wall_tex = "metal1",
            is_exit = true,
            edges = {
                { type = "portal", target_sector = 34 },    -- S
                { type = "wall" },
                { type = "wall" },
                { type = "wall" },
            },
        },
    },

    entities = {
        { type = "ammo",     sector = 1,  x = 4,  z = 4 },
        { type = "health",   sector = 3,  x = 3,  z = -3 },

        { type = "imp",      sector = 20, x = 15, z = 3,   angle = 180 },
        { type = "imp",      sector = 21, x = 15, z = 7.5, angle = 270 },
        { type = "key_red",  sector = 23, x = 21, z = 3 },

        { type = "imp",      sector = 12, x = 3,  z = 15,  angle = 180 },
        { type = "imp",      sector = 12, x = 3,  z = 16,  angle = 90 },
        { type = "health",   sector = 11, x = 9,  z = 11 },
        { type = "ammo",     sector = 11, x = 9,  z = 11 },

        { type = "imp",      sector = 31, x = 3,  z = 25.5, angle = 0 },
        { type = "imp",      sector = 33, x = 15, z = 27,   angle = 270 },
        { type = "health",   sector = 33, x = 15, z = 29 },
        { type = "imp",      sector = 34, x = 15, z = 31.5, angle = 180 },
    },
}
