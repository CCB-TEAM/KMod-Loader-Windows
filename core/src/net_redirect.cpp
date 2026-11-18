#include "net_redirect.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kmod_common.h"
#include "kmod_log.h"
#include "resolver.h"
#include "Basic.hpp"

using namespace SDK;

namespace
{
std::string g_status = "no redirect configured (kmod/net/server.json)";
std::mutex g_mu;

void SetStatus(const std::string& s) { std::lock_guard<std::mutex> g(g_mu); g_status = s; }

std::string ReadAll(const std::string& p)
{
    std::string out;
    if (FILE* f = fopen(p.c_str(), "rb")) { char b[1024]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n); fclose(f); }
    return out;
}

// the value of "key": "..." in a flat JSON object (no escapes needed for URLs)
std::string JsonString(const std::string& json, const char* key)
{
    size_t k = json.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return "";
    size_t c = json.find(':', k);
    if (c == std::string::npos) return "";
    size_t a = json.find('"', c);
    if (a == std::string::npos) return "";
    size_t b = json.find('"', a + 1);
    return b == std::string::npos ? "" : json.substr(a + 1, b - a - 1);
}

bool Writable(uintptr_t addr, size_t n, const void* data)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(addr), data, n);
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(addr), n, old, &tmp);
    return true;
}

// replace every occurrence of `old` (a string literal slot) by `repl`, NUL-filled to the old length
int Replace8(const std::string& old, const std::string& repl)
{
    if (repl.size() > old.size()) return -1;
    std::vector<uintptr_t> hits;
    Resolver_FindBytes(old.c_str(), old.size() + 1, hits);          // including the terminator: whole literals only
    std::string buf = repl;
    buf.resize(old.size() + 1, '\0');
    int n = 0;
    for (uintptr_t h : hits) if (Writable(h, buf.size(), buf.data())) n++;
    return n;
}

int Replace16(const std::u16string& old, const std::u16string& repl)
{
    if (repl.size() > old.size()) return -1;
    std::vector<uintptr_t> hits;
    Resolver_FindBytes(old.c_str(), (old.size() + 1) * 2, hits);
    std::u16string buf = repl;
    buf.resize(old.size() + 1, u'\0');
    int n = 0;
    for (uintptr_t h : hits) if (Writable(h, buf.size() * 2, buf.data())) n++;
    return n;
}

void Run()
{
    std::string server = JsonString(ReadAll(KModRoot() + "/net/server.json"), "server");
    std::string enabled = ReadAll(KModRoot() + "/net/server.json");
    if (server.empty() || enabled.find("\"enabled\": false") != std::string::npos || enabled.find("\"enabled\":false") != std::string::npos) return;
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.compare(0, 7, "http://") != 0 && server.compare(0, 8, "https://") != 0)
    {
        SetStatus("server.json: 'server' must start with http:// or https://");
        KLog(KLOG_ERROR, "net", "%s", g_status.c_str());
        return;
    }
    // the game image is loaded already; the strings are patched before the engine reads them
    uintptr_t base = 0;
    for (int i = 0; i < 6000 && !base; i++) { base = InSDKUtils::GetImageBase(); if (!base) Sleep(10); }
    if (!base) { SetStatus("the game image was not found"); KLog(KLOG_ERROR, "net", "%s", g_status.c_str()); return; }

    static const std::string kConfig8 = "https://kards.live.1939api.com/config";
    static const std::string kBase8 = "https://kards.live.1939api.com/";
    static const std::u16string kConfig16 = u"https://kards.%s.1939api.com/config";
    std::u16string server16;
    for (unsigned char c : server) server16.push_back(c);
    int a = Replace8(kConfig8, server + "/config");
    int b = Replace8(kBase8, server + "/");
    int c = Replace16(kConfig16, server16 + u"/config");
    if (a < 0 || b < 0 || c < 0)
    {
        SetStatus("server '" + server + "' is too long: the original strings leave room for 28 characters (an IP with a port fits)");
        KLog(KLOG_ERROR, "net", "%s", g_status.c_str());
        return;
    }
    char msg[200];
    snprintf(msg, sizeof(msg), " (patched %d + %d + %d string literal(s))", a, b, c);
    SetStatus("game server redirected to " + server + msg);
    KLog(a + b + c > 0 ? KLOG_INFO : KLOG_WARN, "net", "%s", g_status.c_str());
}
}

void NetRedirect_Start()
{
    mkdir((KModRoot() + "/net").c_str(), 0777);
    if (access((KModRoot() + "/net/server.json").c_str(), 0) != 0) return;
    std::thread(Run).detach();
}

const std::string& NetRedirect_Status()
{
    std::lock_guard<std::mutex> g(g_mu);
    return g_status;
}
