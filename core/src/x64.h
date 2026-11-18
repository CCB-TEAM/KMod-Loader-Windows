// x86-64 helpers: a small inline hook engine and code-analysis helpers (Zydis based), used by the engine resolver and the hooks.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Inline hook: `detour` replaces `target`; *original receives a callable trampoline (the stolen instructions, relocated, plus a jump back).
// Handles RIP-relative operands, call/jmp rel32 and conditional jumps in the stolen prologue; refuses loop/jrcxz and jumps into the stolen bytes.
// Other threads are suspended while the 14-byte jump is written. Returns false (and logs why) when the prologue cannot be relocated.
bool KMod_InstallHook(void* target, void* detour, void** original);

namespace X64
{
struct Insn
{
    uintptr_t addr = 0;
    int len = 0;
    bool valid = false;
    // what the scanners need to know
    bool isCall = false, isJmp = false, isRet = false, isCondJmp = false;
    bool relBranch = false;          // call/jmp/jcc with a relative target
    uintptr_t target = 0;            // absolute target of a relative branch
    bool ripMem = false;             // a RIP-relative memory operand
    uintptr_t ripTarget = 0;         // its absolute address
    bool isLea = false;
    bool memCall = false;            // call/jmp through a memory operand: call [base + index*scale + disp]
    int memScale = 0, memDisp = 0;   // for the memory operand
    bool memHasIndex = false, memHasBase = false;
    bool isMovLoad = false;          // mov reg, [mem]
    int mnemonic = 0;                // ZydisMnemonic
};

// Decodes one instruction at `addr` (the memory must be readable). Returns an Insn with valid=false on failure.
Insn Decode(uintptr_t addr);
// The whole function body starting at `fn`: instructions up to `maxBytes`, stopping after a ret/jmp that is not followed by code we reached.
std::vector<Insn> Body(uintptr_t fn, size_t maxBytes);
}
