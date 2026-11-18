// The native-mod UI API (api->ui_show ...) is Android-only. These stubs keep the shared Lua host linking; kmod.h documents the fields as null on Windows.
#include "lua_host.h"

int NativeUi_Show(const char*, void (*)(int, const char*, const char*, void*), void*) { return -1; }
void NativeUi_Set(int, const char*, const char*) {}
void NativeUi_Close(int) {}
void NativeUi_Drain() {}
void* NativeUi_JavaVM() { return nullptr; }
void* NativeUi_Env() { return nullptr; }
void* NativeUi_AppClassLoader() { return nullptr; }
void* NativeUi_Activity() { return nullptr; }
void NativeUi_RunOnUi(void (*)(void*), void*) {}
