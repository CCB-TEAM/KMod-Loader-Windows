#include "kmod_boot.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#include "kmod_common.h"
#include "kmod_log.h"

namespace
{
std::string Dir() { return KModRoot() + "/boot"; }

char g_activity[112] = {};                  // "mod:<id>:<what>" / "pak:<name>"; written by the game thread, read by the crash handler
std::atomic<int> g_gameTid{0};
HANDLE g_markFile = INVALID_HANDLE_VALUE;   // crash.mark, opened at start so the handler only needs WriteFile()
std::atomic<bool> g_safe{false};
int g_crashes = 0;
// KBoot_Init runs from a library constructor, possibly before this file's own globals are constructed:
// anything with a constructor lives in a function-local static.
std::string& LastCrash() { static std::string s; return s; }
std::mutex& Mu() { static std::mutex m; return m; }
std::set<std::string>& Quarantine() { static std::set<std::string> q; return q; }    // "mod:<id>" / "pak:<name>"

std::string ReadAll(const std::string& p)
{
    std::string out;
    if (FILE* f = fopen(p.c_str(), "rb")) { char b[1024]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n); fclose(f); }
    return out;
}

void WriteAll(const std::string& p, const std::string& s)
{
    if (FILE* f = fopen(p.c_str(), "wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
}

void SaveState()
{
    WriteAll(Dir() + "/state.txt", "crashes=" + std::to_string(g_crashes) + "\nsafe=" + (g_safe ? "1" : "0") + "\nphase=" + "starting" + "\n");
}

void SaveQuarantine()
{
    std::string s;
    for (auto& q : Quarantine()) s += q + "\n";
    WriteAll(Dir() + "/quarantine.txt", s);
}

int IntOf(const std::string& text, const char* key, int def)
{
    size_t p = text.find(std::string(key) + "=");
    return p == std::string::npos ? def : atoi(text.c_str() + p + strlen(key) + 1);
}
}

void KBoot_Init()
{
    mkdir(Dir().c_str(), 0777);
    std::string state = ReadAll(Dir() + "/state.txt");
    g_crashes = IntOf(state, "crashes", 0);
    g_safe = IntOf(state, "safe", 0) != 0;
    bool wasStarting = state.find("phase=starting") != std::string::npos;
    {
        std::string q = ReadAll(Dir() + "/quarantine.txt");
        size_t i = 0;
        while (i < q.size())
        {
            size_t e = q.find('\n', i);
            if (e == std::string::npos) e = q.size();
            if (e > i) Quarantine().insert(q.substr(i, e - i));
            i = e + 1;
        }
    }

    std::string mark = ReadAll(Dir() + "/crash.mark");
    if (mark.compare(0, 10, "kmod-crash") == 0)
    {
        size_t nl = mark.find('\n');
        std::string act = nl == std::string::npos ? "" : mark.substr(nl + 1);
        while (!act.empty() && (act.back() == '\n' || act.back() == '\0')) act.pop_back();
        LastCrash() = act;
        if (act.compare(0, 4, "mod:") == 0 || act.compare(0, 4, "pak:") == 0)
        {
            size_t c = act.find(':', 4);
            std::string key = act.substr(0, c == std::string::npos ? act.size() : c);   // "mod:<id>"
            Quarantine().insert(key);
            SaveQuarantine();
            KLog(KLOG_WARN, "boot", "the last run crashed while running %s: it is disabled until you re-enable it", key.c_str());
        }
        else
        {
            g_crashes++;
            KLog(KLOG_WARN, "boot", "the last run crashed (cause unknown), %d crash(es) in a row", g_crashes);
            if (g_crashes >= 2) g_safe = true;
        }
    }
    else if (wasStarting)
    {
        // died before reaching a stable state without writing a crash mark (killed, or crashed before our handler existed)
        g_crashes++;
        if (g_crashes >= 3) { g_safe = true; KLog(KLOG_WARN, "boot", "3 start-ups in a row did not finish"); }
    }
    if (g_safe) KLog(KLOG_WARN, "boot", "SAFE MODE: mods and paks are not loaded (turn it off with the 'safe off' console command or from the Mods page)");
    SaveState();
    g_markFile = CreateFileW(KWide(Dir() + "/crash.mark").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool KBoot_SafeMode() { return g_safe.load(); }
void KBoot_SetGameThread(int tid) { g_gameTid = tid; }
int KBoot_GameThread() { return g_gameTid.load(); }

void KBoot_SetActivity(const char* kind, const char* name, const char* what)
{
    if (!kind) { g_activity[0] = 0; return; }
    char tmp[sizeof(g_activity)];
    snprintf(tmp, sizeof(tmp), "%s:%s%s%s", kind, name ? name : "", what && *what ? ":" : "", what ? what : "");
    memcpy(g_activity, tmp, sizeof(tmp));
}

void KBoot_GetActivity(char* out, size_t cap) { snprintf(out, cap, "%s", g_activity); }
void KBoot_RestoreActivity(const char* saved) { snprintf(g_activity, sizeof(g_activity), "%s", saved ? saved : ""); }

void KBoot_MarkStable()
{
    g_crashes = 0;
    WriteAll(Dir() + "/state.txt", std::string("crashes=0\nsafe=") + (g_safe ? "1" : "0") + "\nphase=stable\n");
    // A crash mark is a one-shot record. Leaving it in place causes the next
    // successful launch to quarantine the same mod again.
    if (g_markFile != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER zero{};
        SetFilePointerEx(g_markFile, zero, nullptr, FILE_BEGIN);
        SetEndOfFile(g_markFile);
        FlushFileBuffers(g_markFile);
    }
}

void KBoot_OnCrash()
{
    if (g_markFile == INVALID_HANDLE_VALUE) return;
    DWORD w;
    const char head[] = "kmod-crash\n";
    WriteFile(g_markFile, head, sizeof(head) - 1, &w, nullptr);
    if ((int)GetCurrentThreadId() == g_gameTid.load() && g_activity[0]) WriteFile(g_markFile, g_activity, (DWORD)strnlen(g_activity, sizeof(g_activity)), &w, nullptr);
    WriteFile(g_markFile, "\n", 1, &w, nullptr);
    FlushFileBuffers(g_markFile);
}

bool KBoot_IsQuarantined(const std::string& kind, const std::string& name)
{
    std::lock_guard<std::mutex> g(Mu());
    return Quarantine().count(kind + ":" + name) != 0;
}

std::vector<std::string> KBoot_QuarantineList(const std::string& kind)
{
    std::lock_guard<std::mutex> g(Mu());
    std::vector<std::string> v;
    std::string pre = kind + ":";
    for (auto& q : Quarantine()) if (q.compare(0, pre.size(), pre) == 0) v.push_back(q.substr(pre.size()));
    return v;
}

void KBoot_Add(const std::string& kind, const std::string& name)
{
    std::lock_guard<std::mutex> g(Mu());
    Quarantine().insert(kind + ":" + name);
    SaveQuarantine();
}

void KBoot_Unquarantine(const std::string& kind, const std::string& name)
{
    std::lock_guard<std::mutex> g(Mu());
    Quarantine().erase(kind + ":" + name);
    SaveQuarantine();
}

void KBoot_SafeOff()
{
    g_safe = false;
    g_crashes = 0;
    SaveState();
}

int KBoot_CrashCount() { return g_crashes; }
const std::string& KBoot_LastCrash() { return LastCrash(); }
