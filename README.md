# kmod loader for Windows

A mod loader for the Windows version of **KARDS** (Unreal Engine 5.6). Same Lua mod API as the [Android loader](https://github.com/CCB-TEAM/KMod-Loader-Android), plus C++ native mods that build from the same source for either platform.

Documentation: <https://ccb-team.github.io/kmod-docs/> (Chinese). Start with *安装（Windows）*.

## Install

Download a zip from [Releases](https://github.com/CCB-TEAM/KMod-Loader-Windows/releases) and copy `dwmapi.dll` and the `kmod` folder next to `kards-Win64-Shipping.exe` (`<game>\kards\Binaries\Win64\`). If a `dwmapi.dll` is already there (UE4SS), rename it first. Delete `dwmapi.dll` to uninstall; the game files are never modified.

* `release` — everyday use.
* `dev` — adds a console window (live log, console commands, Lua) and debug logging, for mod authors.

Mods go into `kmod\mods\` (folders or `.kmod` packages); the log is `kmod\logs\kmod.log`. There is no paks folder on Windows.

## How it works

* `proxy/` — `dwmapi.dll`: forwards every export of the system dwmapi and starts the core from its own thread (never from `DllMain`).
* `core/src/resolver.cpp` — finds `GUObjectArray`, the name pool and `UObject::ProcessEvent` by scanning the PE image; the Blueprint interpreter and `GNatives` by disassembling with Zydis. No hard-coded offsets.
* `core/src/x64.cpp` — inline hook engine (RIP-relative operands and relative branches are relocated).
* `core/src/win/overlay.cpp` — an experimental Dear ImGui overlay on `IDXGISwapChain::Present` (Direct3D 11 and 12), used for testing native mods. Not part of the shipped builds: configure with `-DKMOD_OVERLAY=ON` to get it (Insert opens it).
* The Lua host, reflection layer (`reflect.*`), mod loader scripts (`core/lua`) and `kmod.h` are shared with the Android core.

## Build

Needs Visual Studio (C++ workload), CMake and Python 3, and the Dumper-7 SDK headers from the Android loader repository (`-DKMOD_SDK_DIR=<...>/CppSDK`; `tools/prepare_sdk.py` patches them for MSVC) and the Lua sources (`-DKMOD_LUA_DIR`).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DKMOD_DEV=ON            # dev
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64 -DKMOD_DEV=OFF   # release
cmake --build build --config Release --target kmod_core dwmapi_proxy
powershell -File tools/stage.ps1 -Flavor dev -Game "<game>\kards\Binaries\Win64" # install dev
# For release, build build-release first and pass -Flavor release.
powershell -File tools/package.ps1                                              # dist/*.zip for both flavours
```

`tools/gen_proxy_exports.py` regenerates `proxy/exports.inl` from the system's `dwmapi.dll`.

## Native (C++) mods

Include `core/include/kmod.h` and build a DLL exporting `kmod_start` / `kmod_stop` (`KMOD_MOD(...)`). The same source builds for Android by switching the toolchain; see `examples/native` (`lua_functions`, `param_reader`, `kmod_mod.cmake`). `mod.json`:

```json
{ "format": 2, "platform": ["Android", "Windows"],
  "native": { "Android": "libmymod.so", "Windows": "mymod.dll" } }
```

Windows libraries live in `native/windows-x64/`. Anything marked ANDROID ONLY in `kmod.h` (UI panels, JNI) is null on Windows; check `api->platform`.

## Licence

MIT (see `LICENSE`). Third-party code in `third_party/` keeps its own licence: Dear ImGui (MIT), Zydis (MIT).

KARDS belongs to its owners; this project is not affiliated with them.
