"""Prepares the Android-generated SDK headers for MSVC (they were generated for clang):
   gen/sdk/CoreUObject_classes.hpp  the original with GCC-style packed structs turned into #pragma pack
   gen/sdk/Engine_classes.hpp, UMG_classes.hpp   empty stubs (the core does not use the game's Engine/UMG classes)
   gen/Basic_win.cpp                the SDK's Basic.cpp with the Android-only image lookup replaced by GetModuleHandleW
usage: prepare_sdk.py <CppSDK dir> <gen dir>"""
import os, re, sys
sdk, gen = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(gen, 'sdk'), exist_ok=True)
S = os.path.join(sdk, 'SDK')

# --- Basic.cpp
s = open(os.path.join(S, 'Basic.cpp'), encoding='utf8').read()
a = s.index('SDK_NAMESPACE_START'); b = s.index('static void AppendAsUtf8')
win = '''SDK_NAMESPACE_START

bool InSDKUtils::PathEndsWithModuleName(const char*) { return false; }

uintptr_t InSDKUtils::GetImageBase()
{
	/* the game's own executable: Offsets::* are relative to it */
	static uintptr_t Cached = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	return Cached;
}


'''
s = s[:a] + win + s[b:]
decl = 'extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t*);   // not windows.h: its TCHAR clashes with the SDK\'s'
s = s.replace('#include "Basic.hpp"', '#include "Basic.hpp"\n' + decl, 1)
s = s.replace('return UKismetStringLibrary::Conv_StringToName(FString(Name));', 'return FName();      /* needs the Engine SDK; unused by kmod */')
open(os.path.join(gen, 'Basic_win.cpp'), 'w', encoding='utf8').write(s)

# --- CoreUObject_classes.hpp: packed structs
c = open(os.path.join(S, 'CoreUObject_classes.hpp'), encoding='utf8').read()
pat = re.compile(r'struct __attribute__\(\(packed\)\) (\w+) (\{[^}]*\}) (\w+);')
c, n = pat.subn(lambda m: '#pragma pack(push, 1)\n\tstruct %s %s;\n#pragma pack(pop)\n\t%s %s;' % (m.group(1), m.group(2), m.group(1), m.group(3)), c)
open(os.path.join(gen, 'sdk', 'CoreUObject_classes.hpp'), 'w', encoding='utf8').write(c)
for name in ('Engine_classes.hpp', 'UMG_classes.hpp'):
    open(os.path.join(gen, 'sdk', name), 'w').write('#pragma once\n#include "CoreUObject_classes.hpp"\n')

# --- Basic.hpp: same treatment (the F*Property classes live there). The Dumper-7 size/offset static_asserts are removed: they describe the
# clang (Itanium ABI) layout, MSVC places members of derived classes differently. kmod does not use C++ member offsets of these classes:
# the reflection layout defaults are plain numbers (core/src/reflect.h) that the running game is checked against.
b = open(os.path.join(S, 'Basic.hpp'), encoding='utf8').read()
b = b.replace('#include "../PropertyFixup.hpp"', '#include "PropertyFixup.hpp"').replace('#include "../UnrealContainers.hpp"', '#include "UnrealContainers.hpp"')
b = b.replace('#include "../Assertions.inl"', '').replace('#include "../NameCollisions.inl"', '#include "NameCollisions.inl"')
b = re.sub(r'DUMPER7_ASSERTS_\w+;', '', b)
# the vtable slot of ProcessEvent differs between the Itanium (Android) and MSVC layouts: the resolver sets it at run time
b = b.replace('constexpr int32 ProcessEventIdx', 'inline int32 ProcessEventIdx')
open(os.path.join(gen, 'sdk', 'Basic.hpp'), 'w', encoding='utf8').write(b)
c2 = open(os.path.join(gen, 'sdk', 'CoreUObject_classes.hpp'), encoding='utf8').read()
c2 = re.sub(r'DUMPER7_ASSERTS_\w+;', '', c2)
open(os.path.join(gen, 'sdk', 'CoreUObject_classes.hpp'), 'w', encoding='utf8').write(c2)

# structs/parameters headers: copied as they are, so that their quoted #include "Basic.hpp" finds the patched copy in gen/sdk
for name in ('CoreUObject_structs.hpp', 'CoreUObject_parameters.hpp'):
    t = open(os.path.join(S, name), encoding='utf8').read()
    t = re.sub(r'DUMPER7_ASSERTS_\w+;', '', t)
    open(os.path.join(gen, 'sdk', name), 'w', encoding='utf8').write(t)

# CoreUObject_functions.cpp: copied so its quoted #includes resolve to gen/sdk first
open(os.path.join(gen, 'CoreUObject_functions.cpp'), 'w', encoding='utf8').write(open(os.path.join(S, 'CoreUObject_functions.cpp'), encoding='utf8').read())
print('prepared SDK (%d packed struct(s) patched)' % n)
