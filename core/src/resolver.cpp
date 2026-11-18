#include "resolver.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "kmod_common.h"
#include "kmod_log.h"
#include "x64.h"

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"

using namespace SDK;

namespace
{
constexpr uint32_t PF_X = 1, PF_W = 2;
struct Seg { uintptr_t lo, hi; uint32_t flags; };
std::vector<Seg> g_segs;
uintptr_t g_modLo = 0, g_modHi = 0;
uint32_t g_timestamp = 0;
std::mutex g_segMu;

void LoadSegs()
{
    std::lock_guard<std::mutex> lock(g_segMu);
    if (!g_segs.empty()) return;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_timestamp = nt->FileHeader.TimeDateStamp;
    g_modLo = base;
    g_modHi = base + nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        uint32_t f = 0;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) f |= PF_X;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) f |= PF_W;
        uint32_t size = sec[i].Misc.VirtualSize;
        if (!size) continue;
        g_segs.push_back({base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + size, f});
    }
}

bool SafeRead(uintptr_t addr, void* out, size_t n)       // fails instead of crashing on unmapped memory (ReadProcessMemory raises no exception)
{
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool InModule(uint64_t v) { return v >= g_modLo && v < g_modHi; }
bool InText(uint64_t v) { for (auto& s : g_segs) if ((s.flags & PF_X) && v >= s.lo && v < s.hi) return true; return false; }

// Reads a whole section into `buf` (zeros where memory is unreadable)
void ReadSeg(const Seg& s, std::vector<uint8_t>& buf)
{
    buf.assign(s.hi - s.lo, 0);
    for (size_t off = 0; off < buf.size(); off += 1 << 20)
    {
        size_t n = std::min<size_t>(1 << 20, buf.size() - off);
        if (!SafeRead(s.lo + off, buf.data() + off, n))
            for (size_t o = 0; o < n; o += 4096) SafeRead(s.lo + off + o, buf.data() + off + o, std::min<size_t>(4096, n - o));
    }
}

// --- GObjects -----------------------------------------------------------------------------------
bool ObjArrayOk(uintptr_t a)
{
    uint8_t h[0x30];
    if (!SafeRead(a, h, sizeof(h))) return false;
    int32_t max = *reinterpret_cast<int32_t*>(h + 0x20), num = *reinterpret_cast<int32_t*>(h + 0x24);
    int32_t maxc = *reinterpret_cast<int32_t*>(h + 0x28), numc = *reinterpret_cast<int32_t*>(h + 0x2c);
    uint64_t objs = *reinterpret_cast<uint64_t*>(h + 0x10);
    if (num < 1000 || num > 8000000 || max < num || max > 16000000) return false;
    if (numc <= 0 || maxc < numc || (int64_t)numc * 65536 < num || (int64_t)(numc - 1) * 65536 > num) return false;
    if (!objs || (objs & 7)) return false;
    uint64_t chunk0 = 0, obj0 = 0;
    if (!SafeRead(objs, &chunk0, 8) || !chunk0) return false;
    if (!SafeRead(chunk0, &obj0, 8)) return false;
    uint64_t vt = 0;
    return obj0 && SafeRead(obj0, &vt, 8) && InModule(vt);        // the first object is a UObject with a vtable inside the engine
}

// --- GNames -------------------------------------------------------------------------------------
// The SDK reads the block table at pool+0x40 (its FNamePool layout); the real pool starts elsewhere on Windows, but only the address
// of Blocks[] matters, so "pool" here is simply (address of Blocks[0]) - 0x40.
bool NamesOk(uintptr_t base, uintptr_t pool)
{
    uint64_t p = 0;
    if (!SafeRead(pool + 0x40, &p, 8) || InModule(p)) return false;
    uint8_t e[6];
    if (!SafeRead(p, e, 6)) return false;
    if ((e[0] & 1) || e[1] != 0x01 || memcmp(e + 2, "None", 4) != 0) return false;      // FNameEntryHeader{Len=4}, "None"
    uintptr_t old = Offsets::GNames;
    Offsets::GNames = pool - base;
    bool ok = InSDKUtils::GetNameByIndex(0) == "None" && !InSDKUtils::GetNameByIndex(1).empty();
    if (!ok) Offsets::GNames = old;
    return ok;
}

uintptr_t g_foundObjects = 0, g_foundNames = 0;
int g_peSlot = -1;

// does the function at `fn` look like UObject::ProcessEvent? (loads UFunction::Func at 0xD8, FunctionFlags at 0xB0 and ParmsSize at 0xB6)
int ProcessEventScore(uintptr_t fn)
{
    bool func = false, flags = false, parms = false;
    uintptr_t a = fn;
    for (int n = 0; n < 400 && a < fn + 0x700; n++)
    {
        X64::Insn i = X64::Decode(a);
        if (!i.valid) break;
        if (i.memHasBase && !i.memHasIndex && !i.ripMem)
        {
            if (i.memDisp == 0xD8) func = true;
            if (i.memDisp == 0xB0) flags = true;
            if (i.memDisp == 0xB6) parms = true;
        }
        a += i.len;
    }
    return (func ? 4 : 0) + (flags ? 2 : 0) + (parms ? 1 : 0);
}
}

bool Resolver_ResolveCore(uintptr_t base, std::string& report)
{
    LoadSegs();
    if (g_segs.empty()) { report = "module sections not found"; return false; }
    report.clear();

    // GObjects: the FUObjectArray (UE4SS finds the same object: GUObjectArray)
    if (!g_foundObjects)
    {
        for (auto& s : g_segs)
        {
            if (!(s.flags & PF_W) || g_foundObjects) continue;
            std::vector<uint8_t> buf; ReadSeg(s, buf);
            for (size_t o = 0; o + 0x30 <= buf.size(); o += 8)
            {
                int32_t max = *reinterpret_cast<int32_t*>(&buf[o + 0x20]), num = *reinterpret_cast<int32_t*>(&buf[o + 0x24]);
                if (num < 1000 || num > 8000000 || max < num) continue;
                if (ObjArrayOk(s.lo + o)) { g_foundObjects = s.lo + o; break; }
            }
        }
        if (g_foundObjects) { Offsets::GObjects = g_foundObjects - base; KLog(KLOG_INFO, "resolver", "GObjects: found by scanning at base+0x%llx", (unsigned long long)Offsets::GObjects); }
    }
    if (!g_foundObjects) { report += "GObjects not found"; return false; }

    // GNames: a writable slot that points at the first name block ("None")
    if (!g_foundNames)
    {
        for (auto& s : g_segs)
        {
            if (!(s.flags & PF_W) || g_foundNames) continue;
            std::vector<uint8_t> buf; ReadSeg(s, buf);
            for (size_t o = 0x40; o + 8 <= buf.size(); o += 8)
            {
                uint64_t p = *reinterpret_cast<uint64_t*>(&buf[o]);
                if (p < 0x10000 || p > 0x7FFFFFFFFFFFull || InModule(p) || (p & 7)) continue;
                if (NamesOk(base, s.lo + o - 0x40)) { g_foundNames = s.lo + o - 0x40; break; }
            }
        }
        if (g_foundNames) KLog(KLOG_INFO, "resolver", "GNames: block table at base+0x%llx", (unsigned long long)(Offsets::GNames + 0x40));
    }
    if (!g_foundNames) { report += "GNames not found"; return false; }

    // ProcessEvent: a slot in any UObject's vtable whose code reads UFunction::Func (0xD8), FunctionFlags (0xB0) and ParmsSize (0xB6)
    if (g_peSlot >= 0) { report += "ProcessEvent: already resolved"; return true; }
    for (int i = 1; i < 2000 && g_peSlot < 0; i++)
    {
        UObject* o = InSDKUtils::GetObjectByIndex(i);
        if (!o) continue;
        uint64_t vt = 0;
        if (!SafeRead(reinterpret_cast<uintptr_t>(o), &vt, 8) || !InModule(vt)) continue;
        int best = -1;
        std::string cands;
        for (int slot = 20; slot < 200; slot++)
        {
            uint64_t fn = 0;
            if (!SafeRead(vt + 8ul * slot, &fn, 8) || !InText(fn)) break;
            int sc = ProcessEventScore(fn);
            if (sc < 3) continue;                         // needs both FunctionFlags and ParmsSize reads
            char b[48]; snprintf(b, sizeof(b), " %d:0x%llx(%d)", slot, (unsigned long long)(fn - base), sc); cands += b;
            // the Android (Itanium) slot is 77 and MSVC has one destructor slot less: prefer the candidate closest to 76 (UE4SS finds the same function)
            if (best < 0 || std::abs(slot - 76) < std::abs(best - 76)) best = slot;
        }
        if (best < 0) continue;
        uint64_t fn = 0; SafeRead(vt + 8ul * best, &fn, 8);
        KLog(KLOG_INFO, "resolver", "ProcessEvent candidates:%s", cands.c_str());
        g_peSlot = best;
        Offsets::ProcessEventIdx = best;
        Offsets::ProcessEvent = fn - base;
        Offsets::GWorld = 0;
        char b[100];
        snprintf(b, sizeof(b), "ProcessEvent: vtable slot %d -> base+0x%llx", best, (unsigned long long)Offsets::ProcessEvent);
        report += b;
        return true;
    }
    report += "ProcessEvent not found";
    return false;
}

int Resolver_ProcessEventSlot() { return g_peSlot; }

// The interpreter: ProcessInternal (the shared ExecFunction) ends by jumping to / calling the loop that dispatches through GNatives.
// The loop is recognised by the table it loads: a lea of an address in the image's data holding 256 pointers into code (GNatives).
bool Resolver_FindInterpreter(uintptr_t base, uintptr_t pi, ScriptAddrs& out, std::string& report)
{
    LoadSegs();
    out = ScriptAddrs();
    std::vector<uintptr_t> targets;
    for (uintptr_t a = pi; a < pi + 0x300;)                                                   // direct branches out of ProcessInternal
    {
        X64::Insn i = X64::Decode(a);
        if (!i.valid) break;
        if (i.relBranch && (i.isJmp || i.isCall) && i.target > pi && i.target < pi + 0x1000) targets.push_back(i.target);
        a += i.len;
    }
    for (uintptr_t a = (pi + 0x10) & ~uintptr_t(0xF); a < pi + 0x800; a += 0x10)               // and functions that follow it (padding in front)
    {
        uint8_t prev = 0;
        if (SafeRead(a - 1, &prev, 1) && (prev == 0xCC || prev == 0x90)) targets.push_back(a);
    }
    std::string tried;
    uintptr_t weak = 0;
    for (uintptr_t t : targets)
    {
        bool indexed = false;
        uintptr_t table = 0;
        uintptr_t a = t;
        for (int n = 0; n < 120 && a < t + 0x300; n++)
        {
            X64::Insn i = X64::Decode(a);
            if (!i.valid) break;
            if ((i.memCall || i.isMovLoad) && i.memHasIndex && i.memScale == 8) indexed = true;       // GNatives[opcode]: call/mov [reg + reg*8]
            if (!table && i.isLea && i.ripMem && InModule(i.ripTarget) && !InText(i.ripTarget))
            {
                uintptr_t e[256];
                int good = 0;
                if (SafeRead(i.ripTarget, e, sizeof(e))) for (int q = 0; q < 256; q++) if (InText(e[q])) good++;
                if (good >= 240) table = i.ripTarget;
            }
            a += i.len;
        }
        char b[72]; snprintf(b, sizeof(b), " 0x%llx(idx:%d tbl:%d)", (unsigned long long)(t - base), indexed, table != 0); tried += b;
        if (table) { out.plsf = t; out.gnatives = table; break; }
        if (indexed && !weak) weak = t;
    }
    if (!out.plsf && weak) out.plsf = weak;
    char b[300];
    snprintf(b, sizeof(b), "interpreter: candidates%s -> plsf base+0x%llx, GNatives base+0x%llx", tried.c_str(), (unsigned long long)(out.plsf ? out.plsf - base : 0), (unsigned long long)(out.gnatives ? out.gnatives - base : 0));
    report = b;
    return out.plsf != 0;
}

void Resolver_FindBytes(const void* pat, size_t n, std::vector<uintptr_t>& hits)
{
    LoadSegs();
    for (auto& s : g_segs)
    {
        if (s.flags & PF_X) continue;                    // string literals live in data sections
        std::vector<uint8_t> buf; ReadSeg(s, buf);
        const uint8_t* p = buf.data(); const uint8_t* end = p + buf.size();
        const uint8_t first = *static_cast<const uint8_t*>(pat);
        while (p + n <= end)
        {
            p = static_cast<const uint8_t*>(memchr(p, first, end - p - n + 1));
            if (!p) break;
            if (memcmp(p, pat, n) == 0) hits.push_back(s.lo + (p - buf.data()));
            p++;
        }
    }
}

static uint32_t Crc32(const uint8_t* d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}

std::string Resolver_BuildKey()
{
    LoadSegs();
    if (g_segs.empty()) return "";
    uint32_t crc = 0;
    for (auto& s : g_segs)
        if (s.flags & PF_X)
        {
            std::vector<uint8_t> v(std::min<size_t>(s.hi - s.lo, 1u << 18));
            if (SafeRead(s.lo, v.data(), v.size())) crc = Crc32(v.data(), v.size());
            break;
        }
    char b[96];
    snprintf(b, sizeof(b), "%llx-%08x-%08x", (unsigned long long)(g_modHi - g_modLo), g_timestamp, crc);
    return b;
}

bool Resolver_SafeRead(uintptr_t addr, void* out, size_t n) { return SafeRead(addr, out, n); }
bool Resolver_InText(uintptr_t addr) { LoadSegs(); return InText(addr); }
bool Resolver_InImage(uintptr_t addr) { LoadSegs(); return InModule(addr); }

