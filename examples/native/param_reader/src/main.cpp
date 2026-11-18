// Hooks a game function and reads its parameters. No game headers are needed: the function and its fields are found by name through
// the game's reflection data. Here: KismetStringLibrary:Concat_StrStr(A, B) -> A + B, a pure function the Blueprint VM calls often.
#include "kmod.h"

#include <atomic>
#include <string>

// an engine FString: UTF-16 characters, Num counts the terminating zero
struct FStringView { const char16_t* data; int32_t num; int32_t max; };

static int g_offA = -1, g_offB = -1;
static std::atomic<int> g_calls{0};
static std::string g_last;

static std::string ToUtf8(const FStringView& s)         // ASCII + BMP only: enough for a log line
{
    std::string out;
    for (int i = 0; s.data && i + 1 < s.num && i < 200; i++)
    {
        char16_t c = s.data[i];
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

// pre hook: runs before the function. `params` is the function's parameter block; `result` is only set for Blueprint functions.
static void OnConcat(void* self, void* params, void* result, void* user)
{
    int n = ++g_calls;
    g_last = ToUtf8(kmod::At<FStringView>(params, g_offA));
    if (n <= 5) KLOGI("Concat_StrStr #%d: A=[%s] B=[%s]", n, g_last.c_str(), ToUtf8(kmod::At<FStringView>(params, g_offB)).c_str());
}

static int LuaCalls(KModLua L) { kmod::g_api->lua.pushinteger(L, g_calls); return 1; }
static int LuaLast(KModLua L) { kmod::g_api->lua.pushstring(L, g_last.c_str()); return 1; }

static int OnStart()
{
    const KModApi* api = kmod::g_api;
    void* fn = api->find_function("KismetStringLibrary:Concat_StrStr");
    if (!fn) { KLOGE("function not found"); return 1; }
    g_offA = api->property_offset(fn, "A");               // a UFunction is a struct: its parameters are properties
    g_offB = api->property_offset(fn, "B");
    if (g_offA < 0 || g_offB < 0) { KLOGE("parameters not found"); return 1; }
    KLOGI("A at +0x%x, B at +0x%x", g_offA, g_offB);
    api->hook_ufunction(fn, OnConcat, nullptr, nullptr);
    api->register_lua_function("calls", LuaCalls);
    api->register_lua_function("last", LuaLast);
    return 0;
}
KMOD_MOD(OnStart, nullptr)
