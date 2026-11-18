#include "x64.h"

#include <Zydis/Zydis.h>

#include <cstring>
#include <tlhelp32.h>

#include "kmod_log.h"

namespace
{
ZydisDecoder& Decoder()
{
    static ZydisDecoder d = [] { ZydisDecoder x; ZydisDecoderInit(&x, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64); return x; }();
    return d;
}

bool DecodeFull(const uint8_t* code, size_t avail, ZydisDecodedInstruction& ins, ZydisDecodedOperand* ops)
{
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), code, avail, &ins, ops));
}

// executable memory within +-2 GB of `near`
uint8_t* AllocNear(uintptr_t where, size_t size)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t lo = where > 0x7FFF0000ull ? where - 0x7FFF0000ull : 0x10000;
    const uintptr_t hi = where + 0x7FFF0000ull;
    for (uintptr_t step = 0; step < 0x7FFF0000ull; step += gran)
    {
        for (int dir = 0; dir < 2; dir++)
        {
            uintptr_t a = dir == 0 ? ((where + step) & ~(gran - 1)) : ((where - step) & ~(gran - 1));
            if (a < lo || a > hi) continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) continue;
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

void EmitJmpAbs(std::vector<uint8_t>& o, uint64_t target)      // jmp [rip+0] ; dq target
{
    const uint8_t j[6] = {0xFF, 0x25, 0, 0, 0, 0};
    o.insert(o.end(), j, j + 6);
    for (int i = 0; i < 8; i++) o.push_back((uint8_t)(target >> (8 * i)));
}

void EmitCallAbs(std::vector<uint8_t>& o, uint64_t target)     // call [rip+2] ; jmp +8 ; dq target
{
    const uint8_t c[8] = {0xFF, 0x15, 2, 0, 0, 0, 0xEB, 0x08};
    o.insert(o.end(), c, c + 8);
    for (int i = 0; i < 8; i++) o.push_back((uint8_t)(target >> (8 * i)));
}

// Copies the instruction at `src` into `out` as it must look when it runs at address `dstAddr`. stolenLo/Hi: the range being overwritten.
bool Relocate(uintptr_t src, uintptr_t dstAddr, uintptr_t stolenLo, uintptr_t stolenHi, std::vector<uint8_t>& out, std::string& why)
{
    ZydisDecodedInstruction ins; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!DecodeFull(reinterpret_cast<const uint8_t*>(src), 15, ins, ops)) { why = "cannot decode the prologue"; return false; }
    uint8_t bytes[16]; memcpy(bytes, reinterpret_cast<const void*>(src), ins.length);

    // relative branches
    bool rel = false;
    for (int i = 0; i < ins.operand_count_visible; i++)
        if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[i].imm.is_relative) rel = true;
    if (rel)
    {
        ZyanU64 target = 0;
        for (int i = 0; i < ins.operand_count_visible; i++)
            if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[i].imm.is_relative) ZydisCalcAbsoluteAddress(&ins, &ops[i], src, &target);
        if (target >= stolenLo && target < stolenHi) { why = "a jump inside the stolen prologue"; return false; }
        switch (ins.mnemonic)
        {
        case ZYDIS_MNEMONIC_CALL: EmitCallAbs(out, target); return true;
        case ZYDIS_MNEMONIC_JMP: EmitJmpAbs(out, target); return true;
        case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE: case ZYDIS_MNEMONIC_JRCXZ: case ZYDIS_MNEMONIC_JECXZ:
            why = "loop/jrcxz in the stolen prologue"; return false;
        default:
        {   // jcc: invert the condition and hop over an absolute jump
            int cc = -1;
            if (ins.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT && ins.opcode >= 0x70 && ins.opcode <= 0x7F) cc = ins.opcode & 0xF;
            else if (ins.opcode_map == ZYDIS_OPCODE_MAP_0F && ins.opcode >= 0x80 && ins.opcode <= 0x8F) cc = ins.opcode & 0xF;
            if (cc < 0) { why = "unsupported relative branch"; return false; }
            out.push_back((uint8_t)(0x70 | (cc ^ 1))); out.push_back(14);
            EmitJmpAbs(out, target);
            return true;
        }
        }
    }
    // RIP-relative memory operand: keep the instruction, fix the displacement for the new place
    for (int i = 0; i < ins.operand_count_visible; i++)
    {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[i].mem.base == ZYDIS_REGISTER_RIP)
        {
            ZyanU64 t = 0; ZydisCalcAbsoluteAddress(&ins, &ops[i], src, &t);
            int64_t nd = (int64_t)t - (int64_t)(dstAddr + ins.length);
            if (nd < INT32_MIN || nd > INT32_MAX) { why = "RIP-relative operand out of range"; return false; }
            int32_t d32 = (int32_t)nd;
            memcpy(bytes + ins.raw.disp.offset, &d32, 4);
        }
    }
    out.insert(out.end(), bytes, bytes + ins.length);
    return true;
}

void SuspendOthers(std::vector<HANDLE>& hs)
{
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
        do
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID))
            {
                if (SuspendThread(h) != (DWORD)-1) hs.push_back(h); else CloseHandle(h);
            }
        } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}
void ResumeAll(std::vector<HANDLE>& hs) { for (HANDLE h : hs) { ResumeThread(h); CloseHandle(h); } hs.clear(); }
}

namespace X64
{
Insn Decode(uintptr_t addr)
{
    Insn r; r.addr = addr;
    ZydisDecodedInstruction ins; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!DecodeFull(reinterpret_cast<const uint8_t*>(addr), 15, ins, ops)) return r;
    r.valid = true; r.len = ins.length; r.mnemonic = ins.mnemonic;
    r.isCall = ins.mnemonic == ZYDIS_MNEMONIC_CALL;
    r.isJmp = ins.mnemonic == ZYDIS_MNEMONIC_JMP;
    r.isRet = ins.mnemonic == ZYDIS_MNEMONIC_RET;
    r.isCondJmp = ins.meta.category == ZYDIS_CATEGORY_COND_BR;
    r.isLea = ins.mnemonic == ZYDIS_MNEMONIC_LEA;
    r.isMovLoad = ins.mnemonic == ZYDIS_MNEMONIC_MOV && ins.operand_count_visible >= 2 && ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY;
    for (int i = 0; i < ins.operand_count_visible; i++)
    {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[i].imm.is_relative)
        {
            ZyanU64 t = 0; ZydisCalcAbsoluteAddress(&ins, &ops[i], addr, &t);
            r.relBranch = true; r.target = (uintptr_t)t;
        }
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            if (ops[i].mem.base == ZYDIS_REGISTER_RIP)
            {
                ZyanU64 t = 0; ZydisCalcAbsoluteAddress(&ins, &ops[i], addr, &t);
                r.ripMem = true; r.ripTarget = (uintptr_t)t;
            }
            r.memHasBase = ops[i].mem.base != ZYDIS_REGISTER_NONE;
            r.memHasIndex = ops[i].mem.index != ZYDIS_REGISTER_NONE;
            r.memScale = ops[i].mem.scale;
            r.memDisp = (int)ops[i].mem.disp.value;
            if ((r.isCall || r.isJmp)) r.memCall = true;
        }
    }
    return r;
}

std::vector<Insn> Body(uintptr_t fn, size_t maxBytes)
{
    std::vector<Insn> v;
    uintptr_t a = fn;
    while (a < fn + maxBytes)
    {
        Insn i = Decode(a);
        if (!i.valid) break;
        v.push_back(i);
        a += i.len;
        if (i.isRet) break;                                   // the first return ends the straight-line part; enough for the scanners
    }
    return v;
}
}

bool KMod_InstallHook(void* targetPtr, void* detour, void** original)
{
    uintptr_t target = reinterpret_cast<uintptr_t>(targetPtr);
    // how many bytes the 14-byte jump overwrites: whole instructions
    size_t stolen = 0;
    while (stolen < 14)
    {
        ZydisDecodedInstruction ins; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!DecodeFull(reinterpret_cast<const uint8_t*>(target + stolen), 15, ins, ops)) { KLog(KLOG_WARN, "hook", "cannot decode at %p+%zu", targetPtr, stolen); return false; }
        stolen += ins.length;
        if (ins.mnemonic == ZYDIS_MNEMONIC_RET && stolen < 14) { KLog(KLOG_WARN, "hook", "function at %p is shorter than 14 bytes", targetPtr); return false; }
    }
    uint8_t* tramp = AllocNear(target, 256);
    if (!tramp) { KLog(KLOG_WARN, "hook", "no executable memory near %p", targetPtr); return false; }

    std::vector<uint8_t> code;
    for (size_t off = 0; off < stolen;)
    {
        std::string why;
        size_t before = code.size();
        if (!Relocate(target + off, reinterpret_cast<uintptr_t>(tramp) + before, target, target + stolen, code, why))
        {
            KLog(KLOG_WARN, "hook", "cannot relocate the prologue of %p: %s", targetPtr, why.c_str());
            VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }
        ZydisDecodedInstruction ins; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        DecodeFull(reinterpret_cast<const uint8_t*>(target + off), 15, ins, ops);
        off += ins.length;
    }
    EmitJmpAbs(code, target + stolen);
    if (code.size() > 256) { VirtualFree(tramp, 0, MEM_RELEASE); KLog(KLOG_WARN, "hook", "trampoline too large"); return false; }
    memcpy(tramp, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), tramp, code.size());

    std::vector<uint8_t> patch;
    EmitJmpAbs(patch, reinterpret_cast<uint64_t>(detour));
    while (patch.size() < stolen) patch.push_back(0xCC);

    std::vector<HANDLE> suspended;
    SuspendOthers(suspended);
    DWORD old = 0;
    bool ok = VirtualProtect(targetPtr, stolen, PAGE_EXECUTE_READWRITE, &old) != 0;
    if (ok)
    {
        memcpy(targetPtr, patch.data(), stolen);
        VirtualProtect(targetPtr, stolen, old, &old);
        FlushInstructionCache(GetCurrentProcess(), targetPtr, stolen);
    }
    ResumeAll(suspended);
    if (!ok) { KLog(KLOG_WARN, "hook", "VirtualProtect failed on %p", targetPtr); VirtualFree(tramp, 0, MEM_RELEASE); return false; }
    *original = tramp;
    return true;
}
