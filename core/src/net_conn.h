// Connections for mods: TCP (optionally TLS), UDP and WebSocket clients. Each connection runs on its own thread; events are
// delivered on the game thread to the Lua function kmod._net_event(id, event, data, extra). TLS goes through Java's
// SSLSocket (system certificates, host name checked). See docs/lua-api/mod-env/sockets.md.
#pragma once
#include <string>
#include "lua_worker.h"      // WorkerHooks

struct lua_State;

void NetConn_Init(const WorkerHooks& hooks);
int NetConn_Open(lua_State* L);     // kmod.net_open(owner, kind, opts) -> id   kind: "tcp" | "udp" | "ws"
int NetConn_Send(lua_State* L);     // kmod.net_send(id, data [, binary]) -> bool
int NetConn_Close(lua_State* L);    // kmod.net_close(id)
void NetConn_Pump(lua_State* L);    // game thread, every tick
void NetConn_CloseUser(lua_State* L, const char* only = nullptr);   // close every mod's connections (reload / disable)
