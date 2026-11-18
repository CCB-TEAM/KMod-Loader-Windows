# Shared by the native mod examples: builds one mod library for the toolchain that is configured.
#   MSVC (Windows x64)        -> native/windows-x64/<name>.dll
#   Android NDK (arm64-v8a)   -> native/arm64-v8a/lib<name>.so
# The mod's source files are the same for both: only the toolchain differs (kmod.h picks the platform from the compiler).
function(kmod_native_mod name)
  # kmod.h: from the repository (core/include) when this example sits in it, otherwise from ./include
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../core/include/kmod.h)
    set(KMOD_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../core/include)
  else()
    set(KMOD_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  endif()
  add_library(${name} SHARED ${ARGN})
  target_include_directories(${name} PRIVATE ${KMOD_INCLUDE})
  if(MSVC)
    target_compile_options(${name} PRIVATE /O2 /utf-8 /EHsc /W3)
    target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")       # static CRT: nothing to install next to the game
    set(out ${CMAKE_CURRENT_SOURCE_DIR}/native/windows-x64)
    set_target_properties(${name} PROPERTIES OUTPUT_NAME ${name}
      RUNTIME_OUTPUT_DIRECTORY ${out} RUNTIME_OUTPUT_DIRECTORY_RELEASE ${out} RUNTIME_OUTPUT_DIRECTORY_DEBUG ${out})
  elseif(ANDROID)
    target_compile_options(${name} PRIVATE -fvisibility=hidden -O2)
    target_link_options(${name} PRIVATE -static-libstdc++)
    set_target_properties(${name} PROPERTIES OUTPUT_NAME ${name} PREFIX "lib" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/native/arm64-v8a)
  else()
    message(FATAL_ERROR "kmod native mods are built for Windows (MSVC) or Android (NDK)")
  endif()
endfunction()
