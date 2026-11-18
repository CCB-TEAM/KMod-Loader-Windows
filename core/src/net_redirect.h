// Points the game at another server: the game's server URLs are string literals in libUnreal.so; they are patched in memory
// as soon as the library is loaded. Config: <root>/net/server.json  {"server": "http://192.168.1.5:1145"}.
// The new base URL must be short enough for the original strings (28 characters at most).
#pragma once
#include <string>

void NetRedirect_Start();                 // spawns a thread that waits for libUnreal.so and patches it; no-op without a config
const std::string& NetRedirect_Status();  // human readable result
