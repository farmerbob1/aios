-- AIOS UI Toolkit — Icon Registry
local icons = {}
local _registry = {}
local _missing = nil

function icons.init()
    icons.register("folder",       "/system/icons/folder_16.raw",
                                    "/system/icons/folder_32.raw",
                                    "/system/icons/folder_48.raw")
    icons.register("file",         "/system/icons/file_16.raw",
                                    "/system/icons/file_32.raw",
                                    "/system/icons/file_48.raw")
    icons.register("text",         "/system/icons/text_16.raw",
                                    "/system/icons/text_32.raw")
    icons.register("lua",          "/system/icons/lua_16.raw",
                                    "/system/icons/lua_32.raw")
    icons.register("image",        "/system/icons/image_16.raw",
                                    "/system/icons/image_32.raw")
    icons.register("audio",        "/system/icons/audio_16.raw",
                                    "/system/icons/audio_32.raw")
    icons.register("binary",       "/system/icons/binary_16.raw",
                                    "/system/icons/binary_32.raw")
    icons.register("cobj",         "/system/icons/cobj_32.raw")
    icons.register("raw",          "/system/icons/raw_32.raw")
    icons.register("app_shell",    "/system/icons/shell_32.raw",
                                    "/system/icons/shell_48.raw")
    icons.register("app_files",    "/system/icons/files_32.raw",
                                    "/system/icons/files_48.raw")
    icons.register("app_settings", "/system/icons/settings_32.raw",
                                    "/system/icons/settings_48.raw")
    icons.register("app_claude",   "/system/icons/claude_32.raw",
                                    "/system/icons/claude_48.raw")
    icons.register("close",        "/system/icons/close_16.raw")
    icons.register("minimize",     "/system/icons/minimize_16.raw")
    icons.register("maximize",     "/system/icons/maximize_16.raw")
    icons.register("missing",      "/system/icons/missing_16.raw",
                                    "/system/icons/missing_32.raw")
    _missing = _registry["missing"] and (_registry["missing"][32] or _registry["missing"][16]) or -1
end

function icons.register(name, ...)
    local sizes = {}
    for _, path in ipairs({...}) do
        local tex = chaos_gl.texture_load(path)
        if tex >= 0 then
            local w, h = chaos_gl.texture_get_size(tex)
            sizes[w] = tex
        end
    end
    _registry[name] = sizes
end

function icons.get(name, size)
    size = size or 32
    local entry = _registry[name]
    if not entry then return _missing or -1 end
    return entry[size] or entry[32] or entry[16] or entry[48] or _missing or -1
end

function icons.get_all_sizes(name)
    return _registry[name]
end

return icons
