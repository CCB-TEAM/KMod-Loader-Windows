// Native functions for Lua: a mod's C++ code registers functions during kmod_start, the mod's main.lua calls them as Native.<name>(...).
// The Lua API a native function sees is the small stack API in api->lua (same meaning as the Lua 5.4 C API).
#include "kmod.h"

#include <algorithm>
#include <string>
#include <vector>

// Native.fnv1a(text) -> integer: 32-bit FNV-1a hash
static int Fnv1a(KModLua L)
{
    const KModLuaApi& lua = kmod::g_api->lua;
    size_t len = 0;
    const char* s = lua.tostring(L, 1, &len);
    if (!s) return lua.error(L, "fnv1a: expected a string");
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    lua.pushinteger(L, h);
    return 1;
}

// Native.stats(n1, n2, ...) -> { n =, min =, max =, mean = }
static int Stats(KModLua L)
{
    const KModLuaApi& lua = kmod::g_api->lua;
    int n = lua.gettop(L);
    if (n == 0) return lua.error(L, "stats: give at least one number");
    double mn = lua.tonumber(L, 1), mx = mn, sum = 0;
    for (int i = 1; i <= n; i++)
    {
        double v = lua.tonumber(L, i);
        mn = std::min(mn, v); mx = std::max(mx, v); sum += v;
    }
    lua.createtable(L, 0, 4);
    lua.pushinteger(L, n);      lua.setfield(L, -2, "n");
    lua.pushnumber(L, mn);      lua.setfield(L, -2, "min");
    lua.pushnumber(L, mx);      lua.setfield(L, -2, "max");
    lua.pushnumber(L, sum / n); lua.setfield(L, -2, "mean");
    return 1;
}

// Native.split(text, separator) -> { part1, part2, ... } (separator is one character; empty parts are kept)
static int Split(KModLua L)
{
    const KModLuaApi& lua = kmod::g_api->lua;
    size_t len = 0, sepLen = 0;
    const char* s = lua.tostring(L, 1, &len);
    const char* sep = lua.tostring(L, 2, &sepLen);
    if (!s || !sep || sepLen != 1) return lua.error(L, "split: expected (string, one-character separator)");
    std::vector<std::string> parts(1);
    for (size_t i = 0; i < len; i++)
    {
        if (s[i] == sep[0]) parts.emplace_back(); else parts.back() += s[i];
    }
    lua.createtable(L, (int)parts.size(), 0);
    for (size_t i = 0; i < parts.size(); i++)
    {
        lua.pushstring(L, parts[i].c_str());
        lua.rawseti(L, -2, (int64_t)i + 1);
    }
    return 1;
}

static int OnStart()
{
    const KModApi* api = kmod::g_api;
    api->register_lua_function("fnv1a", Fnv1a);       // only possible while kmod_start runs
    api->register_lua_function("stats", Stats);
    api->register_lua_function("split", Split);
    return 0;
}
KMOD_MOD(OnStart, nullptr)
