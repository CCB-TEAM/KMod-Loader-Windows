// Lua host: reflection-driven UObject proxies, UE4SS-style globals, hooks, timers, mod loader, dev console.
// API names follow UE4SS (docs/lua-api) so existing knowledge/scripts carry over.
#include "lua_host.h"

#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "kmod_common.h"
#include "kmod_log.h"
#include "kmod_version.h"
#include "Engine_classes.hpp"
#include "modloader_lua.h"    // generated from lua/modloader.lua  (kModLoaderLua)
#include "modmanager_lua.h"   // generated from lua/modmanager.lua (kModManagerLua)
#include "commands_lua.h"       // generated from lua/commands.lua (kCommandsLua)
#include "json_lua.h"           // generated from lua/json.lua (kJsonLua)
#include "net_lua.h"            // generated from lua/net.lua (kNetLua)
#include "ue4ss_lua.h"          // generated from lua/ue4ss.lua (kUe4ssLua)
#include "ui_lua.h"             // generated from lua/ui.lua (kUiLua)
#include "lua_worker.h"
#include "http.h"
#include "net_conn.h"
#include "net_redirect.h"
#include "kmod_boot.h"
#include "win/dev_console.h"
#include "win/cmdq.h"
#include "win/overlay.h"
#include "reflect.h"

using namespace SDK;

// UE4SS' Lua build calls these lock hooks (its luauser.c is Windows-only). We only ever run Lua on the game thread.
extern "C" {
void LuaLockInitial(lua_State*) {}
void LuaLock(lua_State*) {}
void LuaUnlock(lua_State*) {}
}

// ---------------------------------------------------------------------------------------------
// utf helpers
// ---------------------------------------------------------------------------------------------
std::u16string Utf8To16(const std::string& s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        i += len;
        if (cp >= 0x10000) { cp -= 0x10000; out += char16_t(0xD800 + (cp >> 10)); out += char16_t(0xDC00 + (cp & 0x3FF)); }
        else out += char16_t(cp);
    }
    return out;
}

std::string Utf16To8(const char16_t* s, size_t len)
{
    std::string out;
    for (size_t i = 0; i < len; i++)
    {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00); }
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Reflection helpers
// ---------------------------------------------------------------------------------------------
namespace
{
enum PT
{
    T_Unknown, T_Bool, T_Int8, T_Byte, T_Int16, T_UInt16, T_Int, T_UInt32, T_Int64, T_UInt64, T_Float, T_Double,
    T_Name, T_Str, T_Text, T_Object, T_Class, T_Weak, T_Soft, T_Interface, T_Struct, T_Array, T_Enum,
    T_Delegate, T_Multicast, T_Map, T_Set
};

constexpr uint64_t CPF_ConstParm = 0x2, CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;

PT TypeOf(FProperty* p)
{
    static std::unordered_map<std::string, PT> m = {
        {"BoolProperty", T_Bool}, {"Int8Property", T_Int8}, {"ByteProperty", T_Byte}, {"Int16Property", T_Int16},
        {"UInt16Property", T_UInt16}, {"IntProperty", T_Int}, {"UInt32Property", T_UInt32}, {"Int64Property", T_Int64},
        {"UInt64Property", T_UInt64}, {"FloatProperty", T_Float}, {"DoubleProperty", T_Double}, {"NameProperty", T_Name},
        {"StrProperty", T_Str}, {"TextProperty", T_Text}, {"ObjectProperty", T_Object}, {"ClassProperty", T_Class},
        {"WeakObjectProperty", T_Weak}, {"SoftObjectProperty", T_Soft}, {"SoftClassProperty", T_Soft},
        {"InterfaceProperty", T_Interface}, {"StructProperty", T_Struct}, {"ArrayProperty", T_Array},
        {"EnumProperty", T_Enum}, {"DelegateProperty", T_Delegate}, {"MulticastInlineDelegateProperty", T_Multicast},
        {"MulticastSparseDelegateProperty", T_Multicast}, {"MapProperty", T_Map}, {"SetProperty", T_Set},
        {"ObjectPtrProperty", T_Object}, {"ClassPtrProperty", T_Class}, {"LazyObjectProperty", T_Weak}};
    // cache by FFieldClass pointer
    static std::unordered_map<FFieldClass*, PT> cache;
    auto it = cache.find(Rf::ClassOf(p));
    if (it != cache.end()) return it->second;
    auto f = m.find(Rf::NameStr(Rf::ClassOf(p)));
    PT t = f == m.end() ? T_Unknown : f->second;
    cache[Rf::ClassOf(p)] = t;
    return t;
}

std::string FNameStr(const FName& n) { return Rf::Str(n); }

UClass* FindClassCached(const std::string& name)
{
    static std::unordered_map<std::string, UClass*> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    UClass* c = Rf::FindClass(name);
    if (c) cache[name] = c;
    return c;
}

FProperty* FindProp(UStruct* s, const std::string& name)
{
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (FField* f = Rf::PropsOf(c); f; f = Rf::NextOf(f))
            if (Rf::NameStr(f) == name) return static_cast<FProperty*>(f);
    return nullptr;
}

UFunction* FindFunc(UStruct* s, const std::string& name)
{
    UClass* fcls = Rf::FunctionClass();
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (UField* f = Rf::ChildrenOf(c); f; f = Rf::NextOf(f))
            if (Rf::ClassOf(f) == fcls && Rf::NameStr(f) == name) return static_cast<UFunction*>(f);
    return nullptr;
}

uint32_t FuncParmsSize(UFunction* fn)
{
    uint32_t sz = Rf::ParamSizeOf(fn);
    for (FField* f = Rf::PropsOf(fn); f; f = Rf::NextOf(f))
    {
        FProperty* p = static_cast<FProperty*>(f);
        if (!(Rf::PropFlagsOf(p) & CPF_Parm)) continue;
        int64_t end = (int64_t)Rf::OffsetOf(p) + (int64_t)Rf::ElemSizeOf(p) * std::max(1, Rf::ArrayDimOf(p));
        if (end < 0 || end > 0x10000)
        {   // a property whose layout we misread: never size a buffer by it (a wrong value here once cost seconds of memset per call)
            static std::set<UFunction*> reported;
            if (reported.insert(fn).second)
                KLog(KLOG_WARN, "lua", "%s: parameter '%s' has an implausible layout (offset %d, element size %d, dim %d); ignored",
                     Rf::PathName(reinterpret_cast<UObject*>(fn)).c_str(), Rf::NameStr(f).c_str(), (int)Rf::OffsetOf(p), (int)Rf::ElemSizeOf(p), (int)Rf::ArrayDimOf(p));
            continue;
        }
        sz = std::max<uint32_t>(sz, (uint32_t)end);
    }
    return sz;
}

struct RawArray { void* Data; int32_t Num; int32_t Max; };

// -- engine-side string helpers (call the BP library UFunctions by name, so the engine allocates) ---------
FName MakeFName(const std::string& str)
{
    Rf::Args a = Rf::Prepare("KismetStringLibrary", "Conv_StringToName");
    std::u16string u = Utf8To16(str);
    int32_t n = (int32_t)u.size() + 1;
    a.Set("InString", FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(u.c_str())), n, n));
    if (!a.Call(Rf::CdoFor("KismetStringLibrary"))) return FName();
    return a.Get<FName>("ReturnValue");
}

FText MakeFText(const std::string& str)
{
    Rf::Args a = Rf::Prepare("KismetTextLibrary", "Conv_StringToText");
    std::u16string u = Utf8To16(str);
    int32_t n = (int32_t)u.size() + 1;
    a.Set("InString", FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(u.c_str())), n, n));
    a.Call(Rf::CdoFor("KismetTextLibrary"));
    return a.Get<FText>("ReturnValue");
}

// ---------------------------------------------------------------------------------------------
// Lua object proxies
// ---------------------------------------------------------------------------------------------
lua_State* L_ = nullptr;
constexpr const char* MT_OBJECT = "kmod.UObject";
constexpr const char* MT_PARAMS = "kmod.Params";

// FUObjectItem::SerialNumber changes whenever a slot is reused: (pointer, index, serial) identifies one object for its whole life
// FUObjectItem::SerialNumber. 0x10 is UE 5's offset; before it is trusted the values there are sampled: serial numbers are small
// non-negative integers, an object pointer or flags word in that place would not look like that. A layout that fails the check
// reports 0 for every object (the "no serial yet" case), so a stale handle is then only noticed through the pointer check.
bool g_serialChecked = false, g_serialOk = true;
inline int32_t SlotSerial(FUObjectItem* it)
{
    if (!g_serialChecked)
    {
        g_serialChecked = true;
        int good = 0, total = 0;
        TUObjectArray* a = InSDKUtils::GetGObjects();
        for (int32_t i = 0; a && i < a->Num() && total < 4000; i += 7)
        {
            FUObjectItem* x = ObjRef::ItemAt(i);
            if (!x || !x->Object) continue;
            int32_t v = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(x) + 0x10);
            total++; if (v >= 0 && v < 0x1000000) good++;
        }
        g_serialOk = total < 100 || good * 100 >= total * 98;
        KLog(g_serialOk ? KLOG_INFO : KLOG_WARN, "core", "object serial numbers: %s (%d of %d sampled values look like serial numbers)", g_serialOk ? "layout confirmed" : "layout NOT confirmed, serial checks off", good, total);
    }
    return g_serialOk ? *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(it) + 0x10) : 0;
}
struct LuaObj { UObject* p; int32_t idx; int32_t serial; };
struct LuaParams { UFunction* fn; uint8_t* parms; uint8_t* result; };   // result: only for script-function hooks (the frame's Result pointer)

void PushObject(lua_State* L, UObject* o)
{
    LuaObj* u = static_cast<LuaObj*>(lua_newuserdatauv(L, sizeof(LuaObj), 0));
    u->p = o; u->idx = o ? Rf::IndexOf(o) : -1;
    FUObjectItem* it = o ? ObjRef::ItemAt(Rf::IndexOf(o)) : nullptr;
    u->serial = it ? SlotSerial(it) : 0;
    luaL_setmetatable(L, MT_OBJECT);
}

bool ObjAlive(const LuaObj* o)
{
    if (!o || !o->p || o->idx < 0) return false;
    FUObjectItem* it = ObjRef::ItemAt(o->idx);
    if (!it || it->Object != o->p || !ObjRef::ItemUsable(it)) return false;
    // The serial number is handed out lazily (the first time anything makes a weak pointer to the object), so a handle made
    // before that has 0. Only a handle that HAS a serial can tell that the slot was reused for another object.
    int32_t now = SlotSerial(it);
    if (o->serial != 0 && now != o->serial) return false;
    if (o->serial == 0 && now != 0) const_cast<LuaObj*>(o)->serial = now;
    return true;
}

LuaObj* ToLuaObj(lua_State* L, int i) { return static_cast<LuaObj*>(luaL_testudata(L, i, MT_OBJECT)); }

UObject* CheckObj(lua_State* L, int i)
{
    LuaObj* o = ToLuaObj(L, i);
    if (!o) luaL_error(L, "expected UObject");
    if (!ObjAlive(o)) luaL_error(L, "UObject is invalid (destroyed or null)");
    return o->p;
}

void PushFromAddr(lua_State* L, FProperty* p, const uint8_t* addr);

void PushStruct(lua_State* L, UStruct* s, const uint8_t* base)
{
    lua_newtable(L);
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (FField* f = Rf::PropsOf(c); f; f = Rf::NextOf(f))
        {
            FProperty* fp = static_cast<FProperty*>(f);
            PushFromAddr(L, fp, base + Rf::OffsetOf(fp));
            lua_setfield(L, -2, Rf::NameStr(f).c_str());
        }
}

// ---- TMap / TSet (read, and in-place update of map values) and delegates -----------------------------------------------
// A TMap/TSet is a sparse array of elements (key [+ value] followed by two hash links) with an allocation bit set. The element
// stride is derived from the property layout exactly like the engine's FScriptMapLayout/FScriptSetLayout, and the walk is only
// trusted when the bit count equals the array count.
void PushDelegate(lua_State* L, FProperty* p, const uint8_t* addr);

int AlignOfProp(FProperty* p)
{
    switch (TypeOf(p))
    {
    case T_Bool: case T_Int8: case T_Byte: return 1;
    case T_Int16: case T_UInt16: return 2;
    case T_Int: case T_UInt32: case T_Float: case T_Name: case T_Weak: return 4;
    case T_Enum: return std::max(1, std::min(8, (int)Rf::ElemSizeOf(p)));
    case T_Struct: { UStruct* s = Rf::StructOf(p); int a = s ? Rf::At<int32_t>(s, Rf::L.ustruct_minalign) : 8; return (a > 0 && a <= 16) ? a : 8; }
    default: return 8;
    }
}
inline int AlignUp(int v, int a) { return (v + a - 1) / a * a; }

struct SparseView { const uint8_t* base; int stride; int32_t num; const uint32_t* bits; };
bool SparseOf(const uint8_t* addr, int stride, SparseView& s)
{
    const RawArray* data = reinterpret_cast<const RawArray*>(addr);
    s.base = static_cast<const uint8_t*>(data->Data); s.num = data->Num; s.stride = stride;
    const uint32_t* inl = reinterpret_cast<const uint32_t*>(addr + 0x10);
    const uint32_t* sec = *reinterpret_cast<const uint32_t* const*>(addr + 0x20);
    int32_t numBits = *reinterpret_cast<const int32_t*>(addr + 0x28);
    s.bits = sec ? sec : inl;
    if (s.num == 0) return true;
    return s.base && s.num > 0 && s.num < 200000 && numBits == s.num && stride > 0 && stride <= 4096;
}
inline bool SparseUsed(const SparseView& s, int i) { return (s.bits[i >> 5] >> (i & 31)) & 1; }

struct MapGeom { FProperty* kp; FProperty* vp; int valueOff; int stride; };
MapGeom GeomOf(FProperty* p)
{
    MapGeom g{};
    bool isMap = TypeOf(p) == T_Map;
    g.kp = isMap ? Rf::MapKeyOf(p) : Rf::SetElemOf(p);
    g.vp = isMap ? Rf::MapValueOf(p) : nullptr;
    int kal = AlignOfProp(g.kp), val = g.vp ? AlignOfProp(g.vp) : 1;
    int pairAl = std::max(kal, val);
    g.valueOff = g.vp ? Rf::OffsetOf(g.vp) : 0;
    int elem = g.vp ? AlignUp(g.valueOff + Rf::ElemSizeOf(g.vp), pairAl) : Rf::ElemSizeOf(g.kp);
    g.stride = AlignUp(AlignUp(elem, 4) + 8, std::max(g.vp ? pairAl : kal, 4));
    return g;
}
bool ScalarKey(FProperty* k)
{
    switch (TypeOf(k)) { case T_Bool: case T_Int8: case T_Byte: case T_Int16: case T_UInt16: case T_Int: case T_UInt32: case T_Int64: case T_UInt64:
                         case T_Float: case T_Double: case T_Name: case T_Str: case T_Enum: return true; default: return false; }
}

void PushMapOrSet(lua_State* L, FProperty* p, const uint8_t* addr)
{
    MapGeom g = GeomOf(p);
    lua_newtable(L);
    SparseView s;
    if (!g.kp || !SparseOf(addr, g.stride, s)) return;
    bool isMap = g.vp != nullptr, scalar = ScalarKey(g.kp);
    int n = 0;
    for (int i = 0; i < s.num; i++)
    {
        if (!SparseUsed(s, i)) continue;
        const uint8_t* e = s.base + (size_t)i * s.stride;
        if (!isMap) { PushFromAddr(L, g.kp, e); lua_rawseti(L, -2, ++n); }
        else if (scalar) { PushFromAddr(L, g.kp, e); PushFromAddr(L, g.vp, e + g.valueOff); lua_settable(L, -3); }
        else
        {
            lua_newtable(L);
            PushFromAddr(L, g.kp, e); lua_setfield(L, -2, "key");
            PushFromAddr(L, g.vp, e + g.valueOff); lua_setfield(L, -2, "value");
            lua_rawseti(L, -2, ++n);
        }
    }
}

void PushFromAddr(lua_State* L, FProperty* p, const uint8_t* addr)
{
    switch (TypeOf(p))
    {
    case T_Bool: { auto* b = static_cast<FBoolProperty*>(p); lua_pushboolean(L, (addr[Rf::BoolByteOffset(b)] & Rf::BoolFieldMask(b)) != 0); break; }
    case T_Int8: lua_pushinteger(L, *reinterpret_cast<const int8_t*>(addr)); break;
    case T_Byte: lua_pushinteger(L, *addr); break;
    case T_Int16: lua_pushinteger(L, *reinterpret_cast<const int16_t*>(addr)); break;
    case T_UInt16: lua_pushinteger(L, *reinterpret_cast<const uint16_t*>(addr)); break;
    case T_Int: lua_pushinteger(L, *reinterpret_cast<const int32_t*>(addr)); break;
    case T_UInt32: lua_pushinteger(L, *reinterpret_cast<const uint32_t*>(addr)); break;
    case T_Int64: case T_UInt64: lua_pushinteger(L, *reinterpret_cast<const int64_t*>(addr)); break;
    case T_Float: lua_pushnumber(L, *reinterpret_cast<const float*>(addr)); break;
    case T_Double: lua_pushnumber(L, *reinterpret_cast<const double*>(addr)); break;
    case T_Enum:
        switch (Rf::ElemSizeOf(p))
        {
        case 1: lua_pushinteger(L, *addr); break;
        case 2: lua_pushinteger(L, *reinterpret_cast<const uint16_t*>(addr)); break;
        case 4: lua_pushinteger(L, *reinterpret_cast<const int32_t*>(addr)); break;
        default: lua_pushinteger(L, *reinterpret_cast<const int64_t*>(addr)); break;
        }
        break;
    case T_Name: lua_pushstring(L, FNameStr(*reinterpret_cast<const FName*>(addr)).c_str()); break;
    case T_Str:
    {
        const FString* s = reinterpret_cast<const FString*>(addr);
        std::string u = s->ToString();
        lua_pushlstring(L, u.data(), u.size());
        break;
    }
    case T_Text:
    {
        const FText* t = reinterpret_cast<const FText*>(addr);
        if (t->TextData) lua_pushstring(L, t->ToString().c_str()); else lua_pushstring(L, "");
        break;
    }
    case T_Object: case T_Class: case T_Interface: PushObject(L, *reinterpret_cast<UObject* const*>(addr)); break;
    case T_Weak:
    {
        int32_t index = *reinterpret_cast<const int32_t*>(addr);
        FUObjectItem* it = index >= 0 ? ObjRef::ItemAt(index) : nullptr;
        PushObject(L, (it && ObjRef::ItemUsable(it)) ? it->Object : nullptr);
        break;
    }
    case T_Struct: PushStruct(L, Rf::StructOf(p), addr); break;
    case T_Array:
    {
        FArrayProperty* ap = static_cast<FArrayProperty*>(p);
        const RawArray* arr = reinterpret_cast<const RawArray*>(addr);
        lua_newtable(L);
        if (arr->Data && arr->Num > 0 && arr->Num < 100000)
            for (int32_t i = 0; i < arr->Num; i++)
            {
                PushFromAddr(L, Rf::InnerOf(ap), static_cast<const uint8_t*>(arr->Data) + (size_t)i * Rf::ElemSizeOf(Rf::InnerOf(ap)));
                lua_rawseti(L, -2, i + 1);
            }
        break;
    }
    case T_Map: case T_Set: PushMapOrSet(L, p, addr); break;
    case T_Delegate: case T_Multicast: PushDelegate(L, p, addr); break;
    default: lua_pushnil(L); break;
    }
}

FString EngineString(const std::string& utf8);

// isParam: the memory is a temporary call buffer (the engine copies strings out of it), so FString/FText may point at our temps.
bool SetFromLua(lua_State* L, int idx, FProperty* p, uint8_t* addr, bool isParam, std::deque<std::u16string>& keep, std::string& err)
{
    idx = lua_absindex(L, idx);
    switch (TypeOf(p))
    {
    case T_Bool:
    {
        auto* b = static_cast<FBoolProperty*>(p);
        if (lua_toboolean(L, idx)) addr[Rf::BoolByteOffset(b)] |= Rf::BoolFieldMask(b); else addr[Rf::BoolByteOffset(b)] &= ~Rf::BoolFieldMask(b);
        return true;
    }
    case T_Int8: case T_Byte: *addr = (uint8_t)luaL_checkinteger(L, idx); return true;
    case T_Int16: case T_UInt16: *reinterpret_cast<int16_t*>(addr) = (int16_t)luaL_checkinteger(L, idx); return true;
    case T_Int: case T_UInt32: *reinterpret_cast<int32_t*>(addr) = (int32_t)luaL_checkinteger(L, idx); return true;
    case T_Int64: case T_UInt64: *reinterpret_cast<int64_t*>(addr) = (int64_t)luaL_checkinteger(L, idx); return true;
    case T_Float: *reinterpret_cast<float*>(addr) = (float)luaL_checknumber(L, idx); return true;
    case T_Double: *reinterpret_cast<double*>(addr) = luaL_checknumber(L, idx); return true;
    case T_Enum:
    {
        int64_t v = (int64_t)luaL_checkinteger(L, idx);
        switch (Rf::ElemSizeOf(p))
        {
        case 1: *addr = (uint8_t)v; break;
        case 2: *reinterpret_cast<int16_t*>(addr) = (int16_t)v; break;
        case 4: *reinterpret_cast<int32_t*>(addr) = (int32_t)v; break;
        default: *reinterpret_cast<int64_t*>(addr) = v; break;
        }
        return true;
    }
    case T_Name: *reinterpret_cast<FName*>(addr) = MakeFName(luaL_checkstring(L, idx)); return true;
    case T_Str:
        if (!isParam)             // on an object: the new text is an engine-owned copy (the old buffer is left to the engine's GC-free heap: a small leak per write)
        {
            *reinterpret_cast<FString*>(addr) = EngineString(luaL_checkstring(L, idx));
            return true;
        }
        keep.push_back(Utf8To16(luaL_checkstring(L, idx)));
        { int32_t n = (int32_t)keep.back().size() + 1; *reinterpret_cast<FString*>(addr) = FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(keep.back().c_str())), n, n); }
        return true;
    case T_Text:
        *reinterpret_cast<FText*>(addr) = MakeFText(luaL_checkstring(L, idx));       // an engine-created FText either way
        return true;
    case T_Array:             // in place: same length as now (growing needs the engine's allocator)
    {
        if (!lua_istable(L, idx)) { err = "expected table for array"; return false; }
        FProperty* inner = Rf::InnerOf(p);
        RawArray* arr = reinterpret_cast<RawArray*>(addr);
        if ((int64_t)lua_rawlen(L, idx) != arr->Num) { err = "arrays can only be written in place: the table must have the array's current length (" + std::to_string(arr->Num) + ")"; return false; }
        for (int32_t i = 0; i < arr->Num; i++)
        {
            lua_rawgeti(L, idx, i + 1);
            bool ok = SetFromLua(L, -1, inner, static_cast<uint8_t*>(arr->Data) + (size_t)i * Rf::ElemSizeOf(inner), isParam, keep, err);
            lua_pop(L, 1);
            if (!ok) return false;
        }
        return true;
    }
    case T_Map:               // in place: the values of keys the map already has (scalar keys only); entries cannot be added or removed
    {
        if (!lua_istable(L, idx)) { err = "expected table for map"; return false; }
        MapGeom g = GeomOf(p);
        SparseView s;
        if (!g.vp || !ScalarKey(g.kp) || !SparseOf(addr, g.stride, s)) { err = "this map cannot be written (only maps with simple keys, in place)"; return false; }
        for (int i = 0; i < s.num; i++)
        {
            if (!SparseUsed(s, i)) continue;
            uint8_t* e = const_cast<uint8_t*>(s.base) + (size_t)i * s.stride;
            PushFromAddr(L, g.kp, e);
            lua_gettable(L, idx);
            bool ok = true;
            if (!lua_isnil(L, -1)) ok = SetFromLua(L, -1, g.vp, e + g.valueOff, isParam, keep, err);
            lua_pop(L, 1);
            if (!ok) return false;
        }
        return true;
    }
    case T_Object: case T_Class: case T_Interface:
    {
        if (lua_isnil(L, idx)) { *reinterpret_cast<UObject**>(addr) = nullptr; return true; }
        LuaObj* o = ToLuaObj(L, idx);
        if (!o) { err = "expected UObject or nil"; return false; }
        *reinterpret_cast<UObject**>(addr) = ObjAlive(o) ? o->p : nullptr;
        return true;
    }
    case T_Struct:
    {
        if (!lua_istable(L, idx)) { err = "expected table for struct"; return false; }
        UStruct* s = Rf::StructOf(p);
        for (UStruct* c = s; c; c = Rf::SuperOf(c))
            for (FField* f = Rf::PropsOf(c); f; f = Rf::NextOf(f))
            {
                lua_getfield(L, idx, Rf::NameStr(f).c_str());
                bool ok = true;
                if (!lua_isnil(L, -1)) { FProperty* fp = static_cast<FProperty*>(f); ok = SetFromLua(L, -1, fp, addr + Rf::OffsetOf(fp), isParam, keep, err); }
                lua_pop(L, 1);
                if (!ok) return false;
            }
        return true;
    }
    default: err = std::string("unsupported property type ") + Rf::NameStr(Rf::ClassOf(p)); return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Calling UFunctions from Lua:  obj:Func(args...)  -> return value, then out params
// ---------------------------------------------------------------------------------------------
int CallUFunctionImpl(lua_State* L, UObject* self, UFunction* fn, int firstArg)
{
    uint32_t sz = FuncParmsSize(fn);
    std::vector<uint8_t> buf(sz + 64, 0);
    std::deque<std::u16string> keep;
    int arg = firstArg;
    FProperty* ret = nullptr;
    std::vector<FProperty*> outs;
    std::string err;

    for (FField* f = Rf::PropsOf(fn); f; f = Rf::NextOf(f))
    {
        FProperty* p = static_cast<FProperty*>(f);
        if (!(Rf::PropFlagsOf(p) & CPF_Parm)) continue;
        if (Rf::PropFlagsOf(p) & CPF_ReturnParm) { ret = p; continue; }
        bool isOut = (Rf::PropFlagsOf(p) & CPF_OutParm) && !(Rf::PropFlagsOf(p) & CPF_ConstParm);
        if (isOut) { outs.push_back(p); continue; }
        if (arg > lua_gettop(L)) luaL_error(L, "missing argument '%s' for %s", Rf::NameStr(f).c_str(), Rf::NameStr(fn).c_str());
        if (!SetFromLua(L, arg, p, buf.data() + Rf::OffsetOf(p), true, keep, err))
            luaL_error(L, "argument '%s' of %s: %s", Rf::NameStr(f).c_str(), Rf::NameStr(fn).c_str(), err.c_str());
        arg++;
    }

    self->ProcessEvent(fn, buf.data());

    int n = 0;
    if (ret) { PushFromAddr(L, ret, buf.data() + Rf::OffsetOf(ret)); n++; }
    for (FProperty* o : outs) { PushFromAddr(L, o, buf.data() + Rf::OffsetOf(o)); n++; }
    return n;
}

int CallUFunction(lua_State* L)
{
    UFunction* fn = static_cast<UFunction*>(lua_touserdata(L, lua_upvalueindex(1)));
    UObject* self = CheckObj(L, 1);
    return CallUFunctionImpl(L, self, fn, 2);
}

// ---------------------------------------------------------------------------------------------
// Delegates: a property of delegate type reads as a proxy with :IsBound() :GetBindings() :Execute(...) :Broadcast(...) :Clear().
// A call goes to every bound (object, function name), like the engine's own ProcessDelegate. Binding new targets needs the
// engine's allocator and is not supported. The proxy points into the owning object: use it right away.
// ---------------------------------------------------------------------------------------------
constexpr const char* MT_DELEGATE = "kmod.Delegate";
struct LuaDelegate { UFunction* sig; uint8_t* addr; bool multi; };

void PushDelegate(lua_State* L, FProperty* p, const uint8_t* addr)
{
    if (Rf::NameStr(Rf::ClassOf(p)).find("Sparse") != std::string::npos) { lua_pushnil(L); return; }     // sparse delegates live elsewhere
    LuaDelegate* d = static_cast<LuaDelegate*>(lua_newuserdatauv(L, sizeof(LuaDelegate), 0));
    d->sig = Rf::At<UFunction*>(p, Rf::L.delegprop_sig);
    d->addr = const_cast<uint8_t*>(addr);
    d->multi = TypeOf(p) == T_Multicast;
    luaL_setmetatable(L, MT_DELEGATE);
}

std::vector<std::pair<UObject*, std::string>> DelegateTargets(const LuaDelegate* d)
{
    std::vector<std::pair<UObject*, std::string>> out;
    auto one = [&](const uint8_t* sd)
    {
        int32_t idx = *reinterpret_cast<const int32_t*>(sd), serial = *reinterpret_cast<const int32_t*>(sd + 4);
        if (idx < 0) return;
        FUObjectItem* it = ObjRef::ItemAt(idx);
        if (!it || !ObjRef::ItemUsable(it) || !it->Object) return;
        if (serial != 0 && SlotSerial(it) != serial) return;
        out.emplace_back(it->Object, FNameStr(*reinterpret_cast<const FName*>(sd + 8)));
    };
    if (!d->multi) one(d->addr);
    else
    {
        const RawArray* a = reinterpret_cast<const RawArray*>(d->addr);
        if (a->Data && a->Num > 0 && a->Num < 1000) for (int32_t i = 0; i < a->Num; i++) one(static_cast<const uint8_t*>(a->Data) + (size_t)i * 16);
    }
    return out;
}

int Delegate_Call(lua_State* L)          // :Execute(args...) / :Broadcast(args...)
{
    LuaDelegate* d = static_cast<LuaDelegate*>(luaL_checkudata(L, 1, MT_DELEGATE));
    int nret = 0;
    auto targets = DelegateTargets(d);
    for (size_t i = 0; i < targets.size(); i++)
    {
        UFunction* fn = FindFunc(Rf::ClassOf(targets[i].first), targets[i].second.c_str());
        if (!fn) continue;
        int before = lua_gettop(L);
        nret = CallUFunctionImpl(L, targets[i].first, fn, 2);
        if (i + 1 < targets.size()) lua_settop(L, before);
    }
    return d->multi ? 0 : nret;
}
int Delegate_IsBound(lua_State* L)
{
    LuaDelegate* d = static_cast<LuaDelegate*>(luaL_checkudata(L, 1, MT_DELEGATE));
    lua_pushboolean(L, !DelegateTargets(d).empty());
    return 1;
}
int Delegate_GetBindings(lua_State* L)   // { { object = UObject, name = "FunctionName" }, ... }
{
    LuaDelegate* d = static_cast<LuaDelegate*>(luaL_checkudata(L, 1, MT_DELEGATE));
    lua_newtable(L);
    int n = 0;
    for (auto& t : DelegateTargets(d))
    {
        lua_newtable(L);
        PushObject(L, t.first); lua_setfield(L, -2, "object");
        lua_pushstring(L, t.second.c_str()); lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
int Delegate_Clear(lua_State* L)
{
    LuaDelegate* d = static_cast<LuaDelegate*>(luaL_checkudata(L, 1, MT_DELEGATE));
    if (d->multi) reinterpret_cast<RawArray*>(d->addr)->Num = 0; else memset(d->addr, 0, 16);
    return 0;
}
int Delegate_Index(lua_State* L)
{
    const char* k = luaL_checkstring(L, 2);
    luaL_getmetatable(L, MT_DELEGATE);
    lua_getfield(L, -1, "__methods");
    lua_getfield(L, -1, k);
    return 1;
}
const luaL_Reg kDelegateMethods[] = {{"Execute", Delegate_Call}, {"Broadcast", Delegate_Call}, {"IsBound", Delegate_IsBound},
                                     {"GetBindings", Delegate_GetBindings}, {"Clear", Delegate_Clear}, {nullptr, nullptr}};

// RegisterCustomProperty({ Name=, Type=, BelongsToClass=, OffsetInternal= }): a property at a fixed offset that is not in the
// game's reflection data, readable and writable like a real one (simple types: integers, floats, bool byte, object pointer).
struct CustomProp { std::string cls, name, type; int off; };
std::vector<CustomProp> g_customProps;

int G_RegisterCustomProperty(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    CustomProp c;
    lua_getfield(L, 1, "Name"); c.name = luaL_optstring(L, -1, ""); lua_pop(L, 1);
    lua_getfield(L, 1, "Type"); c.type = luaL_optstring(L, -1, ""); lua_pop(L, 1);
    lua_getfield(L, 1, "BelongsToClass"); c.cls = luaL_optstring(L, -1, ""); lua_pop(L, 1);
    lua_getfield(L, 1, "OffsetInternal"); c.off = (int)luaL_optinteger(L, -1, -1); lua_pop(L, 1);
    if (c.name.empty() || c.type.empty() || c.cls.empty() || c.off < 0) return luaL_error(L, "RegisterCustomProperty: Name, Type, BelongsToClass and OffsetInternal are required");
    size_t dot = c.cls.rfind('.');                       // "/Script/Engine.Character" -> "Character"
    if (dot != std::string::npos) c.cls = c.cls.substr(dot + 1);
    g_customProps.push_back(c);
    return 0;
}

const CustomProp* FindCustom(UObject* o, const char* key)
{
    for (const CustomProp& c : g_customProps)
        if (c.name == key) { UClass* k = FindClassCached(c.cls); if (k && Rf::IsA(o, k)) return &c; }
    return nullptr;
}
bool CustomGet(lua_State* L, const CustomProp& c, UObject* o)
{
    const uint8_t* a = reinterpret_cast<const uint8_t*>(o) + c.off;
    const std::string& t = c.type;
    if (t == "ByteProperty" || t == "Int8Property") lua_pushinteger(L, *a);
    else if (t == "Int16Property") lua_pushinteger(L, *reinterpret_cast<const int16_t*>(a));
    else if (t == "UInt16Property") lua_pushinteger(L, *reinterpret_cast<const uint16_t*>(a));
    else if (t == "IntProperty") lua_pushinteger(L, *reinterpret_cast<const int32_t*>(a));
    else if (t == "UInt32Property") lua_pushinteger(L, *reinterpret_cast<const uint32_t*>(a));
    else if (t == "Int64Property" || t == "UInt64Property") lua_pushinteger(L, *reinterpret_cast<const int64_t*>(a));
    else if (t == "FloatProperty") lua_pushnumber(L, *reinterpret_cast<const float*>(a));
    else if (t == "DoubleProperty") lua_pushnumber(L, *reinterpret_cast<const double*>(a));
    else if (t == "BoolProperty") lua_pushboolean(L, *a != 0);
    else if (t == "ObjectProperty" || t == "ClassProperty") PushObject(L, *reinterpret_cast<UObject* const*>(a));
    else return false;
    return true;
}
bool CustomSet(lua_State* L, const CustomProp& c, UObject* o, int idx)
{
    uint8_t* a = reinterpret_cast<uint8_t*>(o) + c.off;
    const std::string& t = c.type;
    if (t == "ByteProperty" || t == "Int8Property") *a = (uint8_t)luaL_checkinteger(L, idx);
    else if (t == "Int16Property" || t == "UInt16Property") *reinterpret_cast<int16_t*>(a) = (int16_t)luaL_checkinteger(L, idx);
    else if (t == "IntProperty" || t == "UInt32Property") *reinterpret_cast<int32_t*>(a) = (int32_t)luaL_checkinteger(L, idx);
    else if (t == "Int64Property" || t == "UInt64Property") *reinterpret_cast<int64_t*>(a) = (int64_t)luaL_checkinteger(L, idx);
    else if (t == "FloatProperty") *reinterpret_cast<float*>(a) = (float)luaL_checknumber(L, idx);
    else if (t == "DoubleProperty") *reinterpret_cast<double*>(a) = luaL_checknumber(L, idx);
    else if (t == "BoolProperty") *a = lua_toboolean(L, idx) ? 1 : 0;
    else if (t == "ObjectProperty" || t == "ClassProperty") { LuaObj* v = ToLuaObj(L, idx); *reinterpret_cast<UObject**>(a) = (v && ObjAlive(v)) ? v->p : nullptr; }
    else return false;
    return true;
}

// LoadExport("symbol") -> the address of a symbol the game's libraries export (nil when there is none)
int G_LoadExport(lua_State* L)
{
    void* p = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), luaL_checkstring(L, 1)));
    if (p) lua_pushinteger(L, (lua_Integer)(uintptr_t)p); else lua_pushnil(L);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// UObject methods
// ---------------------------------------------------------------------------------------------
int M_IsValid(lua_State* L) { LuaObj* o = ToLuaObj(L, 1); lua_pushboolean(L, o && ObjAlive(o)); return 1; }
int M_GetFullName(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushstring(L, Rf::FullName(o).c_str()); return 1; }
int M_GetName(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushstring(L, Rf::NameStr(o).c_str()); return 1; }
// Package-qualified path ("/Game/UI/Foo.Foo_C", "/Game/Maps/M.M:PersistentLevel"), unlike GetFullName which drops package paths.
std::string PathOf(UObject* o)
{
    std::vector<UObject*> chain;
    for (UObject* c = o; c && chain.size() < 32; c = Rf::OuterOf(c)) chain.push_back(c);
    std::string path;
    for (size_t i = chain.size(); i-- > 0;)
    {
        std::string n = InSDKUtils::GetNameByIndex(Rf::NameOf(chain[i]).GetDisplayIndex());
        if (i + 1 == chain.size()) path = n;
        else path += (i + 2 == chain.size() ? "." : ":") + n;
    }
    return path;
}
int M_GetPathName(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushstring(L, PathOf(o).c_str()); return 1; }
// classObject:GetDefaultObject() -> the class default object (CDO), invalid for non-classes
int M_GetDefaultObject(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UObject* cdo = Rf::IsA(o, Rf::ClassClass()) ? Rf::CdoOf(static_cast<UClass*>(o)) : nullptr;
    PushObject(L, cdo);
    return 1;
}
int M_GetClass(lua_State* L) { UObject* o = CheckObj(L, 1); PushObject(L, Rf::ClassOf(o)); return 1; }
int M_GetOuter(lua_State* L) { UObject* o = CheckObj(L, 1); PushObject(L, Rf::OuterOf(o)); return 1; }
int M_GetAddress(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushinteger(L, (lua_Integer)(uintptr_t)o); return 1; }
int M_IsA(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UClass* c = nullptr;
    if (lua_type(L, 2) == LUA_TSTRING) c = FindClassCached(lua_tostring(L, 2));
    else if (LuaObj* co = ToLuaObj(L, 2)) c = ObjAlive(co) ? static_cast<UClass*>(co->p) : nullptr;
    lua_pushboolean(L, c && Rf::IsA(o, c));
    return 1;
}

int M_Self(lua_State* L) { lua_settop(L, 1); return 1; }     // UE4SS callbacks hand out param wrappers; obj:get() returns the object itself
// class/struct objects: classObj:IsChildOf(other), :GetSuperStruct(), :ForEachProperty(fn(propInfo)), :ForEachFunction(fn(path))
int M_IsChildOf(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UObject* other = CheckObj(L, 2);
    int guard = 0;
    bool r = false;
    for (const UStruct* c = static_cast<UStruct*>(o); c && guard < 128; c = Rf::SuperOf(c), guard++) if (c == static_cast<UStruct*>(other)) { r = true; break; }
    lua_pushboolean(L, r);
    return 1;
}
int M_GetSuperStruct(lua_State* L) { UObject* o = CheckObj(L, 1); PushObject(L, Rf::SuperOf(static_cast<UStruct*>(o))); return 1; }
int M_ForEachProperty(lua_State* L)       // fn({name, type, offset, owner}); return true to stop
{
    UObject* o = CheckObj(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    UStruct* s = Rf::IsA(o, Rf::FindClass("Struct")) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(Rf::ClassOf(o));
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (FField* f = Rf::PropsOf(c); f; f = Rf::NextOf(f))
        {
            FProperty* p = static_cast<FProperty*>(f);
            lua_pushvalue(L, 2);
            lua_newtable(L);
            lua_pushstring(L, Rf::NameStr(f).c_str()); lua_setfield(L, -2, "name");
            lua_pushstring(L, Rf::NameStr(Rf::ClassOf(p)).c_str()); lua_setfield(L, -2, "type");
            lua_pushinteger(L, Rf::OffsetOf(p)); lua_setfield(L, -2, "offset");
            lua_pushstring(L, Rf::NameStr(c).c_str()); lua_setfield(L, -2, "owner");
            lua_call(L, 1, 1);
            bool stop = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
            if (stop) return 0;
        }
    return 0;
}
int M_ForEachFunction(lua_State* L)       // fn(functionObject); return true to stop
{
    UObject* o = CheckObj(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    UStruct* s = Rf::IsA(o, Rf::FindClass("Struct")) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(Rf::ClassOf(o));
    UClass* fcls = Rf::FunctionClass();
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (UField* f = Rf::ChildrenOf(c); f; f = Rf::NextOf(f))
            if (Rf::ClassOf(f) == fcls)
            {
                lua_pushvalue(L, 2); PushObject(L, f);
                lua_call(L, 1, 1);
                bool stop = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
                if (stop) return 0;
            }
    return 0;
}

// debug helpers
int M_GetProperties(lua_State* L)   // {name = {type=,offset=,size=}, ...} for the class chain; also functions in .functions
{
    UObject* o = CheckObj(L, 1);
    UStruct* s = Rf::IsA(o, Rf::FindClass("Struct")) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(Rf::ClassOf(o));
    lua_newtable(L);
    int i = 1;
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (FField* f = Rf::PropsOf(c); f; f = Rf::NextOf(f))
        {
            FProperty* p = static_cast<FProperty*>(f);
            lua_newtable(L);
            lua_pushstring(L, Rf::NameStr(f).c_str()); lua_setfield(L, -2, "name");
            lua_pushstring(L, Rf::NameStr(Rf::ClassOf(p)).c_str()); lua_setfield(L, -2, "type");
            lua_pushinteger(L, Rf::OffsetOf(p)); lua_setfield(L, -2, "offset");
            lua_pushstring(L, Rf::NameStr(c).c_str()); lua_setfield(L, -2, "owner");
            lua_rawseti(L, -2, i++);
        }
    return 1;
}

int M_GetFunctions(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UStruct* s = Rf::IsA(o, Rf::FindClass("Struct")) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(Rf::ClassOf(o));
    UClass* fcls = Rf::FunctionClass();
    lua_newtable(L);
    int i = 1;
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (UField* f = Rf::ChildrenOf(c); f; f = Rf::NextOf(f))
            if (Rf::ClassOf(f) == fcls)
            {
                lua_pushstring(L, (Rf::NameStr(c) + "." + Rf::NameStr(f)).c_str());
                lua_rawseti(L, -2, i++);
            }
    return 1;
}

// ---- type names and function signatures (console: funcs / hook / search) ----
std::string TypeName(FProperty* p, int depth = 0)
{
    if (!p) return "?";
    if (depth > 3) return "...";
    switch (TypeOf(p))
    {
    case T_Bool: return "bool"; case T_Int8: return "int8"; case T_Byte: return "byte"; case T_Int16: return "int16";
    case T_UInt16: return "uint16"; case T_Int: return "int"; case T_UInt32: return "uint32"; case T_Int64: return "int64";
    case T_UInt64: return "uint64"; case T_Float: return "float"; case T_Double: return "double"; case T_Name: return "Name";
    case T_Str: return "String"; case T_Text: return "Text";
    case T_Object: { auto* c = Rf::ObjClassOf(p); return "Object<" + (c ? Rf::NameStr(c) : std::string("?")) + ">"; }
    case T_Class: { auto* c = Rf::MetaClassOf(p); return "Class<" + (c ? Rf::NameStr(c) : std::string("?")) + ">"; }
    case T_Struct: { auto* st = Rf::StructOf(p); return "Struct<" + (st ? Rf::NameStr(st) : std::string("?")) + ">"; }
    case T_Enum: { auto* e = Rf::EnumPropEnum(p); return "Enum<" + (e ? Rf::NameStr(e) : std::string("?")) + ">"; }
    case T_Array: return "Array<" + TypeName(Rf::InnerOf(p), depth + 1) + ">";
    case T_Map: { auto* m = static_cast<FMapProperty*>(p); return "Map<" + TypeName(Rf::MapKeyOf(m), depth + 1) + "," + TypeName(Rf::MapValueOf(m), depth + 1) + ">"; }
    case T_Set: return "Set<" + TypeName(Rf::SetElemOf(p), depth + 1) + ">";
    default: { std::string n = Rf::NameStr(Rf::ClassOf(p)); size_t k = n.rfind("Property"); return k != std::string::npos ? n.substr(0, k) : n; }
    }
}

struct ParamInfo { std::string name, type; bool out, ret, isConst; };
std::vector<ParamInfo> ParamsOf(UFunction* fn)
{
    std::vector<ParamInfo> v;
    for (FField* f = Rf::PropsOf(fn); f; f = Rf::NextOf(f))
    {
        FProperty* p = static_cast<FProperty*>(f);
        if (!(Rf::PropFlagsOf(p) & CPF_Parm)) continue;
        bool ret = (Rf::PropFlagsOf(p) & CPF_ReturnParm) != 0;
        bool out = (Rf::PropFlagsOf(p) & CPF_OutParm) && !(Rf::PropFlagsOf(p) & CPF_ConstParm);
        v.push_back({Rf::NameStr(f), TypeName(p), out && !ret, ret, (Rf::PropFlagsOf(p) & CPF_ConstParm) != 0});
    }
    return v;
}

std::string SigOf(UFunction* fn)
{
    std::string in, ret;
    for (auto& pi : ParamsOf(fn))
    {
        if (pi.ret) { ret = pi.type; continue; }
        if (!in.empty()) in += ", ";
        in += (pi.out ? "out " : "") + pi.name + ": " + pi.type;
    }
    return "(" + in + ")" + (ret.empty() ? "" : " -> " + ret);
}

// kmod.signatures(classOrObject) -> { {owner=, name=, sig=}, ... } for the class chain
int Kmod_Signatures(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UStruct* s = Rf::IsA(o, Rf::FindClass("Struct")) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(Rf::ClassOf(o));
    UClass* fcls = Rf::FunctionClass();
    lua_newtable(L);
    int i = 1;
    for (UStruct* c = s; c; c = Rf::SuperOf(c))
        for (UField* f = Rf::ChildrenOf(c); f; f = Rf::NextOf(f))
            if (Rf::ClassOf(f) == fcls)
            {
                lua_newtable(L);
                lua_pushstring(L, Rf::NameStr(c).c_str()); lua_setfield(L, -2, "owner");
                lua_pushstring(L, Rf::NameStr(f).c_str()); lua_setfield(L, -2, "name");
                lua_pushstring(L, SigOf(static_cast<UFunction*>(f)).c_str()); lua_setfield(L, -2, "sig");
                lua_rawseti(L, -2, i++);
            }
    return 1;
}

int M_GetPropertyValue(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    FProperty* p = FindProp(Rf::ClassOf(o), luaL_checkstring(L, 2));
    if (!p) { lua_pushnil(L); return 1; }
    PushFromAddr(L, p, reinterpret_cast<uint8_t*>(o) + Rf::OffsetOf(p));
    return 1;
}

int M_SetPropertyValue(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    FProperty* p = FindProp(Rf::ClassOf(o), luaL_checkstring(L, 2));
    if (!p) return luaL_error(L, "no property '%s'", lua_tostring(L, 2));
    std::deque<std::u16string> keep; std::string err;
    if (!SetFromLua(L, 3, p, reinterpret_cast<uint8_t*>(o) + Rf::OffsetOf(p), false, keep, err)) return luaL_error(L, "%s", err.c_str());
    return 0;
}

int M_CallFunction(lua_State* L)   // obj:CallFunction("Name", args...)
{
    UObject* o = CheckObj(L, 1);
    const char* name = luaL_checkstring(L, 2);
    UFunction* fn = FindFunc(Rf::ClassOf(o), name);
    if (!fn) return luaL_error(L, "no function '%s' on %s", name, Rf::NameStr(Rf::ClassOf(o)).c_str());
    return CallUFunctionImpl(L, o, fn, 3);
}

const luaL_Reg kObjectMethods[] = {
    {"IsValid", M_IsValid}, {"GetFullName", M_GetFullName}, {"GetName", M_GetName}, {"GetFName", M_GetName},
    {"GetClass", M_GetClass}, {"GetDefaultObject", M_GetDefaultObject}, {"GetPathName", M_GetPathName}, {"GetOuter", M_GetOuter}, {"GetAddress", M_GetAddress}, {"IsA", M_IsA},
    {"GetPropertyValue", M_GetPropertyValue}, {"SetPropertyValue", M_SetPropertyValue}, {"CallFunction", M_CallFunction},
    {"GetProperties", M_GetProperties}, {"GetFunctions", M_GetFunctions},
    {"get", M_Self}, {"Get", M_Self}, {"IsChildOf", M_IsChildOf}, {"GetSuperStruct", M_GetSuperStruct}, {"GetCDO", M_GetDefaultObject},
    {"ForEachProperty", M_ForEachProperty}, {"ForEachFunction", M_ForEachFunction}, {nullptr, nullptr}};

int Obj_Index(lua_State* L)
{
    LuaObj* lo = ToLuaObj(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) return 0;
    const char* key = lua_tostring(L, 2);

    // built-in methods first (also work on invalid objects, e.g. IsValid)
    luaL_getmetatable(L, MT_OBJECT);
    lua_getfield(L, -1, "__methods");
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 3);

    if (!ObjAlive(lo)) return luaL_error(L, "UObject is invalid (reading '%s')", key);
    UObject* o = lo->p;
    UStruct* cls = Rf::IsA(o, Rf::FindClass("Struct")) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(Rf::ClassOf(o));
    if (FProperty* p = FindProp(Rf::ClassOf(o), key)) { PushFromAddr(L, p, reinterpret_cast<uint8_t*>(o) + Rf::OffsetOf(p)); return 1; }
    if (!g_customProps.empty()) if (const CustomProp* c = FindCustom(o, key)) if (CustomGet(L, *c, o)) return 1;
    if (UFunction* fn = FindFunc(Rf::ClassOf(o), key)) { lua_pushlightuserdata(L, fn); lua_pushcclosure(L, CallUFunction, 1); return 1; }
    (void)cls;
    lua_pushnil(L);
    return 1;
}

int Obj_NewIndex(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    const char* key = luaL_checkstring(L, 2);
    FProperty* p = FindProp(Rf::ClassOf(o), key);
    if (!p && !g_customProps.empty()) if (const CustomProp* c = FindCustom(o, key)) { if (!CustomSet(L, *c, o, 3)) return luaL_error(L, "unsupported custom property type %s", c->type.c_str()); return 0; }
    if (!p) return luaL_error(L, "no property '%s' on %s", key, Rf::NameStr(Rf::ClassOf(o)).c_str());
    std::deque<std::u16string> keep; std::string err;
    if (!SetFromLua(L, 3, p, reinterpret_cast<uint8_t*>(o) + Rf::OffsetOf(p), false, keep, err)) return luaL_error(L, "%s", err.c_str());
    return 0;
}

int Obj_ToString(lua_State* L)
{
    LuaObj* lo = ToLuaObj(L, 1);
    if (!ObjAlive(lo)) lua_pushstring(L, "UObject(invalid)");
    else lua_pushfstring(L, "UObject: %s", Rf::FullName(lo->p).c_str());
    return 1;
}

int Obj_Eq(lua_State* L)
{
    LuaObj* a = ToLuaObj(L, 1); LuaObj* b = ToLuaObj(L, 2);
    lua_pushboolean(L, a && b && a->p == b->p);
    return 1;
}

// hook params proxy
int Params_Index(lua_State* L)
{
    LuaParams* lp = static_cast<LuaParams*>(luaL_checkudata(L, 1, MT_PARAMS));
    const char* key = luaL_checkstring(L, 2);
    for (FField* f = Rf::PropsOf(lp->fn); f; f = Rf::NextOf(f))
        if (Rf::NameStr(f) == key)
        {
            FProperty* p = static_cast<FProperty*>(f);
            bool isRet = (Rf::PropFlagsOf(p) & CPF_ReturnParm) != 0;
            PushFromAddr(L, p, (isRet && lp->result) ? lp->result : lp->parms + Rf::OffsetOf(p));
            return 1;
        }
    lua_pushnil(L);
    return 1;
}

// A string the ENGINE allocated: the caller of a hooked function frees its FString parameters with the engine's allocator,
// so a replacement must come from it too (Concat_StrStr(text, "") returns a fresh engine-owned copy; we take it over).
FString EngineString(const std::string& utf8)
{
    std::u16string u = Utf8To16(utf8);
    int32_t n = (int32_t)u.size() + 1;
    static const char16_t kEmpty[1] = { 0 };
    Rf::Args a = Rf::Prepare("KismetStringLibrary", "Concat_StrStr");
    a.Set("A", FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(u.c_str())), n, n));
    a.Set("B", FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(kEmpty)), 1, 1));
    a.Call(Rf::CdoFor("KismetStringLibrary"));
    return a.Get<FString>("ReturnValue");
}

int Params_NewIndex(lua_State* L)
{
    LuaParams* lp = static_cast<LuaParams*>(luaL_checkudata(L, 1, MT_PARAMS));
    const char* key = luaL_checkstring(L, 2);
    for (FField* f = Rf::PropsOf(lp->fn); f; f = Rf::NextOf(f))
        if (Rf::NameStr(f) == key)
        {
            if (TypeOf(static_cast<FProperty*>(f)) == T_Str)            // strings: engine-allocated copy (see EngineString)
            {
                FString* dst = reinterpret_cast<FString*>(lp->parms + Rf::OffsetOf(static_cast<FProperty*>(f)));
                *dst = EngineString(luaL_checkstring(L, 3));
                return 0;
            }
            std::deque<std::u16string> keep; std::string err;
            FProperty* fp = static_cast<FProperty*>(f);
            bool isRet = (Rf::PropFlagsOf(fp) & CPF_ReturnParm) != 0;
            uint8_t* addr = (isRet && lp->result) ? lp->result : lp->parms + Rf::OffsetOf(fp);
            if (!SetFromLua(L, 3, fp, addr, false, keep, err)) return luaL_error(L, "%s", err.c_str());
            return 0;
        }
    return luaL_error(L, "no parameter '%s'", key);
}

// ---------------------------------------------------------------------------------------------
// Object search
// ---------------------------------------------------------------------------------------------
bool IsCDO(UObject* o) { return (static_cast<uint32_t>(Rf::FlagsOf(o)) & 0x10) != 0; }

long long g_scanCount = 0;
double g_scanMs = 0;
int Kmod_Perf(lua_State* L)       // kmod.perf() -> scans, scan_ms, pe_calls, pe_ms (cumulative; for finding slow spots)
{
    unsigned long long calls; double ms;
    KMod_PerfPE(calls, ms);
    lua_pushinteger(L, g_scanCount); lua_pushnumber(L, g_scanMs); lua_pushinteger(L, (lua_Integer)calls); lua_pushnumber(L, ms);
    return 4;
}

int Kmod_PerfSample(lua_State* L)  // kmod.perf_sample(true) starts, kmod.perf_sample(false) -> { lines }
{
    bool start = lua_toboolean(L, 1) != 0;
    auto lines = KMod_PerfSample(start);
    lua_newtable(L);
    for (size_t i = 0; i < lines.size(); i++) { lua_pushstring(L, lines[i].c_str()); lua_rawseti(L, -2, (lua_Integer)i + 1); }
    return 1;
}

template <class F>
void ForEachObject(F&& f)
{
    struct Timer { LARGE_INTEGER a; Timer() { QueryPerformanceCounter(&a); } ~Timer() { LARGE_INTEGER b, fq; QueryPerformanceCounter(&b); QueryPerformanceFrequency(&fq); g_scanCount++; g_scanMs += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart; } } timer;
    TUObjectArray* a = InSDKUtils::GetGObjects();
    if (!a) return;
    for (int32_t i = 0; i < a->Num(); i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        if (!o || !ObjRef::ItemUsable(it) || !Rf::ClassOf(o)) continue;
        if (f(o)) break;
    }
}

int G_FindFirstOf(lua_State* L)
{
    UClass* c = FindClassCached(luaL_checkstring(L, 1));
    UObject* found = nullptr;
    // A scan walks every object (80k+): the last result per class is reused while that object is still alive.
    static std::unordered_map<UClass*, std::pair<UObject*, int32_t>> cache;
    if (c)
    {
        auto it = cache.find(c);
        if (it != cache.end())
        {
            FUObjectItem* item = ObjRef::ItemAt(it->second.second);
            UObject* o = it->second.first;
            if (item && item->Object == o && ObjRef::ItemUsable(item) && Rf::ClassOf(o) && !IsCDO(o) && Rf::IsA(o, c)) found = o;
        }
        if (!found)
        {
            ForEachObject([&](UObject* o) { if (!IsCDO(o) && Rf::IsA(o, c)) { found = o; return true; } return false; });
            if (found) cache[c] = {found, Rf::IndexOf(found)};
        }
    }
    PushObject(L, found);
    return 1;
}

int G_FindAllOf(lua_State* L)
{
    UClass* c = FindClassCached(luaL_checkstring(L, 1));
    lua_newtable(L);
    int n = 0;
    if (c) ForEachObject([&](UObject* o) { if (!IsCDO(o) && Rf::IsA(o, c)) { PushObject(L, o); lua_rawseti(L, -2, ++n); } return false; });
    if (n == 0) { lua_pop(L, 1); lua_pushnil(L); }   // UE4SS returns nil when nothing found
    return 1;
}

// "/Script/Engine.Actor"  |  "/Game/UI/WBP_X.WBP_X_C"  |  "Class Package.Name" -> match against GetFullName() (path components trimmed by the dump format)
std::string NormalizePath(std::string p)
{
    size_t sp = p.find(' ');
    if (sp != std::string::npos) p = p.substr(sp + 1);          // drop leading class name
    size_t slash = p.rfind('/');
    if (slash != std::string::npos) p = p.substr(slash + 1);    // keep last path component onwards
    for (char& ch : p) if (ch == ':') ch = '.';
    return p;
}

UObject* FindByPath(const std::string& path, bool functionOnly)
{
    // A scan names every object (80k+, tens of ms). Results are cached per path while the object lives; the scan itself only builds
    // the full name of objects whose short name equals the path's last component.
    static std::unordered_map<std::string, std::pair<UObject*, int32_t>> cache;
    const std::string key = (functionOnly ? "f:" : "o:") + path;
    {
        auto it = cache.find(key);
        if (it != cache.end())
        {
            FUObjectItem* item = ObjRef::ItemAt(it->second.second);
            if (item && item->Object == it->second.first && ObjRef::ItemUsable(item) && Rf::ClassOf(it->second.first)) return it->second.first;
            cache.erase(it);
        }
    }
    std::string want = NormalizePath(path);
    const size_t dot = want.rfind('.');
    const std::string last = dot == std::string::npos ? want : want.substr(dot + 1);
    UObject* found = nullptr;
    UClass* fcls = Rf::FunctionClass();
    ForEachObject([&](UObject* o) {
        if (functionOnly && Rf::ClassOf(o) != fcls) return false;
        if (Rf::NameStr(o) != last) return false;
        std::string full = Rf::FullName(o);
        size_t sp = full.find(' ');
        if (sp == std::string::npos) return false;
        std::string rest = full.substr(sp + 1);
        // exact, or a suffix that starts at a component boundary ("Class_C.Func" matches "Pkg.Class_C.Func")
        if (rest == want || (rest.size() > want.size() && rest.compare(rest.size() - want.size(), want.size(), want) == 0 &&
                             rest[rest.size() - want.size() - 1] == '.')) { found = o; return true; }
        return false;
    });
    if (found) cache[key] = {found, Rf::IndexOf(found)};
    return found;
}

int G_StaticFindObject(lua_State* L)
{
    const char* path = lua_isstring(L, lua_gettop(L)) ? lua_tostring(L, lua_gettop(L)) : "";
    PushObject(L, FindByPath(path, false));
    return 1;
}

int G_FindObject(lua_State* L)   // FindObject(className, objectShortName)
{
    UClass* c = FindClassCached(luaL_checkstring(L, 1));
    const char* name = luaL_optstring(L, 2, "");
    UObject* found = nullptr;
    static std::unordered_map<std::string, std::pair<UObject*, int32_t>> cache;       // positive results while the object lives
    const std::string key = std::to_string((uintptr_t)c) + "|" + name;
    if (c)
    {
        auto it = cache.find(key);
        if (it != cache.end())
        {
            FUObjectItem* item = ObjRef::ItemAt(it->second.second);
            if (item && item->Object == it->second.first && ObjRef::ItemUsable(item) && Rf::ClassOf(it->second.first)) found = it->second.first;
            else cache.erase(it);
        }
        if (!found)
        {
            ForEachObject([&](UObject* o) { if (Rf::IsA(o, c) && Rf::NameStr(o) == name) { found = o; return true; } return false; });
            if (found) cache[key] = {found, Rf::IndexOf(found)};
            else KLog(KLOG_DEBUG, "lua", "FindObject(%s, %s): not found (full scan)", luaL_checkstring(L, 1), name);
        }
    }
    PushObject(L, found);
    return 1;
}

int G_CreateInvalidObject(lua_State* L) { PushObject(L, nullptr); return 1; }

// ---- more of the UE4SS global API --------------------------------------------------------------------------------
pid_t g_mainTid = 0;                                       // the game thread (the Lua state's thread)

int G_IsInGameThread(lua_State* L) { lua_pushboolean(L, gettid() == g_mainTid); return 1; }
int G_GetGameThreadId(lua_State* L) { lua_pushinteger(L, g_mainTid); return 1; }
int G_GetCurrentThreadId(lua_State* L) { lua_pushinteger(L, gettid()); return 1; }
int G_IsInAsyncThread(lua_State* L) { lua_pushboolean(L, 0); return 1; }                 // kmod mods run on the game thread (Workers have their own state)

// ForEachUObject(fn(object, chunkIndex, objectIndex)): fn may return true to stop
int G_ForEachUObject(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    TUObjectArray* a = InSDKUtils::GetGObjects();
    if (!a) return 0;
    for (int32_t i = 0; i < a->Num(); i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        if (!o || !ObjRef::ItemUsable(it) || !Rf::ClassOf(o)) continue;
        lua_pushvalue(L, 1); PushObject(L, o); lua_pushinteger(L, i / 65536); lua_pushinteger(L, i);
        lua_call(L, 3, 1);
        bool stop = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
        if (stop) break;
    }
    return 0;
}

// FindObjects(count, className, shortName, requiredFlags, bannedFlags, exactClass) -> { objects }
int G_FindObjects(lua_State* L)
{
    int count = (int)luaL_optinteger(L, 1, 0);
    const char* cls = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const char* name = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";
    uint32_t req = (uint32_t)luaL_optinteger(L, 4, 0), ban = (uint32_t)luaL_optinteger(L, 5, 0);
    bool exact = lua_toboolean(L, 6) != 0;
    UClass* c = *cls ? FindClassCached(cls) : nullptr;
    lua_newtable(L);
    int n = 0;
    if (*cls && !c) return 1;
    ForEachObject([&](UObject* o)
    {
        if (c && (exact ? Rf::ClassOf(o) != c : !Rf::IsA(o, c))) return false;
        if (*name && Rf::NameStr(o) != name) return false;
        uint32_t fl = Rf::FlagsOf(o);
        if ((fl & req) != req || (fl & ban) != 0) return false;
        PushObject(L, o); lua_rawseti(L, -2, ++n);
        return count > 0 && n >= count;
    });
    return 1;
}

// StaticConstructObject(class [, outer [, name]]) -> a new object (through GameplayStatics.SpawnObject: no name or flags)
int G_StaticConstructObject(lua_State* L)
{
    UObject* cls = CheckObj(L, 1);
    UObject* outer = ToLuaObj(L, 2) ? CheckObj(L, 2) : nullptr;
    if (!outer) outer = Rf::FindFirst("GameInstance");
    Rf::Args a = Rf::Prepare("GameplayStatics", "SpawnObject");
    a.Set("ObjectClass", static_cast<UClass*>(cls)).Set("Outer", outer);
    a.Call(Rf::CdoFor("GameplayStatics"));
    PushObject(L, a.Get<UObject*>("ReturnValue"));
    return 1;
}

// DumpAllObjects([file]) / DumpAllActors([file]): one line per object into <logs>/UE4SS_ObjectDump.txt (or the given name there)
int DumpObjects(lua_State* L, bool actorsOnly)
{
    std::string name = luaL_optstring(L, 1, actorsOnly ? "ActorDump.txt" : "ObjectDump.txt");
    for (char& ch : name) if (ch == '/' || ch == '\\') ch = '_';
    std::string path = KLogDir() + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return luaL_error(L, "cannot write %s", path.c_str());
    UClass* actor = Rf::FindClass("Actor");
    int n = 0;
    ForEachObject([&](UObject* o)
    {
        if (actorsOnly && !(actor && Rf::IsA(o, actor))) return false;
        fprintf(f, "[%d] %s\n", (int)Rf::IndexOf(o), Rf::PathName(o).c_str()); n++;
        return false;
    });
    fclose(f);
    lua_pushstring(L, path.c_str()); lua_pushinteger(L, n);
    return 2;
}
int G_DumpAllObjects(lua_State* L) { return DumpObjects(L, false); }
int G_DumpAllActors(lua_State* L) { return DumpObjects(L, true); }
int G_FStringCtor(lua_State* L) { lua_settop(L, 1); return 1; }          // FString("x") / FAnsiString("x"): strings are plain Lua strings here

// kmod.func_params(path) -> { {name=, type=, out=, ret=}, ... } | nil
int Kmod_FuncParams(lua_State* L)
{
    UFunction* fn = static_cast<UFunction*>(FindByPath(luaL_checkstring(L, 1), true));
    if (!fn) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    int i = 1;
    for (auto& pi : ParamsOf(fn))
    {
        lua_newtable(L);
        lua_pushstring(L, pi.name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, pi.type.c_str()); lua_setfield(L, -2, "type");
        lua_pushboolean(L, pi.out); lua_setfield(L, -2, "out");
        lua_pushboolean(L, pi.ret); lua_setfield(L, -2, "ret");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

// kmod.search(pattern, kind, limit) -> results, total.   kind: "class" | "object" | "function" (default "class").
// Every space-separated word of `pattern` must occur in the object's full path (case-insensitive).
// results = { {path=, class=}, ... } (at most `limit`), total = number of matches.
int Kmod_Search(lua_State* L)
{
    std::string pat = Lower(luaL_checkstring(L, 1));
    std::string kind = luaL_optstring(L, 2, "class");
    int limit = (int)luaL_optinteger(L, 3, 30);
    std::vector<std::string> words;
    { size_t i = 0; while (i < pat.size()) { while (i < pat.size() && pat[i] == ' ') i++; size_t j = i; while (j < pat.size() && pat[j] != ' ') j++; if (j > i) words.push_back(pat.substr(i, j - i)); i = j; } }
    lua_newtable(L);
    int n = 0, total = 0;
    UClass* ucls = Rf::ClassClass();
    UClass* fcls = Rf::FunctionClass();
    UClass* pkg = Rf::FindClass("Package");
    ForEachObject([&](UObject* o) {
        bool isClass = Rf::IsA(o, ucls), isFunc = Rf::ClassOf(o) == fcls;
        if (kind == "class" ? !isClass : kind == "function" ? !isFunc : (isClass || isFunc || Rf::ClassOf(o) == pkg || IsCDO(o))) return false;
        std::string path = PathOf(o);
        std::string lp = Lower(path);
        for (auto& w : words) if (lp.find(w) == std::string::npos) return false;
        total++;
        if (n < limit)
        {
            lua_newtable(L);
            lua_pushstring(L, path.c_str()); lua_setfield(L, -2, "path");
            lua_pushstring(L, Rf::ClassOf(o) ? Rf::NameStr(Rf::ClassOf(o)).c_str() : ""); lua_setfield(L, -2, "class");
            lua_rawseti(L, -2, ++n);
        }
        return false;
    });
    lua_pushinteger(L, total);
    return 2;
}

// ---- type declarations for editors (LuaLS / EmmyLua annotations), generated from live reflection ----
std::string LuaIdent(std::string n)
{
    for (char& c : n) if (!(isalnum((unsigned char)c) || c == '_')) c = '_';
    if (n.empty() || isdigit((unsigned char)n[0])) n = "_" + n;
    static const char* kw[] = {"and","break","do","else","elseif","end","false","for","function","goto","if","in","local","nil","not","or","repeat","return","then","true","until","while"};
    for (const char* k : kw) if (n == k) return n + "_";
    return n;
}

std::string LuaType(FProperty* p, int depth = 0)
{
    if (!p) return "any";
    if (depth > 3) return "any";
    switch (TypeOf(p))
    {
    case T_Bool: return "boolean";
    case T_Int8: case T_Byte: case T_Int16: case T_UInt16: case T_Int: case T_UInt32: case T_Int64: case T_UInt64: case T_Float: case T_Double:
        if (TypeOf(p) == T_Byte) { auto* e = Rf::ByteEnum(p); if (e) return LuaIdent(Rf::NameStr(e)); }
        return "number";
    case T_Name: case T_Str: case T_Text: return "string";
    case T_Object: { auto* c = Rf::ObjClassOf(p); return c ? LuaIdent(Rf::NameStr(c)) : "KObject"; }
    case T_Class: case T_Weak: case T_Soft: case T_Interface: return "KObject";
    case T_Struct: { auto* st = Rf::StructOf(p); return st ? LuaIdent(Rf::NameStr(st)) : "table"; }
    case T_Enum: { auto* e = Rf::EnumPropEnum(p); return e ? LuaIdent(Rf::NameStr(e)) : "number"; }
    case T_Array: return LuaType(Rf::InnerOf(p), depth + 1) + "[]";
    case T_Set: return LuaType(Rf::SetElemOf(p), depth + 1) + "[]";
    case T_Map: { auto* m = static_cast<FMapProperty*>(p); return "table<" + LuaType(Rf::MapKeyOf(m), depth + 1) + ", " + LuaType(Rf::MapValueOf(m), depth + 1) + ">"; }
    default: return "any";
    }
}

// "/Script/Engine.Actor" -> "Script_Engine";  "/Game/UI/Foo" -> "Game";  "/Engine/..." -> "EngineContent"
std::string BucketOf(UObject* o)
{
    UObject* top = o;
    for (int i = 0; i < 32 && Rf::OuterOf(top); i++) top = Rf::OuterOf(top);
    std::string pkg = InSDKUtils::GetNameByIndex(Rf::NameOf(top).GetDisplayIndex());     // "/Script/Engine"
    std::vector<std::string> seg;
    { size_t i = 0; while (i < pkg.size()) { size_t j = pkg.find('/', i); if (j == std::string::npos) j = pkg.size(); if (j > i) seg.push_back(pkg.substr(i, j - i)); i = j + 1; } }
    if (seg.empty()) return "Other";
    if (seg[0] == "Script" && seg.size() > 1) return "Script_" + LuaIdent(seg[1]);
    if (seg[0] == "Engine") return "EngineContent";
    return LuaIdent(seg[0]);
}

// kmod.dump_types(outDir, {filter,...}) -> files, classes, structs, enums, functions
int Kmod_DumpTypes(lua_State* L)
{
    std::string outDir = luaL_checkstring(L, 1);
    std::vector<std::string> filters;
    if (lua_istable(L, 2))
        for (int i = 1;; i++) { lua_rawgeti(L, 2, i); if (lua_isnil(L, -1)) { lua_pop(L, 1); break; } filters.push_back(Lower(lua_tostring(L, -1))); lua_pop(L, 1); }
    auto wanted = [&](const std::string& bucket) {
        if (filters.empty()) return true;
        std::string b = Lower(bucket);
        for (auto& f : filters) if (b.find(f) != std::string::npos) return true;
        return false;
    };
    std::map<std::string, std::string> files;
    std::set<std::string> used;
    long nClass = 0, nStruct = 0, nEnum = 0, nFunc = 0;
    UClass* ucls = Rf::ClassClass();
    UClass* scls = Rf::FindClass("ScriptStruct");
    UClass* ecls = Rf::FindClass("Enum");
    ForEachObject([&](UObject* o) {
        bool isClass = Rf::IsA(o, ucls), isStruct = !isClass && Rf::IsA(o, scls), isEnum = !isClass && !isStruct && Rf::IsA(o, ecls);
        if (!isClass && !isStruct && !isEnum) return false;
        std::string bucket = BucketOf(o);
        if (!wanted(bucket)) return false;
        std::string name = LuaIdent(Rf::NameStr(o));
        if (!used.insert(name).second) return false;                    // same short name in two packages: first one wins
        std::string& out = files[bucket];
        if (isEnum) { out += "---@alias " + name + " integer\n\n"; nEnum++; return false; }
        UStruct* st = static_cast<UStruct*>(o);
        out += "---@class " + name;
        if (Rf::SuperOf(st)) { std::string sup = LuaIdent(Rf::NameStr(Rf::SuperOf(st))); if (sup != name) out += " : " + sup; }
        else if (isClass) out += " : KObject";
        out += "\n";
        std::set<std::string> seen;                                  // a field name may only be declared once per class
        for (FField* f = Rf::PropsOf(st); f; f = Rf::NextOf(f))
        {
            FProperty* p = static_cast<FProperty*>(f);
            std::string fname = LuaIdent(Rf::NameStr(f));
            if (!seen.insert(fname).second) continue;
            out += "---@field " + fname + " " + LuaType(p) + "\n";
        }
        if (isClass)
        {
            UClass* fcls = Rf::FunctionClass();
            for (UField* f = Rf::ChildrenOf(st); f; f = Rf::NextOf(f))
            {
                if (Rf::ClassOf(f) != fcls) continue;
                UFunction* fn = static_cast<UFunction*>(f);
                std::string fname = LuaIdent(Rf::NameStr(f));
                if (!seen.insert(fname).second) continue;
                std::string params, rets;
                std::set<std::string> pseen = {"self"};
                for (auto* ff = Rf::PropsOf(fn); ff; ff = Rf::NextOf(ff))
                {
                    FProperty* p = static_cast<FProperty*>(ff);
                    if (!(Rf::PropFlagsOf(p) & CPF_Parm)) continue;
                    bool ret = (Rf::PropFlagsOf(p) & CPF_ReturnParm) != 0;
                    bool outp = (Rf::PropFlagsOf(p) & CPF_OutParm) && !(Rf::PropFlagsOf(p) & CPF_ConstParm);
                    if (ret) rets = LuaType(p) + (rets.empty() ? "" : ", " + rets);
                    else if (outp) rets += (rets.empty() ? "" : ", ") + LuaType(p);
                    else
                    {
                        std::string pn = LuaIdent(Rf::NameStr(ff));
                        for (int k = 2; !pseen.insert(pn).second; k++) pn = LuaIdent(Rf::NameStr(ff)) + "_" + std::to_string(k);
                        params += ", " + pn + ": " + LuaType(p);
                    }
                }
                out += "---@field " + fname + " fun(self: " + name + params + ")" + (rets.empty() ? "" : ": " + rets) + "\n";
                nFunc++;
            }
        }
        out += "\n";
        (isClass ? nClass : nStruct)++;
        return false;
    });
    mkdir(outDir.c_str(), 0777);
    long nFiles = 0;
    for (auto& kv : files)
    {
        std::string path = outDir + "/" + kv.first + ".lua";
        if (FILE* f = fopen(path.c_str(), "wb"))
        {
            std::string head = "---@meta\n-- Generated by kmod `typegen` from the running game's reflection data (" + kv.first + ").\n-- Do not edit; run `typegen` again after a game update.\n\n";
            fwrite(head.data(), 1, head.size(), f);
            fwrite(kv.second.data(), 1, kv.second.size(), f);
            fclose(f);
            nFiles++;
        }
    }
    lua_pushinteger(L, nFiles); lua_pushinteger(L, nClass); lua_pushinteger(L, nStruct); lua_pushinteger(L, nEnum); lua_pushinteger(L, nFunc);
    return 5;
}

// LoadAsset(path) -> UObject | nil, err.   Synchronously loads any asset (pak content included) by object path,
// e.g. "/Game/UI/Foo/WBP_Foo.WBP_Foo_C" (a Blueprint class) or "/Game/Art/T_Icon.T_Icon". A path without ".Name" gets
// the package's short name appended. Already-loaded assets are returned as is. Runs on the game thread.
int G_LoadAsset(lua_State* L)
{
    std::string path = luaL_checkstring(L, 1);
    if (path.empty() || path[0] != '/') { lua_pushnil(L); lua_pushstring(L, "asset path must start with '/'"); return 2; }
    if (path.find('.') == std::string::npos) { size_t s = path.rfind('/'); path += "." + path.substr(s + 1); }
    UObject* obj = nullptr;                                       // LoadAsset_Blocking returns at once for loaded assets
    {
        std::u16string u = Utf8To16(path);
        int32_t n = (int32_t)u.size() + 1;
        FString fs(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(u.c_str())), n, n);
        UObject* lib = Rf::CdoFor("KismetSystemLibrary");
        Rf::Args mk = Rf::Prepare("KismetSystemLibrary", "MakeSoftObjectPath");
        mk.Set("PathString", fs);
        mk.Call(lib);
        FSoftObjectPath sp = mk.Get<FSoftObjectPath>("ReturnValue");
        Rf::Args cv = Rf::Prepare("KismetSystemLibrary", "Conv_SoftObjPathToSoftObjRef");
        cv.Set("SoftObjectPath", sp);
        cv.Call(lib);
        TSoftObjectPtr<UObject> ref = cv.Get<TSoftObjectPtr<UObject>>("ReturnValue");
        Rf::Args ld = Rf::Prepare("KismetSystemLibrary", "LoadAsset_Blocking");
        ld.Set("Asset", ref);
        ld.Call(lib);
        obj = ld.Get<UObject*>("ReturnValue");
    }
    if (!obj) { lua_pushnil(L); lua_pushstring(L, ("could not load " + path).c_str()); return 2; }
    PushObject(L, obj);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Output (logcat + dev console capture)
// ---------------------------------------------------------------------------------------------
std::string g_consoleOut;
bool g_capture = false;

// DEV overlay bridge (exported by libkmod.so)
typedef int (*PopEvalFn)(char*, int);
typedef void (*AppendFn)(const char*);
PopEvalFn g_popEval = nullptr;
AppendFn g_overlayAppend = nullptr;

void ResolveOverlay() {}      // the dev console window (dev_console.cpp); the log lines reach it through kmod_log

// Output funnel: shared log file + logcat, the dev overlay console and the dev console capture.
void EmitTagged(KLogLevel level, const char* tag, const std::string& s)
{
    KLogMsg(level, tag, s);
    const std::string shown = (strcmp(tag, "lua") == 0) ? s : std::string("[") + tag + "] " + s;
    if (kDevBuild && g_overlayAppend) g_overlayAppend(shown.c_str());
    if (g_capture) { g_consoleOut += shown; g_consoleOut += "\n"; }
}

void EmitLine(const std::string& s)
{
    EmitTagged(s.compare(0, 7, "[error]") == 0 ? KLOG_ERROR : KLOG_INFO, "lua", s);
}

int G_Print(lua_State* L)
{
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++)
    {
        size_t len; const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += "\t";
        line.append(s, len);
        lua_pop(L, 1);
    }
    EmitLine(line);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Hooks, timers, queued work
// ---------------------------------------------------------------------------------------------
using NativeHookCb = void (*)(void*, void*, void*, void*);
struct HookEntry { int id; int preRef; int postRef; std::string owner; NativeHookCb npre = nullptr; NativeHookCb npost = nullptr; void* nuser = nullptr; };
std::unordered_map<UFunction*, std::vector<HookEntry>> g_hooks;
// HasHook runs for EVERY Blueprint function call (see the ProcessInternal detour): a bit filter answers "no" without a hash lookup.
uint64_t g_hookBloom[1024] = {};
inline uint32_t BloomBit(UFunction* fn) { uintptr_t p = reinterpret_cast<uintptr_t>(fn); return (uint32_t)((p >> 4) ^ (p >> 20)) & 0xFFFF; }
int g_nextHookId = 1;

struct Timer { double due; int fnRef; double repeatSec; bool loop; std::string owner; };
std::vector<Timer> g_timers;
double g_now = 0;

struct NewObjWatch { std::string cls; UClass* c; int fnRef; std::string owner; bool fast; };
bool g_anyFastWatch = false;                 // some watch asked for the fast scan (every ~16 ms instead of every 50 ms)
double g_nextFastScan = 0;

struct FastTimer { double due; int fnRef; double period; std::string owner; };
std::vector<FastTimer> g_fastTimers;
double FastNow() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
std::vector<NewObjWatch> g_newObjWatches;
int32_t g_lastObjCount = 0;
struct SlotSeen { UObject* p = nullptr; int32_t serial = 0; };
std::vector<SlotSeen> g_slotSeen;

// Per-mod wrappers carry the mod name as upvalue 1 so a mod's hooks/timers can be dropped when it is disabled.
std::string Owner(lua_State* L)
{
    return lua_type(L, lua_upvalueindex(1)) == LUA_TSTRING ? lua_tostring(L, lua_upvalueindex(1)) : "";
}

int G_RegisterHook(lua_State* L)
{
    std::string path = luaL_checkstring(L, 1);
    UFunction* fn = static_cast<UFunction*>(FindByPath(path, true));
    if (!fn) return luaL_error(L, "RegisterHook: function not found: %s", path.c_str());
    luaL_checktype(L, 2, LUA_TFUNCTION);
    {   // a native function: calls from Blueprint need a thunk (see KMod_EnsureNativeHook); say so when that is not possible
        std::string note;
        if (!KMod_EnsureNativeHook(fn, note) && !note.empty()) KLog(KLOG_WARN, Owner(L).empty() ? "core" : Owner(L).c_str(), "RegisterHook %s: %s", path.c_str(), note.c_str());
    }
    HookEntry e{g_nextHookId++, LUA_NOREF, LUA_NOREF, Owner(L)};
    lua_pushvalue(L, 2); e.preRef = luaL_ref(L, LUA_REGISTRYINDEX);
    if (lua_isfunction(L, 3)) { lua_pushvalue(L, 3); e.postRef = luaL_ref(L, LUA_REGISTRYINDEX); }
    g_hooks[fn].push_back(e);
    { uint32_t b = BloomBit(fn); g_hookBloom[b >> 6] |= 1ull << (b & 63); }
    lua_pushinteger(L, e.id);
    lua_pushinteger(L, e.id);
    return 2;
}

int G_UnregisterHook(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 2 <= lua_gettop(L) && lua_isinteger(L, 2) ? 2 : 1);
    for (auto& kv : g_hooks)
        for (size_t i = 0; i < kv.second.size(); i++)
            if (kv.second[i].id == id)
            {
                luaL_unref(L, LUA_REGISTRYINDEX, kv.second[i].preRef);
                luaL_unref(L, LUA_REGISTRYINDEX, kv.second[i].postRef);
                kv.second.erase(kv.second.begin() + i);
                return 0;
            }
    return 0;
}

int G_ExecuteWithDelay(lua_State* L)
{
    double ms = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    g_timers.push_back({g_now + ms / 1000.0, luaL_ref(L, LUA_REGISTRYINDEX), 0, false, Owner(L)});
    return 0;
}

int G_ExecuteInGameThread(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    g_timers.push_back({g_now, luaL_ref(L, LUA_REGISTRYINDEX), 0, false, Owner(L)});
    return 0;
}

int G_ExecuteAsync(lua_State* L)   // UE4SS name; kmod runs it on the game thread at the next tick (use Worker for real parallel work)
{
    return G_ExecuteInGameThread(L);
}

int G_LoopAsync(lua_State* L)   // LoopAsync(ms, fn): repeats until fn returns true
{
    double ms = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    g_timers.push_back({g_now + ms / 1000.0, luaL_ref(L, LUA_REGISTRYINDEX), ms / 1000.0, true, Owner(L)});
    return 0;
}

int G_NotifyOnNewObject(lua_State* L)
{
    std::string cn = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    bool fast = lua_toboolean(L, 3) != 0;                        // NotifyOnNewObject(class, fn, fast)
    g_newObjWatches.push_back({cn, nullptr, luaL_ref(L, LUA_REGISTRYINDEX), Owner(L), fast});
    if (fast) g_anyFastWatch = true;
    return 0;
}

// FastLoop(ms, fn): like LoopAsync, but fn runs every few milliseconds on the game thread (not only 20 times a second):
// use it for tiny checks that must happen before the next frame is drawn. ms is clamped to >= 2. Return true to stop.
int G_FastLoop(lua_State* L)
{
    double ms = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 2) ms = 2;
    lua_pushvalue(L, 2);
    g_fastTimers.push_back({FastNow() + ms / 1000.0, luaL_ref(L, LUA_REGISTRYINDEX), ms / 1000.0, Owner(L)});
    return 0;
}

int TraceHandler(lua_State* L)
{
    const char* m = lua_tostring(L, 1);
    luaL_traceback(L, L, m ? m : "(error object is not a string)", 1);
    return 1;
}

// Runtime errors raised by a mod's hooks/timers/watchers are reported to Lua (kmod._mod_error) so it can disable a mod that keeps failing.
void ReportModError(lua_State* L, const std::string& owner, const std::string& msg)
{
    if (owner.empty()) return;
    int top = lua_gettop(L);
    lua_getglobal(L, "kmod");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, "_mod_error");
        if (lua_isfunction(L, -1))
        {
            lua_pushstring(L, owner.c_str()); lua_pushstring(L, msg.c_str());
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
}

bool PCall(lua_State* L, int nargs, int nres, const char* what, const std::string& owner = std::string())
{
    int base = lua_gettop(L) - nargs;
    char savedActivity[112];
    KBoot_GetActivity(savedActivity, sizeof(savedActivity));
    if (!owner.empty()) KBoot_SetActivity("mod", owner.c_str(), what);      // a crash inside this call is blamed on the mod
    lua_pushcfunction(L, TraceHandler);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nres, base);
    lua_remove(L, base);
    KBoot_RestoreActivity(savedActivity);
    if (rc != LUA_OK)
    {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_pop(L, 1);
        EmitLine(std::string("[error] ") + what + ": " + msg);
        ReportModError(L, owner, msg);
        return false;
    }
    return true;
}

void RunHooks(UObject* self, UFunction* fn, void* parms, bool post, void* result = nullptr)
{
    auto it = g_hooks.find(fn);
    if (it == g_hooks.end() || !L_) return;
    std::vector<HookEntry> copy = it->second;   // callbacks may unregister
    for (const HookEntry& e : copy)
    {
        if (NativeHookCb ncb = post ? e.npost : e.npre) { ncb(self, parms, result, e.nuser); continue; }     // a native mod's hook
        int ref = post ? e.postRef : e.preRef;
        if (ref == LUA_NOREF || ref == LUA_REFNIL) continue;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
        PushObject(L_, self);
        LuaParams* lp = static_cast<LuaParams*>(lua_newuserdatauv(L_, sizeof(LuaParams), 0));
        lp->fn = fn; lp->parms = static_cast<uint8_t*>(parms); lp->result = static_cast<uint8_t*>(result);
        luaL_setmetatable(L_, MT_PARAMS);
        int top = lua_gettop(L_) - 3;                    // the function, the object and the params were pushed above
        if (!PCall(L_, 2, 1, post ? "post-hook" : "pre-hook", e.owner)) continue;
        // like UE4SS: a value returned by the callback replaces the function's return value (a post hook on a Blueprint function
        // changes what its caller receives only through the result pointer; a pre hook's value is overwritten when the function runs)
        if (!lua_isnil(L_, -1))
            for (FField* f = Rf::PropsOf(fn); f; f = Rf::NextOf(f))
            {
                FProperty* p = static_cast<FProperty*>(f);
                if (!(Rf::PropFlagsOf(p) & CPF_ReturnParm)) continue;
                uint8_t* addr = result ? static_cast<uint8_t*>(result) : static_cast<uint8_t*>(parms) + Rf::OffsetOf(p);
                if (TypeOf(p) == T_Str && lua_isstring(L_, -1))
                    *reinterpret_cast<FString*>(addr) = EngineString(lua_tostring(L_, -1));
                else
                {
                    std::deque<std::u16string> keep; std::string err;
                    if (!SetFromLua(L_, lua_gettop(L_), p, addr, false, keep, err)) EmitLine("[error] hook return value: " + err);
                }
                break;
            }
        lua_settop(L_, top);
    }
}

// ---------------------------------------------------------------------------------------------
// Directories: <KModRoot>/mods, <KModRoot>/dev (see KModRoot in kmod_common.h)
// ---------------------------------------------------------------------------------------------
std::string ModsDir() { return KModRoot() + "/mods"; }
std::string DevDir() { return KModRoot() + "/dev"; }

bool FileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

std::string ReadFileStr(const std::string& path)
{
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n); fclose(f); }
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);   // UTF-8 BOM
    return out;
}

// Calls the Lua loader: kmod.load_mods() (migration, inbox install, scan, dependency order, run enabled mods)
void CallLoadMods()
{
    lua_State* L = L_;
    lua_getglobal(L, "kmod");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, "load_mods");
        if (lua_isfunction(L, -1)) PCall(L, 0, 0, "load_mods");
        else { lua_pop(L, 1); EmitLine("[error] kmod.load_mods is not defined (modloader.lua failed to load?)"); }
    }
    lua_pop(L, 1);
}

// ---------------------------------------------------------------------------------------------
// Dev console: runs <root>/dev/console.lua whenever it changes, writes console.out
// ---------------------------------------------------------------------------------------------
time_t g_consoleMtime = 0;
double g_nextConsoleCheck = 0;

void CheckConsole()
{
    std::string path = DevDir() + "/console.lua";
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_mtime == g_consoleMtime) return;
    g_consoleMtime = st.st_mtime;
    std::string src = ReadFileStr(path);
    g_consoleOut.clear();
    g_capture = true;
    LuaHost_RunString(src, "console.lua");
    g_capture = false;
    if (FILE* f = fopen((DevDir() + "/console.out").c_str(), "wb")) { fwrite(g_consoleOut.data(), 1, g_consoleOut.size(), f); fclose(f); }
}


// ---------------------------------------------------------------------------------------------
// kmod.* native helpers (Lua has no directory API)
// ---------------------------------------------------------------------------------------------
int Ccb_ListDir(lua_State* L)   // kmod.listdir(path) -> { {name=, dir=}, ... }
{
    lua_newtable(L);
    int n = 0;
    if (DIR* d = opendir(luaL_checkstring(L, 1)))
    {
        std::string base = lua_tostring(L, 1);
        while (dirent* e = readdir(d))
        {
            if (e->d_name[0] == '.') continue;
            struct stat st;
            bool isDir = stat((base + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            lua_newtable(L);
            lua_pushstring(L, e->d_name); lua_setfield(L, -2, "name");
            lua_pushboolean(L, isDir); lua_setfield(L, -2, "dir");
            lua_rawseti(L, -2, ++n);
        }
        closedir(d);
    }
    return 1;
}
int Ccb_Exists(lua_State* L) { struct stat st; lua_pushboolean(L, stat(luaL_checkstring(L, 1), &st) == 0); return 1; }
int Ccb_Mkdir(lua_State* L) { lua_pushboolean(L, mkdir(luaL_checkstring(L, 1), 0777) == 0); return 1; }
int Kmod_Log(lua_State* L)   // kmod.log(level, tag, ...): level = "debug" | "info" | "warn" | "error"
{
    const char* lv = luaL_checkstring(L, 1);
    const char* tag = luaL_checkstring(L, 2);
    std::string msg;
    for (int i = 3; i <= lua_gettop(L); i++)
    {
        size_t len; const char* str = luaL_tolstring(L, i, &len);
        if (i > 3) msg += "\t";
        msg.append(str, len);
        lua_pop(L, 1);
    }
    KLogLevel level = strcmp(lv, "debug") == 0 ? KLOG_DEBUG : strcmp(lv, "warn") == 0 ? KLOG_WARN : strcmp(lv, "error") == 0 ? KLOG_ERROR : KLOG_INFO;
    EmitTagged(level, tag, msg);
    return 0;
}
#ifdef KMOD_DEV
// DEV only: crash the process on purpose to test the crash logging (kmod.debug_crash("segv" | "abort"))
int Kmod_DebugCrash(lua_State* L)
{
    const char* kind = luaL_optstring(L, 1, "segv");
    KLog(KLOG_WARN, "debug", "kmod.debug_crash('%s') requested - crashing on purpose", kind);
    if (strcmp(kind, "abort") == 0) abort();
    volatile int* p = nullptr;
    *p = 42;                                   // SIGSEGV
    return 0;
}
#endif
int Ccb_ReloadMods(lua_State*) { LuaHost_ReloadMods(); return 0; }
// ---------------------------------------------------------------------------------------------
// Filesystem helpers for the Lua loader: unzip (.kmod), recursive delete, owner-tagged mod API
// ---------------------------------------------------------------------------------------------
bool MkdirAll(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint32_t Rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t Rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool SafeZipName(const std::string& n)
{
    if (n.empty() || n[0] == '/' || n.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= n.size())
    {
        size_t j = n.find('/', i);
        if (j == std::string::npos) j = n.size();
        if (n.compare(i, j - i, "..") == 0) return false;     // no path traversal
        i = j + 1;
    }
    return true;
}

// minimal zip reader: stored + deflate, size limits, path-traversal safe
bool UnzipTo(const std::string& zipPath, const std::string& dest, std::string& err)
{
    std::string blob = ReadFileStr(zipPath);
    if (blob.size() < 22 || blob.size() > (128u << 20)) { err = "bad zip size"; return false; }
    const unsigned char* d = reinterpret_cast<const unsigned char*>(blob.data());
    const size_t n = blob.size();
    size_t eocd = std::string::npos;
    for (size_t back = 22; back <= std::min<size_t>(n, 65557); back++)
        if (Rd32(d + (n - back)) == 0x06054b50) { eocd = n - back; break; }
    if (eocd == std::string::npos) { err = "not a zip file"; return false; }
    const uint16_t count = Rd16(d + eocd + 10);
    size_t p = Rd32(d + eocd + 16);
    size_t total = 0;
    for (uint16_t k = 0; k < count; k++)
    {
        if (p + 46 > n || Rd32(d + p) != 0x02014b50) { err = "corrupt central directory"; return false; }
        const uint16_t method = Rd16(d + p + 10);
        const uint32_t csize = Rd32(d + p + 20), usize = Rd32(d + p + 24);
        const uint16_t nlen = Rd16(d + p + 28), elen = Rd16(d + p + 30), clen = Rd16(d + p + 32);
        const uint32_t lho = Rd32(d + p + 42);
        if (p + 46 + nlen > n) { err = "corrupt entry name"; return false; }
        std::string name(reinterpret_cast<const char*>(d + p + 46), nlen);
        p += 46 + nlen + elen + clen;
        for (char& c : name) if (c == '\\') c = '/';
        if (!SafeZipName(name)) { err = "unsafe path in zip: " + name; return false; }
        const std::string full = dest + "/" + name;
        if (name.back() == '/') { MkdirAll(full); continue; }
        if (usize > (32u << 20)) { err = "entry too large: " + name; return false; }
        total += usize;
        if (total > (128u << 20)) { err = "archive too large"; return false; }
        if ((size_t)lho + 30 > n || Rd32(d + lho) != 0x04034b50) { err = "corrupt local header"; return false; }
        const size_t dataOff = (size_t)lho + 30 + Rd16(d + lho + 26) + Rd16(d + lho + 28);
        if (dataOff + csize > n) { err = "truncated entry: " + name; return false; }
        std::string out;
        if (method == 0) out.assign(reinterpret_cast<const char*>(d + dataOff), csize);
        else if (method == 8)
        {
            out.resize(usize);
            z_stream zs{};
            zs.next_in = const_cast<Bytef*>(d + dataOff); zs.avail_in = csize;
            zs.next_out = reinterpret_cast<Bytef*>(&out[0]); zs.avail_out = usize;
            if (inflateInit2(&zs, -15) != Z_OK) { err = "inflateInit failed"; return false; }
            int r = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (r != Z_STREAM_END || zs.total_out != usize) { err = "inflate failed: " + name; return false; }
        }
        else { err = "unsupported compression method in " + name; return false; }
        MkdirAll(full.substr(0, full.rfind('/')));
        FILE* f = fopen(full.c_str(), "wb");
        if (!f) { err = "cannot write " + full; return false; }
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
    return true;
}

void RmTree(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode))
    {
        if (DIR* d = opendir(path.c_str()))
        {
            while (dirent* e = readdir(d))
            {
                std::string nm = e->d_name;
                if (nm == "." || nm == "..") continue;
                RmTree(path + "/" + nm);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    }
    else unlink(path.c_str());
}

int Ccb_Unzip(lua_State* L)    // ok, err = kmod.unzip(zipPath, destDir)
{
    std::string err;
    std::string dest = luaL_checkstring(L, 2);
    MkdirAll(dest);
    if (UnzipTo(luaL_checkstring(L, 1), dest, err)) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0); lua_pushstring(L, err.c_str());
    return 2;
}

int Ccb_Rmtree(lua_State* L)   // only inside <root>/
{
    std::string path = luaL_checkstring(L, 1);
    std::string root = KModRoot() + "/", priv = KPrivateRoot() + "/";
    if ((path.compare(0, root.size(), root) != 0 && path.compare(0, priv.size(), priv) != 0) || path.find("..") != std::string::npos)
        return luaL_error(L, "rmtree: refusing path %s", path.c_str());
    RmTree(path);
    return 0;
}

// kmod.copytree(src, dst) -> true | false, err: copies a directory tree (dst is created; files are overwritten)
bool CopyTreeImpl(const std::string& src, const std::string& dst, std::string& err)
{
    struct stat st;
    if (stat(src.c_str(), &st) != 0) { err = "missing " + src; return false; }
    if (S_ISDIR(st.st_mode))
    {
        MkdirAll(dst);
        DIR* d = opendir(src.c_str());
        if (!d) { err = "cannot read " + src; return false; }
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") names.push_back(n); }
        closedir(d);
        for (auto& n : names) if (!CopyTreeImpl(src + "/" + n, dst + "/" + n, err)) return false;
        return true;
    }
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) { err = "cannot read " + src; return false; }
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); err = "cannot write " + dst; return false; }
    char buf[1 << 16]; size_t n; bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) err = "copy failed (storage full?): " + dst;
    return ok;
}

int Ccb_CopyTree(lua_State* L)
{
    std::string src = luaL_checkstring(L, 1), dst = luaL_checkstring(L, 2), err;
    std::string priv = KPrivateRoot() + "/", root = KModRoot() + "/";
    if ((dst.compare(0, priv.size(), priv) != 0 && dst.compare(0, root.size(), root) != 0) || dst.find("..") != std::string::npos)
        return luaL_error(L, "copytree: refusing destination %s", dst.c_str());
    if (CopyTreeImpl(src, dst, err)) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0); lua_pushstring(L, err.c_str());
    return 2;
}

// kmod.tree_stamp(path) -> string that changes when a file or anything under a directory changes (names, sizes, modification times)
void StampWalk(const std::string& path, const std::string& rel, uint64_t& h, int depth)
{
    auto mix = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= 0xFF; h *= 1099511628211ull; };
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    mix(rel); mix(std::to_string((long long)st.st_size)); mix(std::to_string((long long)st.st_mtime));
    if (!S_ISDIR(st.st_mode) || depth > 16) return;
    DIR* d = opendir(path.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") names.push_back(n); }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) StampWalk(path + "/" + n, rel + "/" + n, h, depth + 1);
}

int Ccb_TreeStamp(lua_State* L)
{
    uint64_t h = 1469598103934665603ull;
    StampWalk(luaL_checkstring(L, 1), "", h, 0);
    char b[32]; snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    lua_pushstring(L, b);
    return 1;
}

// kmod.boot_info() -> { safe=bool, crashes=n, last_crash=str, mods={id=true}, paks={name=true} }   (see kmod_boot.h)
int Boot_Info(lua_State* L)
{
    lua_newtable(L);
    lua_pushboolean(L, KBoot_SafeMode()); lua_setfield(L, -2, "safe");
    lua_pushinteger(L, KBoot_CrashCount()); lua_setfield(L, -2, "crashes");
    lua_pushboolean(L, KMod_LayoutMismatch()); lua_setfield(L, -2, "layout_mismatch");
    lua_pushstring(L, KBoot_LastCrash().c_str()); lua_setfield(L, -2, "last_crash");
    for (const char* kind : {"mod", "pak"})
    {
        lua_newtable(L);
        for (auto& n : KBoot_QuarantineList(kind)) { lua_pushboolean(L, 1); lua_setfield(L, -2, n.c_str()); }
        lua_setfield(L, -2, std::string(kind) == "mod" ? "mods" : "paks");
    }
    return 1;
}
int Pak_Status(lua_State* L)       // kmod.pak_status() -> { {name=, status=}, ... }
{
    lua_newtable(L);
    int i = 0;
    for (auto& p : KMod_PakStatus())
    {
        lua_newtable(L);
        lua_pushstring(L, p.first.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, p.second.c_str()); lua_setfield(L, -2, "status");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}
int Pak_MountPending(lua_State*) { KMod_MountPendingPaks(); return 0; }
int Pak_Off(lua_State* L)         // kmod.pak_off(name, off): switch a pak off (it is not mounted from the next start on) or back on
{
    std::string name = luaL_checkstring(L, 1);
    if (lua_toboolean(L, 2)) KBoot_Add("off", name); else KBoot_Unquarantine("off", name);
    return 0;
}
// kmod.script_hook_stats() -> installed, script-function calls seen by the ProcessInternal detour, of which came from ProcessEvent
int Kmod_ScriptHookStats(lua_State* S)
{
    unsigned long long pi = 0, pe = 0; bool on = false;
    KMod_ScriptStats(pi, pe, on);
    lua_pushboolean(S, on); lua_pushnumber(S, (double)pi); lua_pushnumber(S, (double)pe);
    return 3;
}
// kmod.native_hook_stats() -> opcode table found, hooked native functions, Blueprint calls intercepted, FFrame chain offset (-1: none)
int Kmod_NativeHookStats(lua_State* S)
{
    unsigned long long calls = 0; int slots = 0, chain = -1; bool table = false;
    KMod_NativeStats(calls, slots, table, chain);
    lua_pushboolean(S, table); lua_pushinteger(S, slots); lua_pushnumber(S, (double)calls); lua_pushinteger(S, chain);
    return 4;
}
// kmod.script_diag(true) starts collecting; kmod.script_diag(false) returns { "path", ... } of functions only the Blueprint VM called
int Kmod_ScriptDiag(lua_State* S)
{
    bool start = lua_toboolean(S, 1) != 0;
    auto v = KMod_ScriptDiag(start);
    lua_newtable(S);
    int i = 1;
    for (auto& s : v) { lua_pushstring(S, s.c_str()); lua_rawseti(S, -2, i++); }
    return 1;
}
int Boot_SafeOff(lua_State*) { KBoot_SafeOff(); return 0; }
int Boot_Unquarantine(lua_State* L) { KBoot_Unquarantine(luaL_checkstring(L, 1), luaL_checkstring(L, 2)); return 0; }
int Boot_SetActivity(lua_State* L)      // kmod.set_activity("mod", id, what) / kmod.set_activity() to clear
{
    if (lua_isnoneornil(L, 1)) KBoot_SetActivity(nullptr, nullptr, nullptr);
    else KBoot_SetActivity(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), luaL_optstring(L, 3, ""));
    return 0;
}

int Ccb_ModApi(lua_State* L)   // kmod.mod_api(modId) -> owner-tagged RegisterHook / ExecuteWithDelay / ... for that mod's environment
{
    const char* name = luaL_checkstring(L, 1);
    lua_newtable(L);
    static const struct { const char* n; lua_CFunction f; } reg[] = {
        {"RegisterHook", G_RegisterHook}, {"ExecuteWithDelay", G_ExecuteWithDelay}, {"ExecuteInGameThread", G_ExecuteInGameThread},
        {"LoopAsync", G_LoopAsync}, {"NotifyOnNewObject", G_NotifyOnNewObject}, {"FastLoop", G_FastLoop}, {"ExecuteAsync", G_ExecuteAsync}};
    for (auto& r : reg) { lua_pushstring(L, name); lua_pushcclosure(L, r.f, 1); lua_setfield(L, -2, r.n); }
    return 1;
}



// REPL used by the overlay console: try as an expression first (prints its value), else run as statements.
void RunRepl(const std::string& code)
{
    lua_State* L = L_;
    {   // console commands (kmod.console_exec in console.lua) take precedence over plain Lua
        lua_getglobal(L, "kmod");
        bool handled = false;
        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "console_exec");
            if (lua_isfunction(L, -1))
            {
                lua_pushlstring(L, code.data(), code.size());
                if (lua_pcall(L, 1, 1, 0) == LUA_OK) { handled = lua_toboolean(L, -1) != 0; lua_pop(L, 1); }
                else { EmitLine(std::string("[error] ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?")); lua_pop(L, 1); handled = true; }
            }
            else lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (handled) return;
    }
    std::string expr = "return " + code;
    if (luaL_loadbuffer(L, expr.data(), expr.size(), "=console") != LUA_OK)
    {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code.data(), code.size(), "=console") != LUA_OK)
        {
            EmitLine(std::string("[error] ") + lua_tostring(L, -1));
            lua_pop(L, 1);
            return;
        }
    }
    int base = lua_gettop(L) - 1;
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK)
    {
        EmitLine(std::string("[error] ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
        lua_settop(L, base);
        return;
    }
    int n = lua_gettop(L) - base;
    if (n > 0)
    {
        std::string line = "=";
        for (int i = 1; i <= n; i++)
        {
            size_t len; const char* str = luaL_tolstring(L, base + i, &len);
            line += (i > 1 ? "	" : " ");
            line.append(str, len);
            lua_pop(L, 1);
        }
        EmitLine(line);
    }
    lua_settop(L, base);
}

const char* kPrelude = R"LUA(
kmod = { dev = ... }
-- Generic widget tree dumper (debug): kmod.tree(widget, maxDepth)
function kmod.tree(w, maxDepth, depth, out)
  depth = depth or 0; maxDepth = maxDepth or 6; out = out or {}
  if not w or not w:IsValid() then return out end
  local geo = nil
  local ok, g = pcall(function() return w:GetCachedGeometry() end)
  if ok and g then geo = g end
  local info = string.format("%s%s [%s]", string.rep("  ", depth), w:GetName(), w:GetClass():GetName())
  if geo and geo.Size then
    local ok2, s = pcall(function() return string.format(" size=(%.0f,%.0f) abs=(%.0f,%.0f)", geo.Size.X, geo.Size.Y, geo.AbsolutePosition.X, geo.AbsolutePosition.Y) end)
    if ok2 then info = info .. s end
  end
  out[#out+1] = info
  if depth < maxDepth then
    local okc, n = pcall(function() return w:GetChildrenCount() end)
    if okc and n and n > 0 then
      for i = 0, n - 1 do kmod.tree(w:GetChildAt(i), maxDepth, depth + 1, out) end
    else
      local okt, tree = pcall(function() return w.WidgetTree end)
      if okt and tree and tree:IsValid() then
        local okr, root = pcall(function() return tree.RootWidget end)
        if okr and root and root:IsValid() then kmod.tree(root, maxDepth, depth + 1, out) end
      end
      local okcnt, content = pcall(function() return w:GetContent() end)
      if okcnt and content and content:IsValid() then kmod.tree(content, maxDepth, depth + 1, out) end
    end
  end
  return out
end
function kmod.print_tree(w, maxDepth) for _, l in ipairs(kmod.tree(w, maxDepth)) do print(l) end end
)LUA";

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
void LuaHost_RunString(const std::string& code, const char* chunkName)
{
    if (!L_) return;
    if (luaL_loadbuffer(L_, code.data(), code.size(), chunkName) != LUA_OK)
    {
        EmitLine(std::string("[error] ") + chunkName + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return;
    }
    PCall(L_, 0, 0, chunkName);
}

void LuaHost_Init()
{
    if (L_) return;
    g_mainTid = gettid();
    if (kDevBuild) ResolveOverlay();
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    lua_State* L = L_;

    // UObject metatable
    luaL_newmetatable(L, MT_OBJECT);
    lua_pushcfunction(L, Obj_Index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Obj_NewIndex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Obj_ToString); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Obj_Eq); lua_setfield(L, -2, "__eq");
    lua_newtable(L); luaL_setfuncs(L, kObjectMethods, 0); lua_setfield(L, -2, "__methods");
    lua_pop(L, 1);

    luaL_newmetatable(L, MT_DELEGATE);
    lua_pushcfunction(L, Delegate_Index); lua_setfield(L, -2, "__index");
    lua_newtable(L); luaL_setfuncs(L, kDelegateMethods, 0); lua_setfield(L, -2, "__methods");
    lua_pop(L, 1);

    luaL_newmetatable(L, MT_PARAMS);
    lua_pushcfunction(L, Params_Index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Params_NewIndex); lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);

    static const luaL_Reg globals[] = {
        {"print", G_Print}, {"FindFirstOf", G_FindFirstOf}, {"FindAllOf", G_FindAllOf},
        {"StaticFindObject", G_StaticFindObject}, {"FindObject", G_FindObject}, {"CreateInvalidObject", G_CreateInvalidObject},
        {"LoadAsset", G_LoadAsset},
        {"ForEachUObject", G_ForEachUObject}, {"FindObjects", G_FindObjects}, {"StaticConstructObject", G_StaticConstructObject},
        {"IsInGameThread", G_IsInGameThread}, {"IsInMainModThread", G_IsInGameThread}, {"GetGameThreadId", G_GetGameThreadId}, {"GetMainModThreadId", G_GetGameThreadId},
        {"GetCurrentThreadId", G_GetCurrentThreadId}, {"IsInAsyncThread", G_IsInAsyncThread},
        {"RegisterCustomProperty", G_RegisterCustomProperty}, {"LoadExport", G_LoadExport},
        {"DumpAllObjects", G_DumpAllObjects}, {"DumpAllActors", G_DumpAllActors}, {"FString", G_FStringCtor}, {"FAnsiString", G_FStringCtor},
        {"RegisterHook", G_RegisterHook}, {"UnregisterHook", G_UnregisterHook}, {"ExecuteWithDelay", G_ExecuteWithDelay},
        {"ExecuteInGameThread", G_ExecuteInGameThread}, {"LoopAsync", G_LoopAsync}, {"NotifyOnNewObject", G_NotifyOnNewObject}, {"FastLoop", G_FastLoop},
        {"ExecuteAsync", G_ExecuteAsync},
        {nullptr, nullptr}};
    lua_pushglobaltable(L);
    luaL_setfuncs(L, globals, 0);
    lua_pop(L, 1);

    // prelude gets `dev` as its vararg
    std::string prelude = kPrelude;
    if (luaL_loadbuffer(L, prelude.data(), prelude.size(), "=prelude") == LUA_OK)
    {
        lua_pushboolean(L, kDevBuild);
        PCall(L, 1, 0, "prelude");
    }
    else { EmitLine(std::string("[error] prelude: ") + lua_tostring(L, -1)); lua_pop(L, 1); }

    {   // shared JSON codec: kmod.json (main state) and the global `json` inside workers
        static const WorkerHooks wh = {
            [](const std::string& s) { EmitLine(s); },
            [](lua_State* S, int na, int nr, const char* what, const std::string& owner) { return PCall(S, na, nr, what, owner); },
            [](lua_State* S, const std::string& owner, const std::string& msg) { ReportModError(S, owner, msg); }};
        Worker_Init(L, wh, kJsonLua);
        Http_Init(wh);
        NetConn_Init(wh);
        if (luaL_loadbuffer(L, kJsonLua, strlen(kJsonLua), "=json") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK)
        {
            lua_getglobal(L, "kmod");
            lua_pushvalue(L, -2); lua_setfield(L, -2, "json");
            lua_pop(L, 2);
        }
        else { EmitLine(std::string("[error] json: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?")); lua_pop(L, 1); }
    }
    lua_getglobal(L, "kmod");
    lua_pushcfunction(L, Worker_Create); lua_setfield(L, -2, "worker");
    lua_pushcfunction(L, Http_Request); lua_setfield(L, -2, "http_request");
    lua_pushcfunction(L, Kmod_ScriptHookStats); lua_setfield(L, -2, "script_hook_stats");
    lua_pushcfunction(L, Kmod_ScriptDiag); lua_setfield(L, -2, "script_diag");
    lua_pushcfunction(L, Kmod_NativeHookStats); lua_setfield(L, -2, "native_hook_stats");
    lua_pushcfunction(L, Native_Load); lua_setfield(L, -2, "native_load");
    lua_pushcfunction(L, Native_Prune); lua_setfield(L, -2, "native_prune");
    lua_pushcfunction(L, Native_IsLoaded); lua_setfield(L, -2, "native_loaded");
    lua_pushcfunction(L, [](lua_State* S) -> int { lua_pushnumber(S, FastNow()); return 1; }); lua_setfield(L, -2, "now");
    lua_pushcfunction(L, [](lua_State* S) -> int { lua_pushboolean(S, KMod_DetectLayout(lua_toboolean(S, 1) != 0)); return 1; }); lua_setfield(L, -2, "detect_layout");
    lua_pushcfunction(L, [](lua_State* S) -> int { lua_pushstring(S, KMod_LayoutReport().c_str()); return 1; }); lua_setfield(L, -2, "layout_report");
    lua_pushcfunction(L, NetConn_Open); lua_setfield(L, -2, "net_open");
    lua_pushcfunction(L, NetConn_Send); lua_setfield(L, -2, "net_send");
    lua_pushcfunction(L, NetConn_Close); lua_setfield(L, -2, "net_close");
    lua_pushcfunction(L, [](lua_State* S) -> int { lua_pushstring(S, NetRedirect_Status().c_str()); return 1; }); lua_setfield(L, -2, "net_redirect_status");
    lua_pushcfunction(L, Boot_Info); lua_setfield(L, -2, "boot_info");
    lua_pushcfunction(L, Boot_SafeOff); lua_setfield(L, -2, "safe_off");
    lua_pushcfunction(L, Pak_Status); lua_setfield(L, -2, "pak_status");
    lua_pushcfunction(L, Pak_MountPending); lua_setfield(L, -2, "mount_pending_paks");
    lua_pushcfunction(L, Pak_Off); lua_setfield(L, -2, "pak_off");
    lua_pushcfunction(L, Boot_Unquarantine); lua_setfield(L, -2, "unquarantine");
    lua_pushcfunction(L, Boot_SetActivity); lua_setfield(L, -2, "set_activity");
    lua_pushcfunction(L, [](lua_State* S) -> int { lua_pushboolean(S, KMod_MountPak(luaL_checkstring(S, 1))); return 1; });
    lua_setfield(L, -2, "mount_pak");
    lua_pushcfunction(L, Ccb_ListDir); lua_setfield(L, -2, "listdir");
    lua_pushcfunction(L, Ccb_Exists); lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, Ccb_Mkdir); lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, Ccb_ReloadMods); lua_setfield(L, -2, "reload_mods");
    lua_pushcfunction(L, Ccb_Unzip); lua_setfield(L, -2, "unzip");
    lua_pushcfunction(L, Ccb_Rmtree); lua_setfield(L, -2, "rmtree");
    lua_pushcfunction(L, Ccb_CopyTree); lua_setfield(L, -2, "copytree");
    lua_pushcfunction(L, Ccb_TreeStamp); lua_setfield(L, -2, "tree_stamp");
    lua_pushcfunction(L, Kmod_Perf); lua_setfield(L, -2, "perf");
    lua_pushcfunction(L, Kmod_PerfSample); lua_setfield(L, -2, "perf_sample");
    lua_pushcfunction(L, Ccb_ModApi); lua_setfield(L, -2, "mod_api");
    lua_pushcfunction(L, Kmod_Log); lua_setfield(L, -2, "log");
    lua_pushcfunction(L, Kmod_Signatures); lua_setfield(L, -2, "signatures");
    lua_pushcfunction(L, Kmod_FuncParams); lua_setfield(L, -2, "func_params");
    lua_pushcfunction(L, Kmod_Search); lua_setfield(L, -2, "search");
    lua_pushcfunction(L, Kmod_DumpTypes); lua_setfield(L, -2, "dump_types");
#ifdef KMOD_DEV
    lua_pushcfunction(L, Kmod_DebugCrash); lua_setfield(L, -2, "debug_crash");
#endif
    lua_pushstring(L, KMOD_VERSION); lua_setfield(L, -2, "version");
    lua_pushstring(L, KMOD_FLAVOR); lua_setfield(L, -2, "flavor");
    lua_pushinteger(L, KMOD_API_LEVEL); lua_setfield(L, -2, "api");
    lua_newtable(L);
    lua_pushstring(L, ExternalFilesDir().c_str()); lua_setfield(L, -2, "files");        // legacy app files dir
    lua_pushstring(L, KModRoot().c_str()); lua_setfield(L, -2, "root");
    lua_pushstring(L, ModsDir().c_str()); lua_setfield(L, -2, "mods");                 // kmod/mods: where users drop mods (shared storage)
    lua_pushstring(L, KPrivateRoot().c_str()); lua_setfield(L, -2, "private");          // the app's private kmod folder: mods are loaded from <private>/mods
    lua_pushstring(L, DevDir().c_str()); lua_setfield(L, -2, "dev");
    lua_pushstring(L, KLogDir().c_str()); lua_setfield(L, -2, "logs");
    lua_setfield(L, -2, "paths");
    lua_pushstring(L, "Windows"); lua_setfield(L, -2, "platform");
    lua_pushstring(L, "windows-x64"); lua_setfield(L, -2, "native_dir");
    lua_pop(L, 1);
    mkdir(KModRoot().c_str(), 0777);
    mkdir(ModsDir().c_str(), 0777);
    if (kDevBuild) mkdir(DevDir().c_str(), 0777);

    // built-in scripts (owner ""): the mod loader, then the mod manager UI. A dev build lets <dev>/modloader.lua and
    // <dev>/modmanager.lua override the embedded copies.
    {
        struct { const char* file; const char* embedded; const char* chunk; } builtins[] = {
            {"modloader.lua", kModLoaderLua, "=modloader"}, {"commands.lua", kCommandsLua, "=commands"}, {"ui.lua", kUiLua, "=ui"}, {"net.lua", kNetLua, "=net"}, {"ue4ss.lua", kUe4ssLua, "=ue4ss"}, {"modmanager.lua", kModManagerLua, "=modmanager"}};
        for (auto& b : builtins)
        {
            std::string code = b.embedded;
            if (kDevBuild) { std::string o = ReadFileStr(DevDir() + "/" + b.file); if (!o.empty()) { code = o; EmitLine(std::string("[builtin] using dev override ") + b.file); } }
            LuaHost_RunString(code, b.chunk);
        }
    }

    {   // everything that exists now counts as "seen": NotifyOnNewObject only reports objects created from here on
        TUObjectArray* a = InSDKUtils::GetGObjects();
        int32_t cnt = a->Num();
        g_lastObjCount = cnt;
        g_slotSeen.assign(cnt, SlotSeen{});
        for (int32_t i = 0; i < cnt; i++)
        {
            FUObjectItem* it = ObjRef::ItemAt(i);
            if (it) { g_slotSeen[i].p = it->Object; g_slotSeen[i].serial = SlotSerial(it); }
        }
    }
    EmitLine(std::string("kmod ") + KMOD_VERSION + " (" + KMOD_FLAVOR + "): Lua host ready (" + LUA_VERSION + ")");
    KLogInstallCrashHandlers();
    CallLoadMods();
}

void LuaHost_ReloadMods()
{
    // drop everything registered by user mods (owner != ""); built-in scripts (owner "") stay
    if (!L_) return;
    Worker_CloseUser(L_);
    Http_CancelUser(L_);
    NetConn_CloseUser(L_);
    for (auto& kv : g_hooks)
    {
        auto& v = kv.second;
        for (size_t i = 0; i < v.size();)
            if (!v[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, v[i].preRef); luaL_unref(L_, LUA_REGISTRYINDEX, v[i].postRef); v.erase(v.begin() + i); }
            else i++;
    }
    for (size_t i = 0; i < g_timers.size();)
        if (!g_timers[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, g_timers[i].fnRef); g_timers.erase(g_timers.begin() + i); } else i++;
    for (size_t i = 0; i < g_newObjWatches.size();)
        if (!g_newObjWatches[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, g_newObjWatches[i].fnRef); g_newObjWatches.erase(g_newObjWatches.begin() + i); } else i++;
    for (size_t i = 0; i < g_fastTimers.size();)
        if (!g_fastTimers[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, g_fastTimers[i].fnRef); g_fastTimers.erase(g_fastTimers.begin() + i); } else i++;
    g_anyFastWatch = false;
    for (auto& w : g_newObjWatches) if (w.fast) g_anyFastWatch = true;
    EmitLine("[mod] reloading mods");
    CallLoadMods();
}

bool LuaHost_HasHook(UFunction* fn)
{
    uint32_t b = BloomBit(fn);
    return (g_hookBloom[b >> 6] >> (b & 63) & 1) && g_hooks.count(fn) != 0;
}
void LuaHost_PreHook(UObject* self, UFunction* fn, void* parms, void* result) { RunHooks(self, fn, parms, false, result); }
void LuaHost_PostHook(UObject* self, UFunction* fn, void* parms, void* result) { RunHooks(self, fn, parms, true, result); }

// New-object watches: compare every GObjects slot with what we saw last time. The engine REUSES freed slots, so a
// new object is not only "a slot past the old end": its (pointer, serial number) pair has to differ from before.
static void ScanNewObjects()
{
    if (g_newObjWatches.empty() || !L_) return;
    TUObjectArray* a = InSDKUtils::GetGObjects();
    int32_t cnt = a->Num();
    if ((int32_t)g_slotSeen.size() < cnt) g_slotSeen.resize(cnt);
    for (int32_t i = 0; i < cnt; i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        int32_t sn = it ? SlotSerial(it) : 0;
        SlotSeen& seen = g_slotSeen[i];
        if (o == seen.p && sn == seen.serial) continue;
        if (o && (!ObjRef::ItemUsable(it) || !Rf::ClassOf(o))) continue;      // not fully set up yet: look again next time
        seen.p = o; seen.serial = sn;
        if (!o) continue;
        for (size_t wi = 0; wi < g_newObjWatches.size(); wi++)          // by index: a callback may register more watches
        {
            NewObjWatch& w = g_newObjWatches[wi];
            if (!w.c) w.c = FindClassCached(w.cls);
            if (w.c && !IsCDO(o) && Rf::IsA(o, w.c))
            {
                lua_rawgeti(L_, LUA_REGISTRYINDEX, w.fnRef);
                PushObject(L_, o);
                PCall(L_, 1, 0, "NotifyOnNewObject", w.owner);
            }
        }
    }
    g_lastObjCount = cnt;
}

// Fast lane, called from the ProcessEvent hook every few milliseconds (see core.cpp): FastLoop timers and, when a watch
// asked for it, the new-object scan at ~60 Hz.
bool LuaHost_HasFast() { return !g_fastTimers.empty() || g_anyFastWatch; }
void LuaHost_FastTick(double now)
{
    if (!L_) return;
    if (!g_fastTimers.empty())
    {
        std::vector<FastTimer> due, keep;
        for (auto& t : g_fastTimers) (t.due <= now ? due : keep).push_back(t);
        g_fastTimers = keep;
        for (auto& t : due)
        {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, t.fnRef);
            bool stop = true;
            if (PCall(L_, 0, 1, "fast loop", t.owner)) { stop = lua_toboolean(L_, -1) != 0; lua_pop(L_, 1); }
            if (!stop) { FastTimer n = t; n.due = now + t.period; g_fastTimers.push_back(n); }
            else luaL_unref(L_, LUA_REGISTRYINDEX, t.fnRef);
        }
    }
    if (g_anyFastWatch && now >= g_nextFastScan) { g_nextFastScan = now + 0.016; ScanNewObjects(); }
}

static void RunNativeTicks(double now);

void LuaHost_Tick(double now)
{
    if (!L_) return;
    g_now = now;
    RunNativeTicks(now);

    // due timers (copy: callbacks may add timers)
    std::vector<Timer> due, keep;
    for (auto& t : g_timers) (t.due <= now ? due : keep).push_back(t);
    g_timers = keep;
    for (auto& t : due)
    {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, t.fnRef);
        bool done = true;
        if (PCall(L_, 0, 1, "timer", t.owner)) { done = !t.loop || lua_toboolean(L_, -1); lua_pop(L_, 1); }
        if (t.loop && !done) { Timer n = t; n.due = now + t.repeatSec; g_timers.push_back(n); }
        else luaL_unref(L_, LUA_REGISTRYINDEX, t.fnRef);
    }

    Worker_Pump(L_);
    Http_Pump(L_);
    NetConn_Pump(L_);

    if (!g_anyFastWatch) ScanNewObjects();        // with a fast watch the scan runs from LuaHost_FastTick instead

    {   // lines typed in the dev console window or the overlay's console run here, on the game thread
        static char evalBuf[65536];
        int n;
        while ((n = CmdQueue_Pop(evalBuf, (int)sizeof(evalBuf))) >= 0) RunRepl(std::string(evalBuf, n));
    }
    if (Overlay_Visible())
    {   // the mods list the overlay shows: refreshed about once a second while it is open
        static double nextSnap = 0;
        if (now >= nextSnap)
        {
            nextSnap = now + 1.0;
            lua_getglobal(L_, "kmod");
            if (lua_istable(L_, -1))
            {
                lua_getfield(L_, -1, "overlay_snapshot");
                if (lua_isfunction(L_, -1) && lua_pcall(L_, 0, 1, 0) == LUA_OK) { if (lua_isstring(L_, -1)) Overlay_SetSnapshot(lua_tostring(L_, -1)); lua_pop(L_, 1); }
                else lua_pop(L_, 1);
            }
            lua_pop(L_, 1);
        }
    }
    if (kDevBuild && now >= g_nextConsoleCheck) { g_nextConsoleCheck = now + 0.5; CheckConsole(); }
}


// ---------------------------------------------------------------------------------------------
// Services for native mods (native_mods.cpp). Everything here runs on the game thread.
// ---------------------------------------------------------------------------------------------
void* KMod_FindFunctionByPath(const char* path) { return FindByPath(path, true); }
void* KMod_FindClassByName(const char* name) { return FindClassCached(name); }
void* KMod_FindFirstInstance(const char* cls) { return Rf::FindFirst(cls); }

bool KMod_AddNativeHook(void* fn, void (*pre)(void*, void*, void*, void*), void (*post)(void*, void*, void*, void*), void* user)
{
    UFunction* f = static_cast<UFunction*>(fn);
    if (!f) return false;
    std::string note;
    KMod_EnsureNativeHook(f, note);
    HookEntry e{g_nextHookId++, LUA_NOREF, LUA_NOREF, std::string()};
    e.npre = pre; e.npost = post; e.nuser = user;
    g_hooks[f].push_back(e);                                   // owner "": a mod reload does not remove it
    uint32_t b = BloomBit(f); g_hookBloom[b >> 6] |= 1ull << (b & 63);
    return true;
}

static std::vector<std::pair<void (*)(double, void*), void*>> g_nativeTicks;
void KMod_AddNativeTick(void (*fn)(double, void*), void* user) { g_nativeTicks.emplace_back(fn, user); }
static void RunNativeTicks(double now) { for (auto& t : g_nativeTicks) t.first(now, t.second); NativeUi_Drain(); }

// Native Lua function of a mod: kmod._natives[modId][name]
void KMod_RegisterNativeLua(const char* modId, const char* name, int (*fn)(lua_State*))
{
    if (!L_) return;
    int top = lua_gettop(L_);
    lua_getglobal(L_, "kmod");
    lua_getfield(L_, -1, "_natives");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); lua_newtable(L_); lua_pushvalue(L_, -1); lua_setfield(L_, -3, "_natives"); }
    lua_getfield(L_, -1, modId);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); lua_newtable(L_); lua_pushvalue(L_, -1); lua_setfield(L_, -3, modId); }
    lua_pushcfunction(L_, fn); lua_setfield(L_, -2, name);
    lua_settop(L_, top);
}
lua_State* KMod_LuaState() { return L_; }
