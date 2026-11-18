#include "kmod_log.h"
#include "kmod_boot.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <share.h>

#include "kmod_common.h"
#include "win/dev_console.h"
#include "win/cmdq.h"
#include "kmod_version.h"

namespace
{
std::mutex g_mu;
FILE* g_file = nullptr;
HANDLE g_crashFile = INVALID_HANDLE_VALUE;      // a second handle to the log, used by the crash handler (no CRT, no locks)
std::string g_dir;
bool g_inited = false;
constexpr int kKeepFiles = 5;

KLogLevel MinLevel() { return kDevBuild ? KLOG_DEBUG : KLOG_INFO; }
char LevelChar(KLogLevel l) { return l == KLOG_DEBUG ? 'D' : l == KLOG_INFO ? 'I' : l == KLOG_WARN ? 'W' : 'E'; }

void MkdirAllLocal(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
}

void WriteLine(KLogLevel level, const char* tag, const std::string& msg)
{
    std::string line = std::string("[") + tag + "] " + msg + "\n";
    OutputDebugStringA(("KMod " + line).c_str());
    LogRing_Add((int)level, std::string("[") + tag + "] " + msg);
    if (kDevBuild && DevConsole_Active()) DevConsole_Write((int)level, std::string("[") + tag + "] " + msg);
    if (level < MinLevel() || !g_file) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_file, "%04d-%02d-%02d %02d:%02d:%02d.%03d %c/%s: %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            LevelChar(level), tag, msg.c_str());
    fflush(g_file);
}
} // namespace

const std::string& KLogDir() { return g_dir; }

void KLogMsg(KLogLevel level, const char* tag, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_mu);
    WriteLine(level, tag, msg);
}

void KLog(KLogLevel level, const char* tag, const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    KLogMsg(level, tag, buf);
}

static std::string WindowsVersion()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = {}; v.dwOSVersionInfoSize = sizeof(v);
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&v);
    char b[64]; snprintf(b, sizeof(b), "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return b;
}

void KLogInit()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_inited) return;
    g_inited = true;
    MkdirAllLocal(KModRoot());
    g_dir = KModRoot() + "/logs";
    MkdirAllLocal(g_dir);

    // rotate: kmod.log -> kmod.1.log -> ... -> kmod.(N-1).log, oldest dropped
    auto name = [](int i) { return g_dir + (i == 0 ? "/kmod.log" : "/kmod." + std::to_string(i) + ".log"); };
    unlink(name(kKeepFiles - 1).c_str());
    for (int i = kKeepFiles - 2; i >= 0; i--) kmod_rename(name(i).c_str(), name(i + 1).c_str());

    g_file = _wfsopen(KWide(name(0)).c_str(), L"w", _SH_DENYWR);            // other programs may read the log while the game runs
    if (!g_file) g_file = fopen(name(0).c_str(), "w");
    g_crashFile = CreateFileW(KWide(name(0)).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    WriteLine(KLOG_INFO, "kmod", std::string("=== kmod loader ") + KMOD_VERSION + " (" + KMOD_FLAVOR + ", Windows) started ===");
    char exe[MAX_PATH * 2] = {};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    WriteLine(KLOG_INFO, "kmod", std::string("process ") + exe + ", pid " + std::to_string(getpid()));
    WriteLine(KLOG_INFO, "kmod", WindowsVersion());
    WriteLine(KLOG_INFO, "kmod", "data dir " + KModRoot());
}

// ------------------------------------------------------------------------------------------------
// Crash handling. A vectored handler sees every exception first: it only reports fatal kinds, and when one hits while a mod's code is
// running on the game thread it records the blame (KBoot_OnCrash) right away. An unhandled-exception filter writes the full report.
// ------------------------------------------------------------------------------------------------
namespace
{
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
volatile LONG g_reporting = 0;

bool IsFatal(DWORD c)
{
    return c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_INT_DIVIDE_BY_ZERO ||
           c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || c == EXCEPTION_FLT_DIVIDE_BY_ZERO;
}

void RawWrite(const char* s, size_t n)
{
    if (g_crashFile == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(g_crashFile, s, (DWORD)n, &w, nullptr);
}

void Hex(char* out, size_t cap, size_t& n, unsigned long long v)
{
    char tmp[20]; int k = 0;
    if (v == 0) tmp[k++] = '0';
    while (v && k < 18) { int d = (int)(v & 0xF); tmp[k++] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; }
    if (n + 2 < cap) { out[n++] = '0'; out[n++] = 'x'; }
    while (k) { if (n + 1 < cap) out[n++] = tmp[--k]; else break; }
}

void Describe(void* addr, char* out, size_t cap, size_t& n)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, sizeof(path));
        const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
        Hex(out, cap, n, (unsigned long long)((char*)addr - (char*)m));
        if (n + 1 < cap) out[n++] = ' ';
        for (const char* p = base; *p && n + 1 < cap; p++) out[n++] = *p;
    }
    else Hex(out, cap, n, (unsigned long long)addr);
}

void Report(EXCEPTION_POINTERS* ep, const char* kind)
{
    if (InterlockedCompareExchange(&g_reporting, 1, 0) != 0) return;
    KBoot_OnCrash();
    char line[512]; size_t n = 0;
    auto put = [&](const char* s) { while (*s && n + 1 < sizeof(line)) line[n++] = *s++; };
    put("\n!!! FATAL "); put(kind); put(" code "); Hex(line, sizeof(line), n, ep->ExceptionRecord->ExceptionCode);
    put(" at "); Describe(ep->ExceptionRecord->ExceptionAddress, line, sizeof(line), n);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
    {
        put(ep->ExceptionRecord->ExceptionInformation[0] == 0 ? " (read of " : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? " (write to " : " (execute at ");
        Hex(line, sizeof(line), n, ep->ExceptionRecord->ExceptionInformation[1]); put(")");
    }
    put(" tid "); Hex(line, sizeof(line), n, GetCurrentThreadId()); put(" (kmod "); put(KMOD_VERSION); put(")\n");
    RawWrite(line, n);
    if (ep->ContextRecord)
    {
        n = 0; put("  rip "); Hex(line, sizeof(line), n, ep->ContextRecord->Rip); put(" rsp "); Hex(line, sizeof(line), n, ep->ContextRecord->Rsp);
        put(" rax "); Hex(line, sizeof(line), n, ep->ContextRecord->Rax); put(" rcx "); Hex(line, sizeof(line), n, ep->ContextRecord->Rcx);
        put(" rdx "); Hex(line, sizeof(line), n, ep->ContextRecord->Rdx); put("\n"); RawWrite(line, n);
    }
    void* frames[48];
    USHORT cnt = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < cnt; i++)
    {
        n = 0; put("  #"); Hex(line, sizeof(line), n, i); put(" "); Describe(frames[i], line, sizeof(line), n); put("\n");
        RawWrite(line, n);
    }
    if (g_crashFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g_crashFile);
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep)
{
    if (!IsFatal(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    char act[112] = {};
    KBoot_GetActivity(act, sizeof(act));
    if (act[0] && (int)GetCurrentThreadId() == KBoot_GameThread())        // a mod's code was running on the game thread: blame it now
        Report(ep, "exception in a mod callback");
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep)
{
    Report(ep, "unhandled exception");
    if (g_prevFilter && g_prevFilter != &UnhandledFilter) return g_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void KLogInstallCrashHandlers()
{
    static bool veh = false;
    if (!veh) { veh = true; AddVectoredExceptionHandler(1, &VectoredHandler); }
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(&UnhandledFilter);     // (re-)install ours; remember whoever was there
    if (cur && cur != &UnhandledFilter) g_prevFilter = cur;
    KLog(KLOG_DEBUG, "kmod", "crash handlers installed");
}
