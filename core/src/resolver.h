// Finds the engine addresses kmod needs at run time instead of trusting offsets that were dumped for one game build.
// On Windows nothing is trusted: the game's PE image is scanned (the same validators the Android loader uses) and the code is analysed with Zydis.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// GObjects, GNames, ProcessEvent (the SDK's writable Offsets::* are filled in) and GWorld (set to 0: the SDK asks the engine).
// Safe to call repeatedly until it returns true. `report` describes what was used.
bool Resolver_ResolveCore(uintptr_t base, std::string& report);

// vtable slot of UObject::ProcessEvent found by Resolver_ResolveCore (-1 before)
int Resolver_ProcessEventSlot();

// The bytecode interpreter behind ProcessInternal (the ExecFunction every script UFunction shares; core.cpp finds that one by voting).
struct ScriptAddrs
{
    uintptr_t plsf = 0;                 // the interpreter loop (ProcessLocalScriptFunction): where a branch of ProcessInternal ends up
    uintptr_t gnatives = 0;             // the GNatives array (opcode -> native handler), 0 when not found
};
bool Resolver_FindInterpreter(uintptr_t base, uintptr_t processInternal, ScriptAddrs& out, std::string& report);

// Identifies this build of the engine: "<image size>-<timestamp>-<crc32 of the start of its code>".
std::string Resolver_BuildKey();

// All addresses in the module's memory where `pat` occurs (any segment).
void Resolver_FindBytes(const void* pat, size_t n, std::vector<uintptr_t>& hits);

bool Resolver_SafeRead(uintptr_t addr, void* out, size_t n);        // reads memory without crashing on unmapped addresses
bool Resolver_InText(uintptr_t addr);                                // inside an executable section of the game's image
bool Resolver_InImage(uintptr_t addr);
