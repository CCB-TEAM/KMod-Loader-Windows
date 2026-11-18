cmake_minimum_required(VERSION 3.22)
project(kmod_loader_windows C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")     # static CRT: the DLLs sit inside the game's process
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The game's C++ SDK (Dumper-7 / MobileDumper-7 output, patched by tools/patch_sdk.py so the offsets are runtime variables).
# The layout of UE 5.6 reflection data is the same on Android and Windows; addresses are resolved at run time.
set(KMOD_SDK_DIR "D:/KARDS/default/android/dump/out/CppSDK" CACHE PATH "Dumper CppSDK root")
set(KMOD_LUA_DIR "D:/CCB-TEAM/RE-UE4SS/deps/first/LuaRaw" CACHE PATH "Lua 5.4 sources (from UE4SS deps)")
option(KMOD_DEV "Developer build: verbose logs" ON)
option(KMOD_OVERLAY "Experimental ImGui overlay (Insert); not part of the shipped builds" OFF)

# ---- Zydis (x86-64 disassembler used by the inline hook engine and the signature resolvers)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYAN_SYSTEM_WINDOWS ON)
add_subdirectory(third_party/zydis EXCLUDE_FROM_ALL)

# ---- Lua 5.4 (MIT); fopen/remove/rename are redirected to UTF-8 aware versions (core/src/win/lua_compat.h)
file(GLOB KMOD_LUA_SRC ${KMOD_LUA_DIR}/src/*.c)
list(FILTER KMOD_LUA_SRC EXCLUDE REGEX ".*/(lua|luac|luauser)\\.c$")
add_library(kmodlua OBJECT ${KMOD_LUA_SRC})
target_include_directories(kmodlua PUBLIC ${KMOD_LUA_DIR}/include)
target_compile_options(kmodlua PRIVATE /w /O2 /FI${CMAKE_CURRENT_SOURCE_DIR}/core/src/win/lua_compat.h)

# ---- the game's SDK objects (slow, built once)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)
execute_process(COMMAND python -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_sdk.py ${KMOD_SDK_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_library(kmodsdk OBJECT
  ${CMAKE_CURRENT_BINARY_DIR}/gen/Basic_win.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/gen/CoreUObject_functions.cpp)
# gen/sdk comes first: it holds the MSVC-ready CoreUObject_classes.hpp and the empty Engine/UMG stubs
target_include_directories(kmodsdk PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/gen/sdk ${KMOD_SDK_DIR} ${KMOD_SDK_DIR}/SDK)
target_compile_options(kmodsdk PRIVATE /w /O1 /bigobj /EHsc /utf-8)

# ---- the core: kmod_core.dll (loaded by the dwmapi proxy)
file(GLOB KMOD_CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/core/src/win/*.cpp)
# Dear ImGui (overlay): its own translation units, without the loader's forced includes
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(kmodimgui OBJECT ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(kmodimgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(kmodimgui PRIVATE /W3 /O2 /EHsc /utf-8)
target_compile_definitions(kmodimgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_library(kmod_core SHARED ${KMOD_CORE_SRC} $<TARGET_OBJECTS:kmodsdk> $<TARGET_OBJECTS:kmodlua>)
if(KMOD_OVERLAY)
  target_sources(kmod_core PRIVATE $<TARGET_OBJECTS:kmodimgui>)
  target_compile_definitions(kmod_core PRIVATE KMOD_OVERLAY=1)
endif()
target_include_directories(kmod_core PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_include_directories(kmod_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include ${CMAKE_CURRENT_SOURCE_DIR}/core/src ${CMAKE_CURRENT_SOURCE_DIR}/core/src/win ${CMAKE_CURRENT_SOURCE_DIR}/core/src/win/posix
  ${CMAKE_CURRENT_BINARY_DIR}/gen/sdk ${KMOD_SDK_DIR} ${KMOD_SDK_DIR}/SDK ${KMOD_LUA_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_options(kmod_core PRIVATE /W3 /wd4244 /wd4267 /wd4996 /wd4800 /bigobj /EHsc /utf-8 /O2 /FI${CMAKE_CURRENT_SOURCE_DIR}/core/src/win/compat.h)
target_compile_definitions(kmod_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS KMOD_PLATFORM_WINDOWS=1 $<$<BOOL:${KMOD_DEV}>:KMOD_DEV=1> $<$<BOOL:${KMOD_OVERLAY}>:KMOD_OVERLAY=1>)
target_link_libraries(kmod_core PRIVATE Zydis winhttp ws2_32 secur32 dbghelp version shlwapi dxguid imm32)
set_target_properties(kmod_core PROPERTIES OUTPUT_NAME kmod_core)

# Embed the built-in Lua scripts (kModLoaderLua, kModManagerLua, ...) as byte arrays: MSVC limits string literals to 64 KB
set(KMOD_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${KMOD_GEN})
set(KMOD_LUA_VARS modloader:kModLoaderLua modmanager:kModManagerLua commands:kCommandsLua json:kJsonLua ui:kUiLua net:kNetLua ue4ss:kUe4ssLua)
foreach(PAIR ${KMOD_LUA_VARS})
  string(REPLACE ":" ";" P "${PAIR}")
  list(GET P 0 NAME)
  list(GET P 1 VAR)
  execute_process(COMMAND python -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_lua.py ${CMAKE_CURRENT_SOURCE_DIR}/core/lua/${NAME}.lua ${KMOD_GEN}/${NAME}_lua.h ${VAR})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/lua/${NAME}.lua)
endforeach()

# ---- the dwmapi.dll proxy (loaded by the game instead of the system dwmapi; forwards everything and starts kmod_core.dll) ----
add_library(dwmapi_proxy SHARED proxy/dllmain.cpp)
set_target_properties(dwmapi_proxy PROPERTIES OUTPUT_NAME dwmapi)
target_include_directories(dwmapi_proxy PRIVATE proxy)
target_compile_definitions(dwmapi_proxy PRIVATE WIN32_LEAN_AND_MEAN)
if(KMOD_DEV)
  target_link_options(kmod_core PRIVATE /MAP /DEBUG)      # symbols for the dev build (the sampling profiler output can be mapped back)
endif()
