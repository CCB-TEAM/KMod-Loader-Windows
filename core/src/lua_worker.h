// Worker threads for Lua mods: each worker is a thread with its own Lua state (no game objects inside) that talks to the
// game thread through message queues. See docs/lua-api/globals/worker.md.
#pragma once
#include <string>

struct lua_State;

struct WorkerHooks
{
    void (*emit)(const std::string& line);                                                              // write a line to the kmod log/console (game thread only)
    bool (*pcall)(lua_State* L, int nargs, int nres, const char* what, const std::string& owner);      // protected call that reports errors to the mod
    void (*report)(lua_State* L, const std::string& owner, const std::string& msg);                    // count an error against the mod
};

// game thread. jsonSource is the text of json.lua (run inside every worker as the global `json`).
void Worker_Init(lua_State* L, const WorkerHooks& hooks, const char* jsonSource);
int Worker_Create(lua_State* L);        // lua_CFunction: kmod.worker(ownerModId, absoluteScriptPath) -> handle
void Worker_Pump(lua_State* L);         // game thread, every tick: deliver messages/logs/errors sent by workers
void Worker_CloseUser(lua_State* L, const char* only = nullptr);    // stop and forget every worker that belongs to a mod (reload / disable)
