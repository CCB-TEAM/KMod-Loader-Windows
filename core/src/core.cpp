// kmod mod core (Windows): hooks UObject::ProcessEvent (and the Blueprint interpreter), gets onto the game thread, and runs the Lua host.
// Shared with the Android core except for this file's platform layers: the x64 hook engine (x64.cpp), the PE resolver (resolver.cpp),
// thread identification and start-up (the dwmapi proxy calls kmod_start()).
#include <algorithm>
#include <array>
#include <utility>
#include <cstddef>
#include <map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <tlhelp32.h>
#include <set>

#include "kmod_common.h"
#include "kmod_log.h"
#include "kmod_version.h"
#include "lua_host.h"
#include "resolver.h"
#include "kmod_boot.h"
#include "net_redirect.h"
#include "reflect.h"
#include "x64.h"
#include "win/dev_console.h"
#include "win/overlay.h"
#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"

using namespace SDK;

void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    KLogMsg(KLOG_INFO, "core", buf);
}

static bool InstallInlineHook(void* target, void* detour, void** outTrampoline) { return KMod_InstallHook(target, detour, outTrampoline); }

// Which thread is the game thread? UE names it "GameThread" (SetThreadDescription); if that is not available it is the process's first thread.
static std::string CurrentThreadName()
{
    using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static GetDescFn fn = reinterpret_cast<GetDescFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    std::string out;
    PWSTR w = nullptr;
    if (fn && SUCCEEDED(fn(GetCurrentThread(), &w)) && w) { out = KUtf8(w); LocalFree(w); }
    return out;
}

static DWORD MainThreadId()
{
    static DWORD tid = [] {
        DWORD best = 0; ULONGLONG bestT = ~0ull;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return best;
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te))
            do
            {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID))
                {
                    FILETIME c, e, k, u;
                    if (GetThreadTimes(h, &c, &e, &k, &u))
                    {
                        ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
                        if (t < bestT) { bestT = t; best = te.th32ThreadID; }
                    }
                    CloseHandle(h);
                }
            } while (Thread32Next(snap, &te));
        CloseHandle(snap);
        return best;
    }();
    return tid;
}

// ---------------------------------------------------------------------------------------------
// ProcessEvent detour -> game-thread tick
// ---------------------------------------------------------------------------------------------
using ProcessEventFn = void (*)(UObject*, UFunction*, void*);
static ProcessEventFn g_origPE = nullptr;
static std::atomic<uint64_t> g_peCalls{0};
static std::atomic<uint64_t> g_peTotal{0};
static std::atomic<long long> g_peTicks{0};            // time spent inside the original ProcessEvent (outermost calls)
static std::atomic<uint64_t> g_peTop{0};
static std::atomic<bool> g_diag{false};                // collect which functions each path sees (kmod.script_diag)
static std::unordered_set<UFunction*> g_diagPI, g_diagPE;   // game thread only
static std::atomic<uint64_t> g_peScript{0};           // ProcessEvent calls of Blueprint (script) functions            // every ProcessEvent call (statistics)
static thread_local int t_gameThread = -1; // -1 unknown, 0 no, 1 yes
static thread_local bool t_busy = false;

static void OnGameTick();
static UObject* CurrentWorld();
// Heartbeat: an engine timer (K2_SetTimer, every ~16 ms) calls Actor.ForceNetUpdate on the PlayerController. The call goes
// through ProcessEvent, so it drives the tick even on a static screen where the game itself calls almost nothing.
static UFunction* g_hbFn = nullptr;

// ---------------------------------------------------------------------------------------------
// Script (Blueprint) function hooks. A Blueprint function runs through ProcessInternal(Context, Stack, Result): the VM calls
// it directly for Blueprint-to-Blueprint calls (no ProcessEvent), and ProcessEvent reaches it too. By the time it is entered
// the caller has evaluated the arguments into the frame's Locals, so a hook sees the parameters, can change them before the
// function runs, and (post hook) read the return value. Native functions keep going through the ProcessEvent hook.
// FFrame (UE 5.x): FOutputDevice base (16 bytes), Node, Object, Code, Locals.
// ---------------------------------------------------------------------------------------------
// FFrame field offsets. The defaults are UE 5.x's; they are re-learned at run time from the first ProcessEvent calls of Blueprint
// functions: ProcessEvent builds the frame itself, so we know what it must contain (function, object, parameter block, the
// function's bytecode pointer) and look for those values in the frame the interpreter receives.
static int kFrameNode = 0x10, kFrameObject = 0x18, kFrameLocals = 0x28, kCodeOff = 0x20;
static bool g_frameLearned = false;
struct FrameExpect { UFunction* fn = nullptr; UObject* self = nullptr; void* parms = nullptr; uintptr_t script = 0; uint64_t head = 0; bool hasHead = false; };
static thread_local FrameExpect t_expect;
static int g_fvNode[26], g_fvObj[26], g_fvLoc[26], g_fvCode[26], g_fSamples = 0;

static int g_fHeadSamples = 0;

static void LearnFrame(const uint8_t* st)
{
    FrameExpect e = t_expect; t_expect = FrameExpect();
    if (!e.fn || g_frameLearned) return;
    g_fSamples++;
    if (e.hasHead) g_fHeadSamples++;
    for (int k = 0; k < 26; k++)
    {
        uintptr_t w = *reinterpret_cast<const uintptr_t*>(st + k * 8);
        g_fvNode[k] += w == (uintptr_t)e.fn; g_fvObj[k] += w == (uintptr_t)e.self;
        g_fvCode[k] += (e.script && w == e.script);
        // Locals: ProcessEvent copies the parameters into the frame's own block, so look for a pointer to memory that starts with them
        uint64_t first = 0;
        if (e.hasHead && w > 0x10000 && w != (uintptr_t)e.parms && Resolver_SafeRead(w, &first, 8) && first == e.head) g_fvLoc[k]++;
    }
    if (g_fSamples < 60 || (g_fHeadSamples < 40 && g_fSamples < 600)) return;
    auto best = [&](const int* v, int& off) { int b = 0; for (int k = 1; k < 26; k++) if (v[k] > v[b]) b = k; off = b * 8; return v[b]; };
    int n, o, l, c;
    int vn = best(g_fvNode, n), vo = best(g_fvObj, o), vl = best(g_fvLoc, l), vc = best(g_fvCode, c);
    bool core3 = vn * 10 >= g_fSamples * 9 && vo * 10 >= g_fSamples * 9 && n != o;
    bool loc = g_fHeadSamples >= 40 && vl * 10 >= g_fHeadSamples * 8 && l != n && l != o;
    bool code = vc * 10 >= g_fSamples * 8 && c != n && c != o && c != l;
    if (core3) { kFrameNode = n; kFrameObject = o; }
    if (core3 && loc) kFrameLocals = l;
    if (core3 && code) kCodeOff = c;
    g_frameLearned = true;
    KLog(core3 && loc && code ? KLOG_INFO : KLOG_WARN, "core",
         "script hook: FFrame offsets (learned: Node %d, Object %d, Locals %d, Code %d): Node +0x%x Object +0x%x Locals +0x%x Code +0x%x; votes %d/%d/%d/%d of %d (%d with parameters)",
         core3, core3, core3 && loc, core3 && code, kFrameNode, kFrameObject, kFrameLocals, kCodeOff, vn, vo, vl, vc, g_fSamples, g_fHeadSamples);
}

using ProcessInternalFn = void (*)(UObject*, void*, void*);
static ProcessInternalFn g_origPI = nullptr;
static std::atomic<bool> g_scriptHookOk{false};          // installed and the frame layout was confirmed
static std::atomic<int> g_piChecked{0};
static std::atomic<bool> g_piBroken{false};
static std::atomic<uint64_t> g_piTotal{0};            // every script function call that went through the detour

static std::atomic<int> g_frameChain{-1};              // FFrame::PropertyChainForCompiledIn offset (learned from script frames)
static int g_chainSamples = 0, g_chainVotes[14] = {};

static void HookedProcessInternal(UObject* ctx, void* stack, void* result)
{
    g_piTotal.fetch_add(1, std::memory_order_relaxed);
    if (!g_scriptHookOk.load(std::memory_order_relaxed) && g_piBroken.load()) { g_origPI(ctx, stack, result); return; }
    if (t_gameThread == 1 && !t_busy && stack)
    {
        if (t_expect.fn) LearnFrame(reinterpret_cast<const uint8_t*>(stack));
        ctx = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(stack) + kFrameObject);      // the interpreter's own Context
        UFunction* fn = *reinterpret_cast<UFunction**>(reinterpret_cast<uint8_t*>(stack) + kFrameNode);
        if (g_piChecked.load(std::memory_order_relaxed) < 2000)          // confirm the frame layout on the first calls
        {
            UObject* obj = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(stack) + kFrameObject);
            if (obj && fn && Rf::ClassOf(reinterpret_cast<UObject*>(fn)) == Rf::FunctionClass()) g_piChecked++;
            else { g_piBroken = true; g_scriptHookOk = false; KLog(KLOG_ERROR, "core", "script hook: unexpected FFrame layout, switched off"); g_origPI(ctx, stack, result); return; }
            if (g_piChecked.load() >= 2000) KLog(KLOG_INFO, "core", "script hook: frame layout confirmed on 2000 calls");
        }
        if (g_diag.load(std::memory_order_relaxed) && fn && g_diagPI.size() < 20000) g_diagPI.insert(fn);
        if (g_frameChain.load(std::memory_order_relaxed) < 0 && g_chainSamples < 300 && fn)       // learn where FFrame keeps its property chain
        {
            if (FField* pr = Rf::PropsOf(reinterpret_cast<UStruct*>(fn)))
            {
                g_chainSamples++;
                for (int k = 0; k < 14; k++)
                    if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(stack) + 0x30 + k * 8) == pr) g_chainVotes[k]++;
                if (g_chainSamples == 300)
                {
                    int best = 0;
                    for (int k = 1; k < 14; k++) if (g_chainVotes[k] > g_chainVotes[best]) best = k;
                    if (g_chainVotes[best] >= 280) { g_frameChain = 0x30 + best * 8; KLog(KLOG_INFO, "core", "script hook: FFrame property chain at +0x%x", (int)g_frameChain.load()); }
                    else KLog(KLOG_WARN, "core", "script hook: FFrame property chain not found (best %d/300): native hooks will see parameters but not change them", g_chainVotes[best]);
                }
            }
        }
        if (fn && LuaHost_HasHook(fn))
        {
            uint8_t* locals = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(stack) + kFrameLocals);
            t_busy = true; LuaHost_PreHook(ctx, fn, locals, result); t_busy = false;
            g_origPI(ctx, stack, result);
            t_busy = true; LuaHost_PostHook(ctx, fn, locals, result); t_busy = false;
            return;
        }
    }
    g_origPI(ctx, stack, result);
}

// ---------------------------------------------------------------------------------------------
// Native function hooks for calls the Blueprint VM makes directly (they never reach ProcessEvent). The VM calls a native
// function through UFunction::Func(Context, Stack, Result) with the arguments still sitting in the caller's BYTECODE; the
// function's exec thunk evaluates them. A hooked native function gets a small per-function thunk in place of Func: it
// evaluates the arguments itself with the engine's own opcode table (GNatives, found at run time), runs the Lua hooks on that
// buffer, and calls the original thunk with a synthetic frame (Code = null, Locals = our buffer: the same shape ProcessEvent
// uses), so changed arguments reach the function. Calls that come through ProcessEvent are left to the ProcessEvent detour.
// Not supported: functions with non-const reference/out parameters (their value lives in the caller's variable).
// ---------------------------------------------------------------------------------------------
using NativeFn = void (*)(UObject*, void*, void*);
struct NatSlot { UFunction* fn = nullptr; NativeFn orig = nullptr; std::vector<int> offs; size_t bufSize = 0; bool vmOk = false; };
static const int kNatSlots = 64;
static NatSlot g_nat[kNatSlots];
static std::atomic<int> g_natCount{0};
static uintptr_t* g_gnatives = nullptr;
static std::atomic<uint64_t> g_natVmCalls{0};

static void NatDispatch(int n, UObject* self, void* stack, void* result)
{
    NatSlot& s = g_nat[n];
    uint8_t* st = reinterpret_cast<uint8_t*>(stack);
    if (!s.vmOk || t_gameThread != 1 || t_busy || !st || !LuaHost_HasHook(s.fn)) { s.orig(self, stack, result); return; }
    uint8_t*& code = *reinterpret_cast<uint8_t**>(st + kCodeOff);
    if (*reinterpret_cast<UFunction**>(st + kFrameNode) == s.fn || !code) { s.orig(self, stack, result); return; }   // ProcessEvent path
    UObject* ctxObj = *reinterpret_cast<UObject**>(st + kFrameObject);
    uint8_t* const start = code;
    std::vector<uint8_t> buf(s.bufSize, 0);
    for (int off : s.offs)                                                       // FFrame::Step for each argument, in declaration order
    {
        uint8_t op = *code++;
        reinterpret_cast<NativeFn>(g_gnatives[op])(ctxObj, stack, buf.data() + off);
    }
    if (*code != 0x16)                                                           // EX_EndFunctionParms expected: otherwise let the function read its own arguments
    {
        code = start;
        s.orig(self, stack, result);
        return;
    }
    g_natVmCalls.fetch_add(1, std::memory_order_relaxed);
    uint8_t* const after = code + 1;
    t_busy = true; LuaHost_PreHook(self, s.fn, buf.data(), result); t_busy = false;
    int chain = g_frameChain.load(std::memory_order_relaxed);
    if (chain >= 0)
    {
        uint8_t fake[0xC0];
        memcpy(fake, st, sizeof(fake));
        *reinterpret_cast<UFunction**>(fake + kFrameNode) = s.fn;
        *reinterpret_cast<uint8_t**>(fake + kCodeOff) = nullptr;
        *reinterpret_cast<uint8_t**>(fake + kFrameLocals) = buf.data();
        *reinterpret_cast<FField**>(fake + chain) = Rf::PropsOf(reinterpret_cast<UStruct*>(s.fn));
        s.orig(self, fake, result);
        code = after;
    }
    else { code = start; s.orig(self, stack, result); }                          // read-only: the function evaluates the original bytecode again
    t_busy = true; LuaHost_PostHook(self, s.fn, buf.data(), result); t_busy = false;
}

template <int N> static void NatThunk(UObject* self, void* stack, void* result) { NatDispatch(N, self, stack, result); }
template <int... I> static std::array<NativeFn, sizeof...(I)> MakeThunks(std::integer_sequence<int, I...>) { return {{&NatThunk<I>...}}; }

// Called when a hook is registered. A Blueprint function needs nothing (the interpreter hook sees it). Returns false with a
// note when calls the Blueprint VM makes to this native function will not be seen.
bool KMod_EnsureNativeHook(UFunction* fn, std::string& note)
{
    if (!fn || !(Rf::FuncFlagsOf(fn) & 0x400)) return true;
    int cnt = g_natCount.load();
    for (int i = 0; i < cnt; i++) if (g_nat[i].fn == fn) return g_nat[i].vmOk;
    if (!g_gnatives) { note = "calls the Blueprint VM makes to this native function cannot be seen (opcode table not found); calls through ProcessEvent still are"; return false; }
    if (cnt >= kNatSlots) { note = "too many native function hooks"; return false; }
    uintptr_t* slot = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(fn) + Rf::L.ufunc_exec);
    if (!*slot) { note = "the function has no native entry"; return false; }
    NatSlot& s = g_nat[cnt];
    s.fn = fn; s.orig = reinterpret_cast<NativeFn>(*slot); s.vmOk = true;
    size_t sz = Rf::ParamSizeOf(fn);
    for (FField* f = Rf::PropsOf(reinterpret_cast<UStruct*>(fn)); f; f = Rf::NextOf(f))
    {
        FProperty* p = reinterpret_cast<FProperty*>(f);
        uint64_t fl = Rf::PropFlagsOf(p);
        if (!(fl & 0x80)) continue;
        { int64_t end = (int64_t)Rf::OffsetOf(p) + (int64_t)Rf::ElemSizeOf(p) * std::max(1, Rf::ArrayDimOf(p)); if (end >= 0 && end <= 0x10000) sz = std::max<size_t>(sz, (size_t)end); }
        if (fl & 0x400) continue;                                                // return value: not in the bytecode
        if ((fl & 0x100) && !(fl & 0x2)) s.vmOk = false;                         // non-const out/reference parameter
        s.offs.push_back(Rf::OffsetOf(p));
    }
    s.bufSize = sz + 64;
    static const auto thunks = MakeThunks(std::make_integer_sequence<int, kNatSlots>());
    g_natCount.store(cnt + 1);
    if (!s.vmOk) { note = "the function has a reference/out parameter: calls from Blueprint cannot be hooked"; return false; }
    *slot = reinterpret_cast<uintptr_t>(thunks[cnt]);
    Log("native hook: %s (%d arguments%s)", Rf::PathName(reinterpret_cast<UObject*>(fn)).c_str(), (int)s.offs.size(), g_frameChain.load() >= 0 ? "" : ", read-only");
    return true;
}

void KMod_NativeStats(unsigned long long& vmCalls, int& slots, bool& haveTable, int& chain)
{
    vmCalls = g_natVmCalls.load(); slots = g_natCount.load(); haveTable = g_gnatives != nullptr; chain = g_frameChain.load();
}

static double NowSec()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void HookedProcessEvent(UObject* self, UFunction* fn, void* parms)
{
    g_peTotal.fetch_add(1, std::memory_order_relaxed);
    if (fn && (Rf::FuncFlagsOf(fn) & 0x400) == 0)
    {
        g_peScript.fetch_add(1, std::memory_order_relaxed);
        if (g_diag.load(std::memory_order_relaxed) && t_gameThread == 1 && g_diagPE.size() < 20000) g_diagPE.insert(fn);
    }
    if (t_gameThread < 0)
    {
        std::string name = CurrentThreadName();
        const DWORD tid = GetCurrentThreadId();
        t_gameThread = (name == "GameThread" || (name.empty() && tid == MainThreadId())) ? 1 : 0;
        if (t_gameThread == 1) KBoot_SetGameThread((int)tid);
        Log("ProcessEvent first seen on thread '%s' tid=%d (main thread %d) -> gameThread=%d", name.c_str(), (int)tid, (int)MainThreadId(), t_gameThread);
    }

    // a Blueprint function is handled by the ProcessInternal detour (which also sees it when ProcessEvent runs it)
    const bool hooked = t_gameThread == 1 && !t_busy && LuaHost_HasHook(fn) &&
                        !(g_scriptHookOk.load(std::memory_order_relaxed) && (Rf::FuncFlagsOf(fn) & 0x400) == 0);
    if (hooked) { t_busy = true; LuaHost_PreHook(self, fn, parms); t_busy = false; }

    if (!g_frameLearned && t_gameThread == 1 && g_scriptHookOk.load(std::memory_order_relaxed) && fn && (Rf::FuncFlagsOf(fn) & 0x400) == 0 && parms)
    {
        t_expect.fn = fn; t_expect.self = self; t_expect.parms = parms;
        t_expect.hasHead = Rf::ParamSizeOf(fn) >= 8 && Resolver_SafeRead(reinterpret_cast<uintptr_t>(parms), &t_expect.head, 8) && t_expect.head > 0x10000;   // an object pointer or similar: not a common value
        t_expect.script = Rf::At<uintptr_t>(fn, Rf::L.ustruct_script);          // UStruct::Script (a TArray): its data pointer is the frame's Code at entry
    }
    {
        static thread_local int depth = 0;
        LARGE_INTEGER a; QueryPerformanceCounter(&a);
        depth++;
        g_origPE(self, fn, parms);
        depth--;
        if (depth == 0) { LARGE_INTEGER b; QueryPerformanceCounter(&b); g_peTicks.fetch_add(b.QuadPart - a.QuadPart, std::memory_order_relaxed); g_peTop.fetch_add(1, std::memory_order_relaxed); }
    }
    t_expect = FrameExpect();

    if (t_busy) return;
    if (t_gameThread != 1) return;
    if (hooked) { t_busy = true; LuaHost_PostHook(self, fn, parms); t_busy = false; }

    static double lastTick = 0;
    const bool beat = (fn == g_hbFn);
    if (!beat && (++g_peCalls & 0x0F) != 0) return;
    double now = NowSec();
    static double lastFast = 0;
    if (LuaHost_HasFast() && now - lastFast >= 0.004)          // fast lane (FastLoop): every few ms, only when a mod asked for it
    {
        lastFast = now;
        t_busy = true;
        LuaHost_FastTick(now);
        t_busy = false;
    }
    if (now - lastTick < 0.05) return; // ~20 Hz
    lastTick = now;

    t_busy = true;
    OnGameTick();
    t_busy = false;
}

// ---------------------------------------------------------------------------------------------
// Pak mods do not exist on Windows (the loader has no paks folder); the entry points stay so the shared Lua host links unchanged.
// ---------------------------------------------------------------------------------------------
bool KMod_MountPak(const std::string&) { return false; }
void KMod_MountPendingPaks() {}
std::vector<std::pair<std::string, std::string>> KMod_PakStatus() { return {}; }

// ---------------------------------------------------------------------------------------------
// Layout verification. The Android loader runs a dumper in the game; on Windows the layout defaults (reflect.h, taken from UE4SS's
// 5.6 data for this game) are checked against the live engine instead: a sample of objects, functions and properties must be
// consistent with them. A failure blocks the mods (like the canaries below).
// ---------------------------------------------------------------------------------------------
static std::atomic<int> g_layoutState{0};              // 0 idle, 1 running, 2 done
static std::atomic<bool> g_layoutGate{false};          // open = the Lua host may start (verification finished)
static std::atomic<bool> g_layoutMismatch{false};      // the engine's layout differs from what this core assumes
static std::string g_layoutReport = "layout verification has not run";
static std::mutex g_layoutMu;

static void LayoutMain()
{
    struct Gate { ~Gate() { g_layoutState = 2; g_layoutGate = true; } } gate;
    TUObjectArray* arr = InSDKUtils::GetGObjects();
    UClass* fcls = Rf::FunctionClass();
    int32_t n = arr ? arr->Num() : 0;
    int objTotal = 0, objGood = 0, fnTotal = 0, fnGood = 0, propTotal = 0, propGood = 0, superTotal = 0, superGood = 0;
    for (int32_t i = 0; i < n; i += (n > 6000 ? n / 6000 : 1))
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        if (!o || !ObjRef::ItemUsable(it)) continue;
        objTotal++;
        UClass* c = Rf::ClassOf(o);
        uintptr_t vt = 0;
        bool ok = c && Resolver_SafeRead(reinterpret_cast<uintptr_t>(o), &vt, 8) && Resolver_InImage(vt) && !Rf::NameStr(o).empty() && !Rf::NameStr(reinterpret_cast<UObject*>(c)).empty();
        if (ok) objGood++;
        if (!c || !ok) continue;
        if (c == Rf::ClassClass() || Rf::IsA(o, Rf::ClassClass()))                    // a class: its super chain ends
        {
            superTotal++;
            UStruct* s = reinterpret_cast<UStruct*>(o);
            int depth = 0;
            while (s && depth < 40) { s = Rf::SuperOf(s); depth++; }
            if (!s) superGood++;
        }
        if (c == fcls && fnTotal < 600)
        {
            UFunction* f = reinterpret_cast<UFunction*>(o);
            fnTotal++;
            uint32_t fl = Rf::FuncFlagsOf(f);
            uintptr_t exec = Rf::At<uintptr_t>(f, Rf::L.ufunc_exec);
            bool fok = Rf::ParamSizeOf(f) <= 0x4000 && (!(fl & 0x400) || (exec && Resolver_InText(exec)));       // a native function has its thunk in the engine
            if (fok) fnGood++;
            for (FField* p = Rf::PropsOf(reinterpret_cast<UStruct*>(f)); p && propTotal < 4000; p = Rf::NextOf(p))
            {
                FProperty* fp = reinterpret_cast<FProperty*>(p);
                if (!(Rf::PropFlagsOf(fp) & 0x80)) continue;                         // CPF_Parm
                propTotal++;
                int64_t end = (int64_t)Rf::OffsetOf(fp) + (int64_t)Rf::ElemSizeOf(fp) * std::max(1, Rf::ArrayDimOf(fp));
                if (Rf::OffsetOf(fp) >= 0 && Rf::ElemSizeOf(fp) > 0 && end <= (int64_t)Rf::ParamSizeOf(f) && !Rf::NameStr(p).empty() && Rf::ClassOf(p)) propGood++;
            }
        }
    }
    auto pct = [](int g, int t) { return t ? g * 100 / t : 0; };
    bool ok = objTotal >= 200 && pct(objGood, objTotal) >= 99 && fnTotal >= 50 && pct(fnGood, fnTotal) >= 97 && propTotal >= 50 && pct(propGood, propTotal) >= 97 &&
              superTotal >= 20 && pct(superGood, superTotal) >= 99;
    char b[300];
    snprintf(b, sizeof(b), "engine layout %s: objects %d/%d, class chains %d/%d, functions %d/%d, parameters %d/%d consistent with the layout in this core",
             ok ? "verified" : "MISMATCH", objGood, objTotal, superGood, superTotal, fnGood, fnTotal, propGood, propTotal);
    { std::lock_guard<std::mutex> g(g_layoutMu); g_layoutReport = b; if (!ok) g_layoutReport += " -> mods are NOT loaded"; }
    if (!ok) g_layoutMismatch = true;
    KLog(ok ? KLOG_INFO : KLOG_ERROR, "layout", "%s", b);
}

// Starts the verification in the background (force: run again).
bool KMod_DetectLayout(bool force)
{
    int expected = 0;
    if (force) { g_layoutState = 0; g_layoutMismatch = false; }
    if (!g_layoutState.compare_exchange_strong(expected, 1) && !force) return false;
    if (force) g_layoutState = 1;
    std::thread(LayoutMain).detach();
    return true;
}

std::string KMod_LayoutReport()
{
    std::lock_guard<std::mutex> g(g_layoutMu);
    return std::string(g_layoutState == 1 ? "running... " : "") + g_layoutReport;
}

bool KMod_LayoutMismatch() { return g_layoutMismatch.load(); }

// Diagnostics: kmod.script_diag(true) starts collecting, kmod.script_diag(false) stops and returns the paths of the script
// functions that ran through ProcessInternal but never through ProcessEvent: calls the Blueprint VM made directly.
std::vector<std::string> KMod_ScriptDiag(bool start)
{
    std::vector<std::string> out;
    if (start) { g_diagPI.clear(); g_diagPE.clear(); g_diag = true; return out; }
    g_diag = false;
    for (UFunction* f : g_diagPI)
        if (!g_diagPE.count(f) && out.size() < 40) out.push_back(Rf::PathName(reinterpret_cast<UObject*>(f)));
    out.push_back("-- seen via ProcessInternal: " + std::to_string(g_diagPI.size()) + ", via ProcessEvent: " + std::to_string(g_diagPE.size()));
    g_diagPI.clear(); g_diagPE.clear();
    return out;
}

// Counters for the script-function hook: calls that went through ProcessInternal, and through ProcessEvent.
void KMod_ScriptStats(unsigned long long& pi, unsigned long long& pe, bool& installed)
{
    pi = g_piTotal.load(); pe = g_peScript.load(); installed = g_scriptHookOk.load();   // pe here = ProcessEvent calls of script functions
}

// Finds ProcessInternal as the ExecFunction that every script (non-native) UFunction shares, and hooks the interpreter behind it.
static void EnsureScriptHook(uintptr_t base)
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    UClass* fcls = Rf::FunctionClass();
    TUObjectArray* arr = InSDKUtils::GetGObjects();
    if (!fcls || !arr) return;
    std::map<uintptr_t, int> votes;
    int seen = 0;
    for (int32_t i = 0; i < arr->Num() && seen < 600; i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        if (!o || !ObjRef::ItemUsable(it) || Rf::ClassOf(o) != fcls) continue;
        if (Rf::FuncFlagsOf(reinterpret_cast<UFunction*>(o)) & 0x400) continue;            // native
        uintptr_t f = Rf::At<uintptr_t>(o, Rf::L.ufunc_exec);
        if (f) { votes[f]++; seen++; }
    }
    uintptr_t best = 0; int bestN = 0;
    for (auto& kv : votes) if (kv.second > bestN) { best = kv.first; bestN = kv.second; }
    if (!best || seen < 50 || bestN * 10 < seen * 9)
    {
        KLog(KLOG_WARN, "core", "script hook: no common ProcessInternal found (%d of %d); Blueprint-internal calls stay invisible", bestN, seen);
        return;
    }
    Log("script hook: ProcessInternal = base+0x%llx (%d/%d script functions share it)", (unsigned long long)(best - base), bestN, seen);
    // ProcessInternal hands over to the bytecode interpreter (a loop dispatching through GNatives[opcode]); the Blueprint VM calls that
    // interpreter directly for Blueprint-to-Blueprint calls, so it is the one place every script call passes.
    ScriptAddrs sa;
    std::string rep;
    bool found = Resolver_FindInterpreter(base, best, sa, rep);
    Log("script hook: %s", rep.c_str());
    if (sa.gnatives) { g_gnatives = reinterpret_cast<uintptr_t*>(sa.gnatives); Log("GNatives at base+0x%llx", (unsigned long long)(sa.gnatives - base)); }
    else KLog(KLOG_WARN, "core", "script hook: opcode table (GNatives) not found; native functions called from Blueprint stay invisible");
    uintptr_t hookAt = found ? sa.plsf : best;
    if (!found) KLog(KLOG_WARN, "core", "script hook: interpreter not found behind ProcessInternal; hooking ProcessInternal itself (Blueprint-to-Blueprint calls may stay invisible)");
    void* tramp = nullptr;
    if (!InstallInlineHook(reinterpret_cast<void*>(hookAt), reinterpret_cast<void*>(&HookedProcessInternal), &tramp))
    {
        KLog(KLOG_WARN, "core", "script hook: cannot hook base+0x%llx; Blueprint-internal calls stay invisible", (unsigned long long)(hookAt - base));
        return;
    }
    g_origPI = reinterpret_cast<ProcessInternalFn>(tramp);
    g_scriptHookOk = true;
    Log("script hook installed at base+0x%llx", (unsigned long long)(hookAt - base));
}

static double g_readySince = 0;
static double g_lastHeartbeatArm = 0;

// Game thread. Arms (or re-arms: the timer is replaced, not duplicated) the heartbeat; also follows world changes.
static void ArmHeartbeat(double now)
{
    if (now - g_lastHeartbeatArm < 2.0) return;
    g_lastHeartbeatArm = now;
    UObject* world = CurrentWorld();
    if (!world) return;
    Rf::Args gp = Rf::Prepare("GameplayStatics", "GetPlayerController");
    gp.Set("WorldContextObject", world).Set("PlayerIndex", (int32_t)0);
    gp.Call(Rf::CdoFor("GameplayStatics"));
    UObject* pc = gp.Get<UObject*>("ReturnValue");
    if (!pc) return;
    if (!g_hbFn) g_hbFn = Rf::FindFunc(reinterpret_cast<UStruct*>(Rf::ClassOf(pc)), "ForceNetUpdate");
    if (!g_hbFn) return;
    Rf::Args t = Rf::Prepare("KismetSystemLibrary", "K2_SetTimer");
    static const char16_t kName[] = u"ForceNetUpdate";
    t.Set("Object", pc).Set("FunctionName", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kName)), 15, 15));
    t.Set("Time", 0.016f).Set("bLooping", true).Set("bMaxOncePerFrame", false).Set("InitialStartDelay", 0.f).Set("InitialStartDelayVariance", 0.f);
    t.Call(Rf::CdoFor("KismetSystemLibrary"));
}

// The game engine object (cached), its viewport and world: read by property name through the runtime reflection layer.
static UObject* CurrentWorld()
{
    static UObject* engine = nullptr;
    static int32_t engineIdx = -1;
    if (engine)                                                   // still the same live object?
    {
        FUObjectItem* it = ObjRef::ItemAt(engineIdx);
        if (!it || it->Object != engine || !ObjRef::ItemUsable(it)) engine = nullptr;
    }
    if (!engine)
    {
        engine = Rf::FindFirst("GameEngine");
        if (!engine) return nullptr;
        engineIdx = Rf::IndexOf(engine);
    }
    UObject* vp = Rf::ObjProp(engine, "GameViewport");
    return vp ? Rf::ObjProp(vp, "World") : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Canaries: the layouts that come from the SDK compiled into this core (object table geometry, FString, FText, TArray) are checked
// against the running game once the engine is up. A wrong object table or FString layout would make every mod read garbage, so those
// two block the mods (like a reflection layout mismatch); FText and TArray only warn (the features that use them report errors).
// ---------------------------------------------------------------------------------------------
static std::string g_canaryReport;

static void RunCanaries()
{
    std::string rep;
    bool block = false;

    // 1. object table: every object's own Index must equal its slot (validates Objects/chunk layout, FUObjectItem stride and UObject.Index)
    {
        TUObjectArray* arr = InSDKUtils::GetGObjects();
        int total = 0, good = 0;
        int32_t n = arr ? arr->Num() : 0;
        int32_t step = n > 4000 ? n / 4000 : 1;
        for (int32_t i = 0; i < n; i += step)
        {
            FUObjectItem* it = ObjRef::ItemAt(i);
            if (!it || !it->Object) continue;
            total++;
            if (Rf::IndexOf(it->Object) == i) good++;
        }
        bool ok = total >= 200 && good * 100 >= total * 99;
        char b[160]; snprintf(b, sizeof(b), "object table %s (%d/%d objects at their own index)", ok ? "ok" : "WRONG", good, total);
        rep += b; if (!ok) block = true;
    }

    // 2. FString: concatenate through the engine and read the result back
    {
        static const char16_t kAb[] = u"ab", kCd[] = u"cd";
        Rf::Args a = Rf::Prepare("KismetStringLibrary", "Concat_StrStr");
        a.Set("A", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kAb)), 3, 3));
        a.Set("B", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kCd)), 3, 3));
        bool ok = false;
        if (a.ok() && a.Call(Rf::CdoFor("KismetStringLibrary"))) ok = a.Get<FString>("ReturnValue").ToString() == "abcd";
        rep += std::string("; FString ") + (ok ? "ok" : "WRONG");
        if (!ok) block = true;
    }

    // 3. FText: string -> text -> string through the engine
    {
        static const char16_t kT[] = u"kmod";
        Rf::Args a = Rf::Prepare("KismetTextLibrary", "Conv_StringToText");
        a.Set("InString", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kT)), 5, 5));
        bool ok = false;
        if (a.ok() && a.Call(Rf::CdoFor("KismetTextLibrary")))
        {
            FText t = a.Get<FText>("ReturnValue");
            Rf::Args b = Rf::Prepare("KismetTextLibrary", "Conv_TextToString");
            b.Set("InText", t);
            if (b.ok() && b.Call(Rf::CdoFor("KismetTextLibrary"))) ok = b.Get<FString>("ReturnValue").ToString() == "kmod";
        }
        rep += std::string("; FText ") + (ok ? "ok" : "WRONG (FText properties may misbehave)");
    }

    // 4. TArray: GameInstance.LocalPlayers is a short array of valid objects
    {
        bool ok = false;
        if (UObject* gi = Rf::FindFirst("GameInstance"))
            if (FProperty* p = Rf::FindProp(reinterpret_cast<UStruct*>(Rf::ClassOf(gi)), "LocalPlayers"))
            {
                struct Raw { void* data; int32_t num, max; };
                const Raw* r = &Rf::At<Raw>(gi, Rf::OffsetOf(p));
                if (r->data && r->num >= 1 && r->num <= 16 && r->max >= r->num)
                {
                    UObject* first = *static_cast<UObject**>(r->data);
                    ok = first && Rf::ClassOf(first) && Rf::FindClass("LocalPlayer") && Rf::IsA(first, Rf::FindClass("LocalPlayer"));
                }
            }
        rep += std::string("; TArray ") + (ok ? "ok" : "WRONG (array properties may misbehave)");
    }

    g_canaryReport = rep;
    KLog(block ? KLOG_ERROR : KLOG_INFO, "layout", "canaries: %s%s", rep.c_str(), block ? " -> mods are NOT loaded" : "");
    if (block)
    {
        g_layoutMismatch = true;
        std::lock_guard<std::mutex> g(g_layoutMu);
        g_layoutReport += " | canaries: " + rep + " -> mods are NOT loaded (the SDK layouts compiled into this core do not match the game).";
    }
}

// Runs on the game thread (throttled ~20 Hz) from the ProcessEvent hook.
static void OnGameTick()
{
    // Nothing that reads reflection data may run before the engine's layout was checked (and, if it moved, adopted):
    // the first run on a new game build detects it first (about 15 s).
    static double gateSince = 0;
    if (!g_layoutGate.load())
    {
        double t = NowSec();
        if (gateSince == 0) gateSince = t;
        if (t - gateSince < 90.0) return;
        g_layoutGate = true;
    }
    if (!CurrentWorld()) return;
    EnsureScriptHook(InSDKUtils::GetImageBase());          // once, with the engine layout already adopted
    { static bool canaries = false; if (!canaries) { canaries = true; RunCanaries(); } }       // before any mod runs

    double now = NowSec();
    if (g_readySince == 0) { g_readySince = now; Log("engine ready"); KMod_MountPendingPaks(); }
    ArmHeartbeat(now);
    if (now - g_readySince < 5.0) return; // let the game settle
    static bool stableMarked = false;
    if (!stableMarked && now - g_readySince > 20.0) { stableMarked = true; KBoot_MarkStable(); }

    LuaHost_Init();          // no-op after the first call
    LuaHost_Tick(now);
}

// ---------------------------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------------------------
static void InitThread()
{
    Log("core init thread started");
    uintptr_t base = 0;
    std::string report;
    bool ok = false;
    for (int i = 0; i < 600 && !ok; i++)      // up to 5 minutes
    {
        base = InSDKUtils::GetImageBase();
        if (base && Resolver_ResolveCore(base, report))
        {
            TUObjectArray* objs = InSDKUtils::GetGObjects();
            ok = objs && objs->Num() > 50000;
            if (!ok && i % 10 == 0) Log("waiting for the engine to start (%d objects)", objs ? objs->Num() : 0);
        }
        if (!ok && i % 20 == 5) KLog(KLOG_INFO, "core", "still looking for the engine: %s", report.empty() ? "(no report)" : report.c_str());
        if (!ok) Sleep(500);
    }
    if (!base) { KLog(KLOG_ERROR, "core", "game image not found"); return; }
    if (!ok)
    {
        KLog(KLOG_ERROR, "core", "this game build is not supported (could not locate the engine data): %s. kmod stays inactive.", report.c_str());
        return;
    }
    Log("resolved: %s", report.c_str());
    Log("build key %s", Resolver_BuildKey().c_str());
    Sleep(3000);

    void* target = reinterpret_cast<void*>(base + Offsets::ProcessEvent);
    Log("image base=0x%llx ProcessEvent=%p objects=%d", (unsigned long long)base, target, InSDKUtils::GetGObjects()->Num());
    void* tramp = nullptr;
    if (!InstallInlineHook(target, reinterpret_cast<void*>(&HookedProcessEvent), &tramp)) { KLog(KLOG_ERROR, "core", "hook install FAILED"); return; }
    g_origPE = reinterpret_cast<ProcessEventFn>(tramp);
    Log("ProcessEvent hook installed, trampoline=%p", tramp);
    KLogInstallCrashHandlers();      // the engine installs its own handlers during start-up; (re)install ours on top of them
    KMod_DetectLayout(false);        // verify the engine layout against what this core assumes
    Overlay_Start();                 // the ImGui menu (Insert)
}

// The folders a user puts files into must exist as soon as the game has started once: kmod/mods (mod folders and .kmod packages), kmod/data (mod data).
static void EnsureUserFolders()
{
    std::string root = KModRoot();
    for (size_t i = 1; i <= root.size(); i++)
        if (i == root.size() || root[i] == '/') mkdir(root.substr(0, i).c_str(), 0777);
    for (const char* sub : {"mods", "data", ".private", ".private/mods"}) mkdir((root + "/" + sub).c_str(), 0777);
}

// Called once by the dwmapi proxy (from its own thread, never from DllMain).
extern "C" __declspec(dllexport) void kmod_start()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    KLogInit();
    if (kDevBuild) DevConsole_Start();
    EnsureUserFolders();
    KBoot_Init();
    atexit([] { KBoot_MarkStable(); });          // a normal exit counts as a finished start (a crash or a kill does not)
    KLogInstallCrashHandlers();
    NetRedirect_Start();          // before anything else: the game reads its server URLs early
    Log("kmod_core loaded");
    std::thread(InitThread).detach();
}

// kmod.perf() support: outermost ProcessEvent calls and the time spent in them (ms)
void KMod_PerfPE(unsigned long long& calls, double& ms)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    calls = g_peTop.load(); ms = (double)g_peTicks.load() * 1000.0 / (double)f.QuadPart;
}

// ---------------------------------------------------------------------------------------------
// Sampling profiler for the game thread (diagnostics; kmod.perf_sample). Every ~1 ms the thread is suspended, its stack is unwound
// (x64 unwind data) and every function on it is counted: "self" = the top frame, "incl" = anywhere on the stack.
// ---------------------------------------------------------------------------------------------
namespace
{
std::atomic<bool> g_sampling{false};
std::thread g_sampler;
std::map<std::string, int> g_self, g_incl;
int g_samples = 0;

std::string FuncKey(DWORD64 pc)
{
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &base, nullptr);
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) && m)
    {
        GetModuleFileNameA(m, name, sizeof(name));
        const char* b = strrchr(name, '\\'); if (b) memmove(name, b + 1, strlen(b));
    }
    char out[128];
    snprintf(out, sizeof(out), "%s+0x%llx", name, (unsigned long long)((rf ? base + rf->BeginAddress : pc) - (DWORD64)m));
    return out;
}

void SamplerMain(DWORD tid)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return;
    while (g_sampling.load())
    {
        CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        DWORD64 pcs[40]; int npc = 0;     // no allocation while the game thread is suspended (it may hold the heap lock)
        if (SuspendThread(th) != (DWORD)-1)
        {
            if (GetThreadContext(th, &ctx))
            {
                for (int i = 0; i < 40 && ctx.Rip; i++)
                {
                    pcs[npc++] = ctx.Rip;
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                    if (!rf) { if (!ctx.Rsp) break; ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp); ctx.Rsp += 8; continue; }
                    PVOID handler; DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler, &frame, nullptr);
                }
            }
            ResumeThread(th);
        }
        if (npc > 0)
        {
            g_samples++;
            g_self[FuncKey(pcs[0])]++;
            std::set<std::string> seen;
            for (int i = 0; i < npc; i++) seen.insert(FuncKey(pcs[i]));
            for (auto& k : seen) g_incl[k]++;
        }
        Sleep(1);
    }
    CloseHandle(th);
}
}

std::vector<std::string> KMod_PerfSample(bool start)
{
    std::vector<std::string> out;
    if (start)
    {
        if (g_sampling.exchange(true)) return out;
        g_self.clear(); g_incl.clear(); g_samples = 0;
        int tid = KBoot_GameThread();
        if (tid) g_sampler = std::thread(SamplerMain, (DWORD)tid); else g_sampling = false;
        return out;
    }
    if (!g_sampling.exchange(false)) return out;
    if (g_sampler.joinable()) g_sampler.join();
    auto top = [&](const std::map<std::string, int>& m, const char* title)
    {
        std::vector<std::pair<int, std::string>> v;
        for (auto& kv : m) v.emplace_back(kv.second, kv.first);
        std::sort(v.rbegin(), v.rend());
        out.push_back(std::string(title) + " (" + std::to_string(g_samples) + " samples)");
        for (size_t i = 0; i < v.size() && i < 14; i++) out.push_back("  " + std::to_string(v[i].first) + "  " + v[i].second);
    };
    top(g_self, "self");
    top(g_incl, "inclusive");
    return out;
}
