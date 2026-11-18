// HTTP(S) client for mods: runs on Java's HttpURLConnection through JNI (system TLS and certificates), on its own threads;
// answers are delivered on the game thread. See docs/lua-api/mod-env/http.md.
#pragma once
#include <string>
#include "lua_worker.h"      // WorkerHooks

struct lua_State;

void Http_Init(const WorkerHooks& hooks);
int Http_Request(lua_State* L);          // lua_CFunction: kmod.http_request(ownerModId, options, callback) -> id
void Http_Pump(lua_State* L);            // game thread, every tick
void Http_CancelUser(lua_State* L);      // forget the callbacks of every mod (reload / disable); late answers are dropped
