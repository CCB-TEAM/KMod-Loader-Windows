-- The C++ side registered fnv1a, stats and split in the table `Native` (see src/main.cpp).
print("fnv1a('kmod') = " .. Native.fnv1a("kmod"))

local s = Native.stats(3, 1, 4, 1, 5, 9, 2, 6)
print(string.format("stats: n=%d min=%g max=%g mean=%g", s.n, s.min, s.max, s.mean))

local parts = Native.split("a,b,,c", ",")
print("split -> " .. #parts .. " parts: " .. table.concat(parts, "|"))

-- errors raised by native functions are ordinary Lua errors
local ok, err = pcall(Native.stats)
print("stats() with no numbers: " .. tostring(ok) .. " / " .. tostring(err))
