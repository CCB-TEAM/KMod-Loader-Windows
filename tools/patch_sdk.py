#!/usr/bin/env python3
"""Make the Dumper-7 CppSDK offsets runtime-resolvable (idempotent). Run after every re-dump:

    python tools/patch_sdk.py <CppSDK dir>

Basic.hpp:           constexpr Offsets::{GObjects,GNames,GWorld,ProcessEvent}  ->  inline (writable) variables
Engine_functions.cpp: `if constexpr (Offsets::GWorld != 0)`                    ->  `if (Offsets::GWorld != 0)`
core/src/resolver.cpp fills the values in at start-up (and sets GWorld to 0: UWorld::GetWorld() then asks the engine).
"""
import re, sys, pathlib

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "D:/KARDS/default/android/dump/out/CppSDK") / "SDK"
hpp = root / "Basic.hpp"
s = hpp.read_text(encoding="utf-8")
n = 0
for name in ("GObjects", "GNames", "GWorld", "ProcessEvent"):
    s, k = re.subn(r"constexpr uintptr_t (%s)(\s*)=" % name, r"inline uintptr_t \1\2=", s)
    n += k
hpp.write_text(s, encoding="utf-8")
cpp = root / "Engine_functions.cpp"
t = cpp.read_text(encoding="utf-8")
t, k = re.subn(r"if constexpr \(Offsets::GWorld != 0\)", "if (Offsets::GWorld != 0)", t)
cpp.write_text(t, encoding="utf-8")
print("patched: %d offset declaration(s), %d GWorld check(s)" % (n, k))
