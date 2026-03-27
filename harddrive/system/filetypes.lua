-- AIOS — File Type Registry
-- Centralized mapping of extensions to types, icons, and handler apps.

local filetypes = {}

-- Extension -> category type
local ext_to_type = {
    -- Text
    txt = "text", md = "text", log = "text", cfg = "text", ini = "text",
    -- Code
    lua = "lua", c = "text", h = "text", asm = "text",
    -- Images
    png = "image", jpg = "image", jpeg = "image", bmp = "image",
    gif = "image", raw = "image", svg = "image",
    -- Audio
    wav = "audio", mp3 = "audio", mid = "audio", pcm = "audio",
    -- 3D
    cobj = "cobj",
    -- Data
    sf2 = "binary", bin = "binary", dat = "binary",
    kaos = "binary",
}

-- Category -> icon name in icons registry
local type_to_icon = {
    text   = "text",
    lua    = "lua",
    image  = "image",
    audio  = "audio",
    cobj   = "cobj",
    binary = "binary",
    folder = "folder",
    file   = "file",
}

-- Category -> app path for opening (nil = no handler)
local type_to_app = {
    text  = "/apps/edit/main.lua",
    lua   = "/apps/edit/main.lua",
    image = "/apps/imageview/main.lua",
    cobj  = "/apps/modelview/main.lua",
    audio = "/apps/music/main.lua",
}

function filetypes.get_type(filename)
    local ext = filename:match("%.(%w+)$")
    if ext then
        return ext_to_type[ext:lower()] or "file"
    end
    return "file"
end

function filetypes.get_icon_name(filename, is_dir)
    if is_dir then return "folder" end
    local t = filetypes.get_type(filename)
    return type_to_icon[t] or "file"
end

function filetypes.get_app(filename)
    local t = filetypes.get_type(filename)
    return type_to_app[t]
end

function filetypes.get_type_label(filename, is_dir)
    if is_dir then return "Folder" end
    local ext = filename:match("%.(%w+)$")
    if ext then return ext:upper() end
    return "File"
end

return filetypes
