-- AIOS Lua API test script
print("api_test: starting")

-- Test aios.os
local ms = aios.os.millis()
print("api_test: millis = " .. tostring(ms))

-- Test aios.io
aios.io.writefile("/test/from_lua.txt", "written by Lua")
local content = aios.io.readfile("/test/from_lua.txt")
print("api_test: readfile = " .. tostring(content))

-- Test aios.os.meminfo
local mem = aios.os.meminfo()
print("api_test: heap_free = " .. tostring(mem.heap_free))

print("api_test: done")
return true
