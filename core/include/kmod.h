/* kmod native mod API (level 2). Include this one header; no other kmod or game header is needed.
 *
 * A native mod is a shared library (Android: arm64-v8a .so, Windows: x64 .dll) that exports:
 *
 *     KMOD_EXPORT int  kmod_start(const KModApi* api);   // return 0 on success; called once, on the game thread
 *     KMOD_EXPORT void kmod_stop(void);                  // optional; called when the game shuts the mod down
 *
 * and is declared in mod.json ("native": {"Android": "libmymod.so", "Windows": "mymod.dll"}, "platform": ["Android", "Windows"], "format": 2).
 * The same source builds for either platform: only the target platform / toolchain changes. Everything in this header is portable
 * except the parts marked ANDROID ONLY (they are null on Windows; check for null, or test api->platform). Everything the mod can do
 * goes through the KModApi table the loader passes in: the table only ever grows (check api->struct_size before using a
 * field that was added later), so a mod built against level 1 keeps working with newer loaders.
 *
 * All callbacks run on the game thread. Game objects are plain pointers (UObject*, UFunction*, ...); the helpers below
 * treat them as opaque handles and work through the game's reflection data, so the mod needs no game headers.
 */
#ifndef KMOD_H
#define KMOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KMOD_API_LEVEL 3

#if defined(_WIN32)
#  define KMOD_PLATFORM_WINDOWS 1
#  define KMOD_EXPORT __declspec(dllexport)
#elif defined(__ANDROID__)
#  define KMOD_PLATFORM_ANDROID 1
#  define KMOD_EXPORT __attribute__((visibility("default")))
#else
#  error "kmod mods are built for Windows (x64) or Android (arm64)"
#endif

enum { KMOD_LOG_DEBUG = 0, KMOD_LOG_INFO = 1, KMOD_LOG_WARN = 2, KMOD_LOG_ERROR = 3 };

typedef void* KModLua;      /* the Lua state a native Lua function receives (opaque; use api->lua) */

/* A hook callback: self = the object the function runs on, params = the parameter block (UFunction layout; use
 * api->property_offset to find a field), result = where a Blueprint function's return value is written (may be null). */
typedef void (*KModHookFn)(void* self, void* params, void* result, void* user);
typedef int (*KModLuaFn)(KModLua L);                      /* like lua_CFunction: returns the number of results */
typedef void (*KModTickFn)(double now_seconds, void* user);
/* UI event: panel id, widget id ("$close" when the panel was closed, "$error" if it could not be built), value as text. */
typedef void (*KModUiEventFn)(int panel, const char* id, const char* value, void* user);

/* The Lua stack functions a native Lua function can use (same meaning as the Lua 5.4 C API). */
typedef struct KModLuaApi {
    int (*gettop)(KModLua L);
    int (*type)(KModLua L, int idx);                      /* 0 nil, 1 boolean, 3 number, 4 string, 5 table ... (LUA_T*) */
    int64_t (*tointeger)(KModLua L, int idx);
    double (*tonumber)(KModLua L, int idx);
    int (*toboolean)(KModLua L, int idx);
    const char* (*tostring)(KModLua L, int idx, size_t* len);   /* valid until the function returns */
    void (*pushnil)(KModLua L);
    void (*pushboolean)(KModLua L, int b);
    void (*pushinteger)(KModLua L, int64_t v);
    void (*pushnumber)(KModLua L, double v);
    void (*pushstring)(KModLua L, const char* s);
    void (*createtable)(KModLua L, int narr, int nrec);
    void (*setfield)(KModLua L, int idx, const char* key);      /* t[key] = top, pops it */
    void (*rawseti)(KModLua L, int idx, int64_t n);             /* t[n] = top, pops it */
    int (*error)(KModLua L, const char* message);               /* raises a Lua error (does not return) */
} KModLuaApi;

typedef struct KModApi {
    uint32_t struct_size;                                 /* sizeof(KModApi) of the loader */
    uint32_t api_level;                                   /* KMOD_API_LEVEL of the loader */
    const char* mod_id;
    const char* mod_dir;                                  /* kmod/mods/<id> (read only) */
    const char* data_dir;                                 /* kmod/data/<id> (yours; created on demand) */

    void (*log)(int level, const char* message);

    /* ---- the game's code ---- */
    uintptr_t (*image_base)(void);                        /* load address of the game's main library */
    /* Finds the first match of an IDA-style pattern ("FD 7B ?? A9 ?? ?? 00 90") in the main library's code; 0 if none. */
    uintptr_t (*find_pattern)(const char* pattern);
    /* Inline-hooks a function: detour replaces target, *original receives a callable trampoline. Returns 0 on success.
     * The overwritten prologue is relocated by the loader. Android (arm64): the first 4 instructions may contain ADR/ADRP/LDR literal/B/BL
     * but not conditional branches. Windows (x64): the first 14+ bytes (whole instructions) may contain RIP-relative operands and
     * call/jmp/jcc rel; not loop/jrcxz or a jump into the overwritten bytes. Calling convention of detour/original: the platform's
     * default C convention (AAPCS64 / Microsoft x64). */
    int (*hook_function)(void* target, void* detour, void** original);

    /* ---- the game's reflection data ---- */
    void* (*find_class)(const char* name);                /* "Actor", "BP_Foo_C" */
    void* (*find_function)(const char* path);             /* "Class:Function" or "Class.Function" like RegisterHook */
    void* (*find_first_of)(const char* class_name);       /* first live instance, null if none */
    int (*property_offset)(void* struct_or_class, const char* name);   /* byte offset, -1 if unknown */
    int (*is_a)(void* object, void* cls);
    int (*object_name)(void* object, char* buf, int cap); /* writes the short name, returns its length */
    void (*process_event)(void* object, void* function, void* params);  /* call a UFunction */

    /* ---- hooks, ticks, Lua ---- */
    /* Calls pre/post (either may be null) around every call of a UFunction. Returns 0 on success. */
    int (*hook_ufunction)(void* function, KModHookFn pre, KModHookFn post, void* user);
    void (*on_tick)(KModTickFn fn, void* user);           /* about 20 times a second, game thread */
    /* Adds a function to the table `Native` that this mod's main.lua sees: Native.<name>(...) */
    void (*register_lua_function)(const char* name, KModLuaFn fn);
    KModLuaApi lua;

    /* ---- level 2: ANDROID ONLY (all null on Windows): plain Android views in a dialog on the game's activity ---- */
    /* spec_json: { "title", "modal", "close", "items": [ {type: text|button|switch|slider|edit|choice, id, text, value, min, max,
     * options} ] } (see docs). Returns a panel id (> 0) or -1. Events arrive on the game thread. */
    int (*ui_show)(const char* spec_json, KModUiEventFn cb, void* user);
    void (*ui_set)(int panel, const char* widget_id, const char* value);   /* text / "true" / number / option */
    void (*ui_close)(int panel);
    /* raw JNI for anything the panel spec cannot do: JavaVM*, JNIEnv* of the calling thread (attached), and the game's
     * application class loader (a JNI global reference) */
    void* (*java_vm)(void);
    void* (*jni_env)(void);
    void* (*app_class_loader)(void);
    /* the game's current Activity as a JNI global reference (replaced by the next call: do not delete it); null if none */
    void* (*activity)(void);
    /* runs fn(user) on the Android UI thread (the one that owns views) */
    void (*run_on_ui_thread)(void (*fn)(void*), void* user);

    /* ---- level 3 ---- */
    const char* platform;                                 /* "Android" or "Windows" */
} KModApi;

typedef int (*KModStartFn)(const KModApi* api);
typedef void (*KModStopFn)(void);

#ifdef __cplusplus
}   /* extern "C" */

/* ---- C++ conveniences (header only; optional) ------------------------------------------------------------------- */
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

namespace kmod
{
inline const KModApi* g_api = nullptr;        /* set by KMOD_MOD below */

inline void Log(int level, const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (g_api) g_api->log(level, buf);
}
#define KLOGI(...) ::kmod::Log(KMOD_LOG_INFO, __VA_ARGS__)
#define KLOGW(...) ::kmod::Log(KMOD_LOG_WARN, __VA_ARGS__)
#define KLOGE(...) ::kmod::Log(KMOD_LOG_ERROR, __VA_ARGS__)

/* A typed view of a hook's parameter block: Param<float>(params, offsetOf("Damage")) */
template <class T> inline T& At(void* base, int offset) { return *reinterpret_cast<T*>(static_cast<char*>(base) + offset); }

inline std::string NameOf(void* object)
{
    char buf[256];
    int n = g_api->object_name(object, buf, sizeof(buf));
    return std::string(buf, n > 0 ? (size_t)n : 0);
}

/* Hooks a game function with a C++ lambda/function (captures need std::function; a plain function pointer is enough here). */
inline bool HookFunction(const char* path, KModHookFn pre, KModHookFn post = nullptr, void* user = nullptr)
{
    void* fn = g_api->find_function(path);
    if (!fn) { KLOGW("HookFunction: %s not found", path); return false; }
    return g_api->hook_ufunction(fn, pre, post, user) == 0;
}

/* Shows a panel; `handler(panel, widgetId, value)` receives the events (the std::function lives until the panel closes). */
using UiHandler = std::function<void(int panel, const char* id, const char* value)>;
inline int ShowPanel(const std::string& specJson, UiHandler handler)
{
    if (!g_api->ui_show) return -1;                    /* no UI on this platform */
    auto* h = new UiHandler(std::move(handler));
    int id = g_api->ui_show(specJson.c_str(), [](int panel, const char* wid, const char* val, void* user)
    {
        auto* fn = static_cast<UiHandler*>(user);
        if (*fn) (*fn)(panel, wid, val);
        if (std::string(wid) == "$close" || std::string(wid) == "$error") delete fn;
    }, h);
    if (id < 0) delete h;
    return id;
}
}   // namespace kmod

/* Declares the entry points. Usage:
 *     static int OnStart() { KLOGI("hello"); return 0; }
 *     KMOD_MOD(OnStart, nullptr)           // start function, optional stop function
 */
#define KMOD_MOD(START, STOP)                                                                                   \
    extern "C" KMOD_EXPORT int kmod_start(const KModApi* api) { ::kmod::g_api = api; return (START)(); }        \
    extern "C" KMOD_EXPORT void kmod_stop(void) { void (*s)(void) = (STOP); if (s) s(); }
#endif

#endif /* KMOD_H */
