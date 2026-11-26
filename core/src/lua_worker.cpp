#include "lua_worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "kmod_common.h"

namespace
{
// ---------------------------------------------------------------------------------------------
// Messages: a deep copy of nil / boolean / number / string / table, so no Lua value is ever shared between states.
// ---------------------------------------------------------------------------------------------
struct Val
{
    enum T { Nil, Bool, Int, Num, Str, Tab } t = Nil;
    bool b = false;
    long long i = 0;
    double n = 0;
    std::string s;
    std::vector<std::pair<Val, Val>> kv;
};

const int kMaxDepth = 32;
const size_t kMaxQueue = 20000;

bool ToVal(lua_State* L, int idx, Val& out, int depth, std::string& err)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx))
    {
    case LUA_TNIL: out.t = Val::Nil; return true;
    case LUA_TBOOLEAN: out.t = Val::Bool; out.b = lua_toboolean(L, idx) != 0; return true;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) { out.t = Val::Int; out.i = lua_tointeger(L, idx); }
        else { out.t = Val::Num; out.n = lua_tonumber(L, idx); }
        return true;
    case LUA_TSTRING: { size_t n; const char* p = lua_tolstring(L, idx, &n); out.t = Val::Str; out.s.assign(p, n); return true; }
    case LUA_TTABLE:
    {
        if (depth >= kMaxDepth) { err = "message is nested too deeply (or contains itself)"; return false; }
        out.t = Val::Tab;
        if (!lua_checkstack(L, 4)) { err = "message is too large"; return false; }
        lua_pushnil(L);
        while (lua_next(L, idx))
        {
            Val k, v;
            int kt = lua_type(L, -2);
            if (kt != LUA_TSTRING && kt != LUA_TNUMBER && kt != LUA_TBOOLEAN) { err = std::string("a table key of type ") + lua_typename(L, kt) + " cannot be sent"; lua_pop(L, 2); return false; }
            if (!ToVal(L, -2, k, depth + 1, err) || !ToVal(L, -1, v, depth + 1, err)) { lua_pop(L, 2); return false; }
            out.kv.emplace_back(std::move(k), std::move(v));
            lua_pop(L, 1);
        }
        return true;
    }
    default:
        err = std::string("a value of type ") + lua_typename(L, lua_type(L, idx)) + " cannot be sent (only nil, boolean, number, string, table)";
        return false;
    }
}

void PushVal(lua_State* L, const Val& v)
{
    switch (v.t)
    {
    case Val::Nil: lua_pushnil(L); break;
    case Val::Bool: lua_pushboolean(L, v.b); break;
    case Val::Int: lua_pushinteger(L, (lua_Integer)v.i); break;
    case Val::Num: lua_pushnumber(L, v.n); break;
    case Val::Str: lua_pushlstring(L, v.s.data(), v.s.size()); break;
    case Val::Tab:
        lua_createtable(L, 0, (int)v.kv.size());
        for (auto& p : v.kv) { PushVal(L, p.first); PushVal(L, p.second); lua_rawset(L, -3); }
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Worker state (shared between the game thread and the worker thread)
// ---------------------------------------------------------------------------------------------
struct Out { enum Kind { Msg, Log, Err } kind; Val v; std::string text; };

struct W
{
    int id = 0;
    std::string owner, path;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Val> inbox;
    std::deque<Out> outbox;
    std::atomic<bool> stop{false}, done{false};
    std::thread th;
    int cbRef = LUA_NOREF;      // game-thread Lua function receiving the messages (registry of the main state)
};

WorkerHooks g_hooks{};
std::string g_jsonSource;
std::vector<std::shared_ptr<W>> g_workers;      // game thread only
int g_nextId = 1;
const char* MT_WORKER = "kmod.Worker";

struct Handle { int id; };

std::shared_ptr<W> Find(int id)
{
    for (auto& w : g_workers) if (w->id == id) return w;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// The worker side
// ---------------------------------------------------------------------------------------------
W* SelfW(lua_State* L) { return *static_cast<W**>(lua_getextraspace(L)); }

void CountHook(lua_State* L, lua_Debug*)
{
    W* w = SelfW(L);
    if (w && w->stop.load()) luaL_error(L, "worker stopped");
}

void Push(W* w, Out&& o)
{
    std::lock_guard<std::mutex> g(w->mu);
    if (w->outbox.size() < kMaxQueue * 4) w->outbox.push_back(std::move(o));
}

int Wk_post(lua_State* L)
{
    W* w = SelfW(L);
    Out o; o.kind = Out::Msg;
    std::string err;
    bool ok = ToVal(L, 1, o.v, 0, err);
    if (!ok) return luaL_error(L, "post: %s", err.c_str());
    {
        std::lock_guard<std::mutex> g(w->mu);
        if (w->outbox.size() >= kMaxQueue) return luaL_error(L, "post: the game thread is not keeping up (queue full)");
        w->outbox.push_back(std::move(o));
    }
    return 0;
}

int Wk_onMessage(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "kmod.worker.handler");
    return 0;
}

int Wk_Sleep(lua_State* L)
{
    double ms = luaL_checknumber(L, 1);
    W* w = SelfW(L);
    std::unique_lock<std::mutex> lk(w->mu);
    w->cv.wait_for(lk, std::chrono::microseconds((long long)(ms * 1000)), [&] { return w->stop.load(); });
    lua_pushboolean(L, !w->stop.load());
    return 1;
}

int Wk_IsStopping(lua_State* L) { lua_pushboolean(L, SelfW(L)->stop.load()); return 1; }

int Wk_print(lua_State* L)
{
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++)
    {
        size_t len; const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    Out o; o.kind = Out::Log; o.text = line;
    Push(SelfW(L), std::move(o));
    return 0;
}

// fs: reads anywhere under <kmod root>, writes only under <kmod root>/data
bool UnderRoot(const std::string& p, const char* sub)
{
    std::string root = KModRoot() + "/" + sub;
    return p.compare(0, root.size(), root) == 0 && p.find("..") == std::string::npos;
}

int Fs_read(lua_State* L)
{
    std::string path = luaL_checkstring(L, 1);
    if (!UnderRoot(path, "")) return luaL_error(L, "fs.read: only files inside the kmod folder can be read");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { lua_pushnil(L); return 1; }
    std::string out; char b[16384]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
    fclose(f);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int Fs_write(lua_State* L)
{
    std::string path = luaL_checkstring(L, 1);
    size_t len; const char* data = luaL_checklstring(L, 2, &len);
    if (!UnderRoot(path, "data/")) return luaL_error(L, "fs.write: files can only be written inside kmod/data/");
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { lua_pushboolean(L, 0); return 1; }
    fwrite(data, 1, len, f); fclose(f);
    lua_pushboolean(L, 1);
    return 1;
}

int Fs_exists(lua_State* L)
{
    std::string path = luaL_checkstring(L, 1);
    struct stat st;
    lua_pushboolean(L, UnderRoot(path, "") && stat(path.c_str(), &st) == 0);
    return 1;
}

int Fs_list(lua_State* L)   // fs.list(dir) -> { {name=, dir=}, ... }
{
    std::string path = luaL_checkstring(L, 1);
    if (!UnderRoot(path, "")) return luaL_error(L, "fs.list: only folders inside the kmod folder can be listed");
    lua_newtable(L);
    int n = 0;
    if (DIR* d = opendir(path.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            if (e->d_name[0] == '.') continue;
            struct stat st;
            std::string full = path + "/" + e->d_name;
            lua_newtable(L);
            lua_pushstring(L, e->d_name); lua_setfield(L, -2, "name");
            lua_pushboolean(L, stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)); lua_setfield(L, -2, "dir");
            lua_rawseti(L, -2, ++n);
        }
        closedir(d);
    }
    return 1;
}

void OpenSafe(lua_State* L)
{
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
    luaL_requiref(L, "coroutine", luaopen_coroutine, 1); lua_pop(L, 1);
    luaL_requiref(L, "os", luaopen_os, 1);                       // only the clock/time functions survive
    for (const char* n : {"execute", "exit", "remove", "rename", "tmpname", "getenv", "setlocale"}) { lua_pushnil(L); lua_setfield(L, -2, n); }
    lua_pop(L, 1);
    for (const char* n : {"dofile", "loadfile"}) { lua_pushnil(L); lua_setglobal(L, n); }
}

void ThreadMain(std::shared_ptr<W> w, std::string source)
{
    lua_State* L = luaL_newstate();
    *static_cast<W**>(lua_getextraspace(L)) = w.get();
    OpenSafe(L);
    lua_register(L, "post", Wk_post);
    lua_register(L, "onMessage", Wk_onMessage);
    lua_register(L, "Sleep", Wk_Sleep);
    lua_register(L, "IsStopping", Wk_IsStopping);
    lua_register(L, "print", Wk_print);
    static const luaL_Reg fs[] = {{"read", Fs_read}, {"write", Fs_write}, {"exists", Fs_exists}, {"list", Fs_list}, {nullptr, nullptr}};
    luaL_newlib(L, fs); lua_setglobal(L, "fs");
    lua_newtable(L);
    lua_pushstring(L, KModRoot().c_str()); lua_setfield(L, -2, "root");
    lua_setglobal(L, "paths");
    if (luaL_loadbuffer(L, g_jsonSource.data(), g_jsonSource.size(), "=json") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK) lua_setglobal(L, "json");
    else lua_pop(L, 1);
    lua_sethook(L, CountHook, LUA_MASKCOUNT, 4000);

    auto fail = [&](const std::string& what) { Out o; o.kind = Out::Err; o.text = what; Push(w.get(), std::move(o)); };
    auto trace = [](lua_State* S) -> int { const char* m = lua_tostring(S, 1); luaL_traceback(S, S, m ? m : "?", 1); return 1; };

    auto run = [&](int nargs) -> bool {
        int base = lua_gettop(L) - nargs;
        lua_pushcfunction(L, trace); lua_insert(L, base);
        int rc = lua_pcall(L, nargs, 0, base);
        lua_remove(L, base);
        if (rc != LUA_OK)
        {
            std::string m = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
            lua_pop(L, 1);
            if (m.find("worker stopped") == std::string::npos) fail(m);
            return false;
        }
        return true;
    };

    if (luaL_loadbuffer(L, source.data(), source.size(), ("@" + w->path).c_str()) != LUA_OK) { fail(lua_tostring(L, -1)); lua_pop(L, 1); }
    else if (run(0))
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "kmod.worker.handler");
        bool hasHandler = lua_isfunction(L, -1);
        lua_pop(L, 1);
        while (hasHandler && !w->stop.load())
        {
            Val msg;
            {
                std::unique_lock<std::mutex> lk(w->mu);
                w->cv.wait(lk, [&] { return !w->inbox.empty() || w->stop.load(); });
                if (w->stop.load()) break;
                msg = std::move(w->inbox.front());
                w->inbox.pop_front();
            }
            lua_getfield(L, LUA_REGISTRYINDEX, "kmod.worker.handler");
            PushVal(L, msg);
            run(1);
        }
    }
    lua_close(L);
    w->done = true;
}

// ---------------------------------------------------------------------------------------------
// The game-thread side
// ---------------------------------------------------------------------------------------------
Handle* CheckHandle(lua_State* L, int idx) { return static_cast<Handle*>(luaL_checkudata(L, idx, MT_WORKER)); }

int H_post(lua_State* L)
{
    Handle* h = CheckHandle(L, 1);
    std::shared_ptr<W> w = Find(h->id);
    if (!w || w->stop.load()) return luaL_error(L, "post: the worker is closed");
    Val v; std::string err;
    bool ok = ToVal(L, 2, v, 0, err);
    if (!ok) return luaL_error(L, "post: %s", err.c_str());
    {
        std::lock_guard<std::mutex> g(w->mu);
        if (w->inbox.size() >= kMaxQueue) return luaL_error(L, "post: the worker is not keeping up (queue full)");
        w->inbox.push_back(std::move(v));
    }
    w->cv.notify_one();
    return 0;
}

int H_onMessage(lua_State* L)
{
    Handle* h = CheckHandle(L, 1);
    std::shared_ptr<W> w = Find(h->id);
    if (!w) return 0;
    if (w->cbRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->cbRef);
    w->cbRef = LUA_NOREF;
    if (lua_isfunction(L, 2)) { lua_pushvalue(L, 2); w->cbRef = luaL_ref(L, LUA_REGISTRYINDEX); }
    lua_pushvalue(L, 1);
    return 1;
}

void Shutdown(lua_State* L, const std::shared_ptr<W>& w)
{
    w->stop = true;
    w->cv.notify_all();
    for (int i = 0; i < 400 && !w->done.load(); i++) usleep(5000);      // up to 2 s for the script to notice (it is interrupted every few thousand instructions)
    if (w->th.joinable()) { if (w->done.load()) w->th.join(); else w->th.detach(); }
    if (w->cbRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, w->cbRef); w->cbRef = LUA_NOREF; }
}

int H_close(lua_State* L)
{
    Handle* h = CheckHandle(L, 1);
    std::shared_ptr<W> w = Find(h->id);
    if (!w) return 0;
    Shutdown(L, w);
    for (size_t i = 0; i < g_workers.size(); i++) if (g_workers[i]->id == h->id) { g_workers.erase(g_workers.begin() + i); break; }
    return 0;
}

int H_alive(lua_State* L)
{
    Handle* h = CheckHandle(L, 1);
    std::shared_ptr<W> w = Find(h->id);
    lua_pushboolean(L, w && !w->done.load());
    return 1;
}
} // namespace

void Worker_Init(lua_State* L, const WorkerHooks& hooks, const char* jsonSource)
{
    g_hooks = hooks;
    g_jsonSource = jsonSource ? jsonSource : "";
    luaL_newmetatable(L, MT_WORKER);
    lua_newtable(L);
    static const luaL_Reg methods[] = {{"post", H_post}, {"onMessage", H_onMessage}, {"close", H_close}, {"alive", H_alive}, {nullptr, nullptr}};
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

int Worker_Create(lua_State* L)
{
    std::string owner = luaL_checkstring(L, 1);
    std::string path = luaL_checkstring(L, 2);
    std::string src;
    bool readable = false;
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        char b[8192]; size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) src.append(b, n);
        fclose(f);
        readable = true;
    }
    if (!readable) return luaL_error(L, "Worker: cannot read %s", path.c_str());
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) src.erase(0, 3);
    if (g_workers.size() >= 16) return luaL_error(L, "Worker: too many workers (16 at most)");

    auto w = std::make_shared<W>();
    w->id = g_nextId++;
    w->owner = owner;
    w->path = path;
    g_workers.push_back(w);
    w->th = std::thread(ThreadMain, w, std::move(src));

    Handle* h = static_cast<Handle*>(lua_newuserdatauv(L, sizeof(Handle), 0));
    h->id = w->id;
    luaL_setmetatable(L, MT_WORKER);
    return 1;
}

void Worker_Pump(lua_State* L)
{
    if (g_workers.empty()) return;
    std::vector<std::shared_ptr<W>> list = g_workers;        // callbacks may close workers
    for (auto& w : list)
    {
        std::deque<Out> batch;
        {
            std::lock_guard<std::mutex> g(w->mu);
            size_t n = std::min<size_t>(w->outbox.size(), 256);
            for (size_t i = 0; i < n; i++) { batch.push_back(std::move(w->outbox.front())); w->outbox.pop_front(); }
        }
        for (auto& o : batch)
        {
            std::string tag = "[worker " + (w->owner.empty() ? std::string("?") : w->owner) + "] ";
            if (o.kind == Out::Log) g_hooks.emit(tag + o.text);
            else if (o.kind == Out::Err) { g_hooks.emit("[error] " + tag + o.text); g_hooks.report(L, w->owner, o.text); }
            else if (w->cbRef != LUA_NOREF)
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, w->cbRef);
                PushVal(L, o.v);
                g_hooks.pcall(L, 1, 0, "worker message", w->owner);
            }
        }
    }
    // forget workers that finished on their own and have nothing left to deliver (the handle then only answers alive() = false)
    for (size_t i = 0; i < g_workers.size();)
    {
        auto& w = g_workers[i];
        bool empty;
        { std::lock_guard<std::mutex> g(w->mu); empty = w->outbox.empty(); }
        if (w->done.load() && empty) { Shutdown(L, w); g_workers.erase(g_workers.begin() + i); }
        else i++;
    }
}

void Worker_CloseUser(lua_State* L, const char* only)
{
    for (size_t i = 0; i < g_workers.size();)
    {
        if (!g_workers[i]->owner.empty() && (!only || g_workers[i]->owner == only)) { Shutdown(L, g_workers[i]); g_workers.erase(g_workers.begin() + i); }
        else i++;
    }
}
