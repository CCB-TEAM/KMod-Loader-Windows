#include "http.h"

#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kmod_common.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
struct Options
{
    std::string method = "GET", url, body, saveTo;            // saveTo: stream the body into this file instead of memory
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 15000;
    size_t maxBytes = 8u << 20;
};

struct Response
{
    int id = 0, status = 0;
    bool progress = false;                                       // a progress report, not the final answer
    long long bytes = 0, total = -1;
    std::string body, error, saved;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct Pending { std::string owner; int cbRef; int progressRef; };

WorkerHooks g_hooks{};
std::mutex g_mu;
std::deque<Response> g_done;
std::map<int, Pending> g_pending;        // game thread only
std::atomic<int> g_active{0};
int g_nextId = 1;
const int kMaxActive = 6;

struct WinHandle
{
    HINTERNET h = nullptr;
    WinHandle() = default;
    explicit WinHandle(HINTERNET x) : h(x) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    ~WinHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

std::string WinError(DWORD e)
{
    char b[128];
    const char* name = e == ERROR_WINHTTP_TIMEOUT ? "timed out" : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "host name not found" :
                       e == ERROR_WINHTTP_CANNOT_CONNECT ? "cannot connect" : e == ERROR_WINHTTP_SECURE_FAILURE ? "TLS failure" :
                       e == ERROR_WINHTTP_CONNECTION_ERROR ? "connection error" : nullptr;
    if (name) snprintf(b, sizeof(b), "%s (WinHTTP %lu)", name, (unsigned long)e); else snprintf(b, sizeof(b), "WinHTTP error %lu", (unsigned long)e);
    return b;
}

// the system proxy of the current user (what browsers use), else automatic detection
void ApplyProxy(HINTERNET session, bool& usedNamed, std::wstring& proxyOut)
{
    usedNamed = false;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie = {};
    if (WinHttpGetIEProxyConfigForCurrentUser(&ie))
    {
        if (ie.lpszProxy) { proxyOut = ie.lpszProxy; usedNamed = !proxyOut.empty(); }
        if (ie.lpszProxyBypass) GlobalFree(ie.lpszProxyBypass);
        if (ie.lpszAutoConfigUrl) GlobalFree(ie.lpszAutoConfigUrl);
        if (ie.lpszProxy) GlobalFree(ie.lpszProxy);
    }
    (void)session;
}

Response Perform(const Options& o, int id)
{
    Response r;
    FILE* file = nullptr;
    long long written = 0;
    auto lastReport = std::chrono::steady_clock::now();
    std::wstring wurl = KWide(o.url);
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "invalid URL"; return r; }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    bool named = false; std::wstring proxy;
    ApplyProxy(nullptr, named, proxy);
    WinHandle session(named ? WinHttpOpen(L"kmod", WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0)
                            : WinHttpOpen(L"kmod", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { r.error = WinError(GetLastError()); return r; }
    WinHttpSetTimeouts(session, o.timeoutMs, o.timeoutMs, o.timeoutMs, o.timeoutMs);
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
    WinHandle conn(WinHttpConnect(session, host.c_str(), uc.nPort, 0));
    if (!conn) { r.error = WinError(GetLastError()); return r; }
    WinHandle req(WinHttpOpenRequest(conn, KWide(o.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) { r.error = WinError(GetLastError()); return r; }

    std::wstring hdrs;
    for (auto& h : o.headers) hdrs += KWide(h.first) + L": " + KWide(h.second) + L"\r\n";
    if (!WinHttpSendRequest(req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), (DWORD)-1L,
                            o.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(o.body.data()), (DWORD)o.body.size(), (DWORD)o.body.size(), 0) ||
        !WinHttpReceiveResponse(req, nullptr))
    { r.error = WinError(GetLastError()); return r; }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)status;
    {
        DWORD n = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &n, WINHTTP_NO_HEADER_INDEX);
        if (n)
        {
            std::wstring raw(n / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &n, WINHTTP_NO_HEADER_INDEX))
            {
                std::string text = KUtf8(raw.c_str());
                size_t i = text.find("\r\n");                           // skip the status line
                while (i != std::string::npos && i + 2 < text.size())
                {
                    size_t e = text.find("\r\n", i + 2);
                    std::string line = text.substr(i + 2, e == std::string::npos ? std::string::npos : e - i - 2);
                    size_t c = line.find(':');
                    if (c != std::string::npos && c > 0)
                    {
                        size_t v = c + 1; while (v < line.size() && line[v] == ' ') v++;
                        r.headers.emplace_back(line.substr(0, c), line.substr(v));
                    }
                    i = e;
                }
            }
        }
    }
    long long total = -1;
    for (auto& h : r.headers) { std::string k = h.first; for (auto& ch : k) ch = (char)tolower((unsigned char)ch); if (k == "content-length") total = atoll(h.second.c_str()); }
    if (!o.saveTo.empty() && status < 400)
    {
        for (size_t p = KModRoot().size() + 1; (p = o.saveTo.find('/', p)) != std::string::npos; p++)      // create the folders
            mkdir(o.saveTo.substr(0, p).c_str(), 0777);
        file = fopen((o.saveTo + ".part").c_str(), "wb");
        if (!file) { r.error = "cannot write " + o.saveTo; return r; }
    }
    std::vector<char> buf(16384);
    for (;;)
    {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) { r.error = WinError(GetLastError()); break; }
        if (got == 0) break;
        if (file)
        {
            if (fwrite(buf.data(), 1, got, file) != got) { r.error = "writing the file failed"; break; }
            written += got;
            if (written > (long long)o.maxBytes) { r.error = "download larger than the limit"; break; }
            auto now = std::chrono::steady_clock::now();
            if (now - lastReport > std::chrono::milliseconds(150))
            {
                lastReport = now;
                Response p; p.id = id; p.progress = true; p.bytes = written; p.total = total;
                std::lock_guard<std::mutex> g(g_mu);
                g_done.push_back(std::move(p));
            }
            continue;
        }
        r.body.append(buf.data(), got);
        if (r.body.size() > o.maxBytes) { r.body.resize(o.maxBytes); r.error = "response larger than the limit (truncated)"; break; }
    }
    r.bytes = written; r.total = total;
    if (file)
    {
        fclose(file);
        if (r.error.empty()) { if (kmod_rename((o.saveTo + ".part").c_str(), o.saveTo.c_str()) == 0) r.saved = o.saveTo; else r.error = "cannot move the downloaded file into place"; }
        else kmod_remove((o.saveTo + ".part").c_str());
    }
    return r;
}

void Run(int id, Options o)
{
    Response r = Perform(o, id);
    r.id = id;
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_done.push_back(std::move(r));
    }
    g_active--;
}

bool StartsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
}

void Http_Init(const WorkerHooks& hooks) { g_hooks = hooks; }

// kmod.http_request(owner, { method=, url=, headers={}, body=, timeout=ms, max_bytes= }, callback(response)) -> id
//   response = { status = 200, body = "...", headers = { name = value }, error = nil | "message" }
int Http_Request(lua_State* L)
{
    std::string owner = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    Options o;
    lua_getfield(L, 2, "url");    if (lua_isstring(L, -1)) o.url = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "method"); if (lua_isstring(L, -1)) o.method = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "body");   if (lua_isstring(L, -1)) { size_t n; const char* b = lua_tolstring(L, -1, &n); o.body.assign(b, n); }
    lua_pop(L, 1);
    lua_getfield(L, 2, "timeout"); if (lua_isnumber(L, -1)) { double t = lua_tonumber(L, -1); o.timeoutMs = (int)(t < 1000 ? 1000 : (t > 60000 ? 60000 : t)); }
    lua_pop(L, 1);
    lua_getfield(L, 2, "max_bytes"); if (lua_isnumber(L, -1)) { double m = lua_tonumber(L, -1); if (m > 0 && m < (32 << 20)) o.maxBytes = (size_t)m; }
    lua_pop(L, 1);
    lua_getfield(L, 2, "save_to");
    if (lua_isstring(L, -1))
    {
        o.saveTo = lua_tostring(L, -1);
        bool ok = false;
        for (const char* sub : {"/data/", "/mods/", "/art/"})
            if (o.saveTo.compare(0, (KModRoot() + sub).size(), KModRoot() + sub) == 0) ok = true;
        if (!ok || o.saveTo.find("..") != std::string::npos)
            return luaL_error(L, "Http: files can only be saved inside kmod/data, kmod/mods or kmod/art");
        if (o.maxBytes == (8u << 20)) o.maxBytes = 512u << 20;        // downloads may be big unless the mod set its own limit
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "headers");
    if (lua_istable(L, -1))
    {
        lua_pushnil(L);
        while (lua_next(L, -2))
        {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) o.headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    for (auto& c : o.method) c = (char)toupper((unsigned char)c);
    if (o.method != "GET" && o.method != "POST" && o.method != "PUT" && o.method != "DELETE" && o.method != "HEAD" && o.method != "PATCH")
        return luaL_error(L, "Http: unsupported method '%s'", o.method.c_str());
    if (!StartsWith(o.url, "https://") && !StartsWith(o.url, "http://"))
        return luaL_error(L, "Http: only http:// and https:// URLs are allowed");
    if (g_active.load() >= kMaxActive) return luaL_error(L, "Http: too many requests at once (%d at most)", kMaxActive);

    int id = g_nextId++;
    lua_pushvalue(L, 3);
    int cb = luaL_ref(L, LUA_REGISTRYINDEX);
    int progressRef = LUA_NOREF;
    if (lua_isfunction(L, 4)) { lua_pushvalue(L, 4); progressRef = luaL_ref(L, LUA_REGISTRYINDEX); }
    g_pending[id] = Pending{owner, cb, progressRef};
    g_active++;
    std::thread(Run, id, std::move(o)).detach();
    lua_pushinteger(L, id);
    return 1;
}

void Http_Pump(lua_State* L)
{
    if (g_pending.empty()) return;
    std::deque<Response> batch;
    {
        std::lock_guard<std::mutex> g(g_mu);
        batch.swap(g_done);
    }
    for (auto& r : batch)
    {
        auto it = g_pending.find(r.id);
        if (it == g_pending.end()) continue;                    // cancelled (mod reloaded)
        if (r.progress)
        {
            if (it->second.progressRef != LUA_NOREF)
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.progressRef);
                lua_pushnumber(L, (double)r.bytes);
                lua_pushnumber(L, (double)r.total);
                g_hooks.pcall(L, 2, 0, "http progress", it->second.owner);
            }
            continue;
        }
        Pending p = it->second;
        g_pending.erase(it);
        if (p.progressRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, p.progressRef);
        lua_rawgeti(L, LUA_REGISTRYINDEX, p.cbRef);
        luaL_unref(L, LUA_REGISTRYINDEX, p.cbRef);
        lua_newtable(L);
        lua_pushinteger(L, r.status); lua_setfield(L, -2, "status");
        lua_pushlstring(L, r.body.data(), r.body.size()); lua_setfield(L, -2, "body");
        lua_newtable(L);
        for (auto& h : r.headers) { lua_pushstring(L, h.second.c_str()); lua_setfield(L, -2, h.first.c_str()); }
        lua_setfield(L, -2, "headers");
        if (!r.error.empty()) { lua_pushstring(L, r.error.c_str()); lua_setfield(L, -2, "error"); }
        if (!r.saved.empty()) { lua_pushstring(L, r.saved.c_str()); lua_setfield(L, -2, "saved"); lua_pushnumber(L, (double)r.bytes); lua_setfield(L, -2, "size"); }
        g_hooks.pcall(L, 1, 0, "http callback", p.owner);
    }
}

void Http_CancelUser(lua_State* L)
{
    for (auto it = g_pending.begin(); it != g_pending.end();)
    {
        if (!it->second.owner.empty())
        {
            luaL_unref(L, LUA_REGISTRYINDEX, it->second.cbRef);
            if (it->second.progressRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->second.progressRef);
            it = g_pending.erase(it);
        }
        else ++it;
    }
}
