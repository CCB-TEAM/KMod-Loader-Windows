// Native (C++) mods: DLLs declared in mod.json ("native": "foo.dll", or {"Android": "libfoo.so", "Windows": "foo.dll"}). The DLL lives in the
// mod's package (kmod/mods/<id>/native/windows-x64/), which kmod.sync_mods mirrors into kmod/.private/mods/<id>; it is loaded from the mirror
// in place (so the original can be replaced while the game runs). The mod gets the KModApi table (include/kmod.h).
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

#include "kmod.h"
#include "kmod_boot.h"
#include "kmod_common.h"
#include "kmod_log.h"
#include "lua_host.h"
#include "reflect.h"
#include "resolver.h"
#include "x64.h"

namespace
{
struct LoadedMod
{
    std::string id, dir, data;
    HMODULE handle = nullptr;
    KModStopFn stop = nullptr;
    KModApi api{};
};
std::map<std::string, std::unique_ptr<LoadedMod>> g_mods;
std::string g_registering;                                  // mod id while its kmod_start runs (register_lua_function)

std::string PrivateRoot() { return KPrivateRoot() + "/native"; }
const char* kNativeDir = "windows-x64";

void MkdirP(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0771);
}

bool StatOf(const std::string& p, struct stat& st) { return stat(p.c_str(), &st) == 0; }

// Copies src to dst unless dst already has the same size and modification time. The copy is written under a temporary name
// and renamed, so an interrupted copy never leaves a half library behind. Returns false with err set on failure.
bool SyncFile(const std::string& src, const std::string& dst, std::string& err)
{
    struct stat a, b;
    if (!StatOf(src, a)) { err = "missing " + src; return false; }
    if (StatOf(dst, b) && b.st_size == a.st_size && b.st_mtime >= a.st_mtime) return true;
    std::string tmp = dst + ".tmp";
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) { err = "cannot read " + src; return false; }
    unlink(tmp.c_str());
    FILE* out = fopen(tmp.c_str(), "wb");
    if (!out) { fclose(in); err = "cannot write " + tmp; return false; }
    char buf[65536]; size_t n; bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) { unlink(tmp.c_str()); err = "copy failed (storage full?)"; return false; }
    unlink(dst.c_str());
    if (kmod_rename(tmp.c_str(), dst.c_str()) != 0) { unlink(tmp.c_str()); err = "cannot move the library into place"; return false; }
    return true;
}

void RemoveTree(const std::string& path)
{
    if (DIR* d = opendir(path.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string full = path + "/" + n;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) RemoveTree(full); else unlink(full.c_str());
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

// ---- the API table -------------------------------------------------------------------------------------------------
void ApiLog(int level, const char* msg)
{

    KLog(static_cast<KLogLevel>(level < 0 ? 0 : level > 3 ? 3 : level), "native", "%s", msg);
}

uintptr_t ApiImageBase() { return SDK::InSDKUtils::GetImageBase(); }

// IDA pattern ("48 8B ?? 05") -> bytes + mask
bool ParsePattern(const char* s, std::vector<int>& out)
{
    while (*s)
    {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { out.push_back(-1); s += (s[1] == '?') ? 2 : 1; continue; }
        char* end; long v = strtol(s, &end, 16);
        if (end == s || end - s > 2) return false;
        out.push_back((int)v); s = end;
    }
    return !out.empty();
}

uintptr_t ApiFindPattern(const char* pattern)
{
    std::vector<int> pat;
    if (!pattern || !ParsePattern(pattern, pat)) return 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    std::vector<uint8_t> buf;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++)
    {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t lo = base + sec[s].VirtualAddress;
        size_t len = sec[s].Misc.VirtualSize;
        for (size_t off = 0; off < len; off += (4u << 20) - pat.size())          // 4 MB windows overlapping by the pattern length
        {
            size_t n = std::min<size_t>(4u << 20, len - off);
            buf.resize(n);
            if (!Resolver_SafeRead(lo + off, buf.data(), n)) continue;
            for (size_t i = 0; i + pat.size() <= n; i++)
            {
                size_t k = 0;
                while (k < pat.size() && (pat[k] < 0 || buf[i + k] == (uint8_t)pat[k])) k++;
                if (k == pat.size()) return lo + off + i;
            }
        }
    }
    return 0;
}

int ApiHookFunction(void* target, void* detour, void** original)
{
    if (!target || !detour || !original) return -1;
    return KMod_InstallHook(target, detour, original) ? 0 : -1;
}

using namespace SDK;
void* ApiFindClass(const char* name) { return name ? KMod_FindClassByName(name) : nullptr; }
void* ApiFindFunction(const char* path) { return path ? KMod_FindFunctionByPath(path) : nullptr; }
void* ApiFindFirstOf(const char* cls) { return cls ? KMod_FindFirstInstance(cls) : nullptr; }
int ApiPropertyOffset(void* s, const char* name)
{
    if (!s || !name) return -1;
    FProperty* p = Rf::FindProp(static_cast<UStruct*>(s), name);
    return p ? Rf::OffsetOf(p) : -1;
}
int ApiIsA(void* o, void* cls) { return o && cls && Rf::IsA(static_cast<UObject*>(o), static_cast<UClass*>(cls)); }
int ApiObjectName(void* o, char* buf, int cap)
{
    if (!o || !buf || cap <= 0) return 0;
    std::string n = Rf::NameStr(static_cast<UObject*>(o));
    int len = (int)n.size() < cap - 1 ? (int)n.size() : cap - 1;
    memcpy(buf, n.data(), len); buf[len] = 0;
    return len;
}
void ApiProcessEvent(void* o, void* fn, void* params)
{
    if (o && fn) static_cast<UObject*>(o)->ProcessEvent(static_cast<UFunction*>(fn), params);
}
int ApiHookUFunction(void* fn, KModHookFn pre, KModHookFn post, void* user)
{
    return KMod_AddNativeHook(fn, pre, post, user) ? 0 : -1;
}
void ApiOnTick(KModTickFn fn, void* user) { if (fn) KMod_AddNativeTick(fn, user); }
void ApiRegisterLua(const char* name, KModLuaFn fn)
{
    if (name && fn && !g_registering.empty()) KMod_RegisterNativeLua(g_registering.c_str(), name, reinterpret_cast<int (*)(lua_State*)>(fn));
}

#define LS(L) static_cast<lua_State*>(L)
KModLuaApi MakeLuaApi()
{
    KModLuaApi a{};
    a.gettop = [](KModLua L) { return lua_gettop(LS(L)); };
    a.type = [](KModLua L, int i) { return lua_type(LS(L), i); };
    a.tointeger = [](KModLua L, int i) { return (int64_t)lua_tointeger(LS(L), i); };
    a.tonumber = [](KModLua L, int i) { return (double)lua_tonumber(LS(L), i); };
    a.toboolean = [](KModLua L, int i) { return lua_toboolean(LS(L), i); };
    a.tostring = [](KModLua L, int i, size_t* len) { return lua_tolstring(LS(L), i, len); };
    a.pushnil = [](KModLua L) { lua_pushnil(LS(L)); };
    a.pushboolean = [](KModLua L, int b) { lua_pushboolean(LS(L), b); };
    a.pushinteger = [](KModLua L, int64_t v) { lua_pushinteger(LS(L), (lua_Integer)v); };
    a.pushnumber = [](KModLua L, double v) { lua_pushnumber(LS(L), v); };
    a.pushstring = [](KModLua L, const char* s) { lua_pushstring(LS(L), s); };
    a.createtable = [](KModLua L, int na, int nr) { lua_createtable(LS(L), na, nr); };
    a.setfield = [](KModLua L, int i, const char* k) { lua_setfield(LS(L), i, k); };
    a.rawseti = [](KModLua L, int i, int64_t n) { lua_rawseti(LS(L), i, (lua_Integer)n); };
    a.error = [](KModLua L, const char* m) { return luaL_error(LS(L), "%s", m); };
    return a;
}

void FillApi(LoadedMod& m)
{
    KModApi& a = m.api;
    memset(&a, 0, sizeof(a));
    a.struct_size = sizeof(KModApi);
    a.api_level = KMOD_API_LEVEL;
    a.mod_id = m.id.c_str(); a.mod_dir = m.dir.c_str(); a.data_dir = m.data.c_str();
    a.log = ApiLog;
    a.image_base = ApiImageBase; a.find_pattern = ApiFindPattern; a.hook_function = ApiHookFunction;
    a.find_class = ApiFindClass; a.find_function = ApiFindFunction; a.find_first_of = ApiFindFirstOf;
    a.property_offset = ApiPropertyOffset; a.is_a = ApiIsA; a.object_name = ApiObjectName; a.process_event = ApiProcessEvent;
    a.hook_ufunction = ApiHookUFunction; a.on_tick = ApiOnTick; a.register_lua_function = ApiRegisterLua;
    a.lua = MakeLuaApi();
    /* the Android UI / JNI fields stay null on Windows (see kmod.h) */
    a.platform = "Windows";
}

int Fail(lua_State* L, const std::string& msg)
{
    lua_pushnil(L); lua_pushstring(L, msg.c_str());
    return 2;
}

bool SafeName(const std::string& s) { return !s.empty() && s.find('/') == std::string::npos && s.find("..") == std::string::npos && s.find('\\') == std::string::npos; }
}   // namespace

// kmod.native_load(id, modDir, libraryFile) -> true | nil, message
// Copies the library to private storage when it changed, loads it and calls kmod_start. A mod is started once per game run.
int Native_Load(lua_State* L)
{
    std::string id = luaL_checkstring(L, 1), dir = luaL_checkstring(L, 2), file = luaL_checkstring(L, 3);
    if (g_mods.count(id)) { lua_pushboolean(L, 1); return 1; }
    if (!SafeName(id) || !SafeName(file)) return Fail(L, "bad native library name");
    std::string src = dir + "/native/" + kNativeDir + "/" + file;
    std::string privDir = PrivateRoot() + "/" + id;
    std::string libPath = privDir + "/" + file;
    std::string err;
    if (dir.compare(0, KPrivateRoot().size() + 1, KPrivateRoot() + "/") == 0)
    {
        libPath = src;                                       // the mod was already mirrored into private storage (kmod.sync_mods): load it in place
        struct stat st;
        if (stat(src.c_str(), &st) != 0) return Fail(L, std::string("missing native/") + kNativeDir + "/" + file);
        MkdirP(privDir);
    }
    else
    {
        MkdirP(privDir);
        KBoot_SetActivity("mod", id.c_str(), "native copy");
        bool synced = SyncFile(src, libPath, err);
        KBoot_SetActivity(nullptr, nullptr, nullptr);
        if (!synced) return Fail(L, err);
    }

    auto m = std::make_unique<LoadedMod>();
    m->id = id; m->dir = dir; m->data = KModRoot() + "/data/" + id;
    MkdirP(m->data);
    KBoot_SetActivity("mod", id.c_str(), "native load");                 // a crash while loading or starting is blamed on the mod
    m->handle = LoadLibraryW(KWide(libPath).c_str());
    if (!m->handle)
    {
        char eb[64]; snprintf(eb, sizeof(eb), "LoadLibrary failed (error %lu: a missing dependency, or built for another platform?)", (unsigned long)GetLastError());
        std::string e = eb;
        KBoot_SetActivity(nullptr, nullptr, nullptr);
        return Fail(L, "cannot load the library: " + e);
    }
    KModStartFn start = reinterpret_cast<KModStartFn>(GetProcAddress(m->handle, "kmod_start"));
    m->stop = reinterpret_cast<KModStopFn>(GetProcAddress(m->handle, "kmod_stop"));
    if (!start) { KBoot_SetActivity(nullptr, nullptr, nullptr); FreeLibrary(m->handle); return Fail(L, "the library does not export kmod_start"); }
    FillApi(*m);
    g_registering = id;
    int rc = start(&m->api);
    g_registering.clear();
    KBoot_SetActivity(nullptr, nullptr, nullptr);
    if (rc != 0) { char b[64]; snprintf(b, sizeof(b), "kmod_start returned %d", rc); return Fail(L, b); }   // the library stays loaded: hooks it may have installed cannot be undone
    KLog(KLOG_INFO, "native", "%s: native library started", id.c_str());
    g_mods[id] = std::move(m);
    lua_pushboolean(L, 1);
    return 1;
}

// kmod.native_prune({id, ...}): deletes the private copies of libraries whose mod is gone or switched off
int Native_Prune(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::map<std::string, bool> keep;
    for (int i = 1;; i++)
    {
        lua_rawgeti(L, 1, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        keep[lua_tostring(L, -1) ? lua_tostring(L, -1) : ""] = true;
        lua_pop(L, 1);
    }
    if (DIR* d = opendir(PrivateRoot().c_str()))
    {
        std::vector<std::string> drop;
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n != "." && n != ".." && !keep.count(n) && !g_mods.count(n)) drop.push_back(n);
        }
        closedir(d);
        for (auto& n : drop) RemoveTree(PrivateRoot() + "/" + n);
    }
    return 0;
}

int Native_IsLoaded(lua_State* L) { lua_pushboolean(L, g_mods.count(luaL_checkstring(L, 1)) != 0); return 1; }

// the game is closing: call every mod's kmod_stop
void NativeMods_Shutdown()
{
    for (auto& kv : g_mods) if (kv.second->stop) kv.second->stop();
}
