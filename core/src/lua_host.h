// Lua host for the kmod mod core: UE4SS-style scripting API on top of UE reflection.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace SDK { class UObject; class UFunction; }

void LuaHost_Init();                                   // game thread; create state, register API, load mods
void LuaHost_Tick(double now);                         // game thread; timers, queued callbacks, dev console
void LuaHost_FastTick(double now);                     // game thread; FastLoop timers and the fast new-object scan (every few ms)
bool LuaHost_HasFast();
bool LuaHost_HasHook(SDK::UFunction* fn);              // cheap check used by the ProcessEvent detour
void LuaHost_PreHook(SDK::UObject* self, SDK::UFunction* fn, void* parms, void* result = nullptr);    // result: script-function hooks only
void LuaHost_PostHook(SDK::UObject* self, SDK::UFunction* fn, void* parms, void* result = nullptr);
void LuaHost_RunString(const std::string& code, const char* chunkName);
void LuaHost_ReloadMods();
bool KMod_DetectLayout(bool force);
bool KMod_LayoutMismatch();
std::vector<std::string> KMod_ScriptDiag(bool start);
bool KMod_EnsureNativeHook(SDK::UFunction* fn, std::string& note);     // core.cpp: thunk for the calls the Blueprint VM makes to a native function
void KMod_NativeStats(unsigned long long& vmCalls, int& slots, bool& haveTable, int& chain);
std::vector<std::string> KMod_PerfSample(bool start);                // core.cpp: sampling profiler of the game thread
void KMod_PerfPE(unsigned long long& calls, double& ms);          // core.cpp
void KMod_ScriptStats(unsigned long long& pi, unsigned long long& pe, bool& installed);                               // core.cpp: start layout detection in the background (dev: needs kmod/dev/libkmodmd7.so)
std::string KMod_LayoutReport();
bool KMod_MountPak(const std::string& path);           // core.cpp; game thread: mount one .pak through the engine
void KMod_MountPendingPaks();                           // core.cpp; game thread: mount the paks in kmod/paks that have not been handled yet
std::vector<std::pair<std::string, std::string>> KMod_PakStatus();   // (file name, status) for every .pak in kmod/paks

// ---- services for native mods (lua_host.cpp / core.cpp); see native_mods.cpp ----
struct lua_State;
void* KMod_FindFunctionByPath(const char* path);
void* KMod_FindClassByName(const char* name);
void* KMod_FindFirstInstance(const char* cls);
bool KMod_AddNativeHook(void* fn, void (*pre)(void*, void*, void*, void*), void (*post)(void*, void*, void*, void*), void* user);
void KMod_AddNativeTick(void (*fn)(double, void*), void* user);
void KMod_RegisterNativeLua(const char* modId, const char* name, int (*fn)(lua_State*));
lua_State* KMod_LuaState();
bool KMod_InstallHook(void* target, void* detour, void** original);     // core.cpp
int Native_Load(lua_State* L);          // native_mods.cpp: kmod.native_load(id, dir, file)
int Native_Prune(lua_State* L);         // kmod.native_prune({ids})
int Native_IsLoaded(lua_State* L);
void NativeMods_Shutdown();
int NativeUi_Show(const char* specJson, void (*cb)(int, const char*, const char*, void*), void* user);   // native_ui.cpp
void NativeUi_Set(int panel, const char* key, const char* value);
void NativeUi_Close(int panel);
void NativeUi_Drain();
void* NativeUi_JavaVM();
void* NativeUi_Env();
void* NativeUi_AppClassLoader();
void* NativeUi_Activity();
void NativeUi_RunOnUi(void (*fn)(void*), void* user);
