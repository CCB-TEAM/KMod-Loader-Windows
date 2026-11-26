// Pak files shipped inside .kmod packages: the zip reader that handles them, their checksums, and the files waiting to be
// swapped in at the next game start. The decisions (what to install, when to ask the player) are in lua/modloader.lua.
// Shared by the Android and the Windows core.
#pragma once
#include <string>

struct lua_State;

std::string KPaksRoot();             // where installed paks live: Android <root>/paks, Windows <game>/kards/Content/Paks
std::string KPakStagedRoot();        // <root>/pak-staged: <name> replaces a pak at the next start, <name>.del removes it

// Called once, as early as possible in the core's start (before the game mounts its paks).
void KMod_ApplyStagedPaks();

// Extracts a .kmod into dest, leaving out "paks/" (those are installed separately, straight into the paks folder).
bool KMod_UnzipPackage(const std::string& zipPath, const std::string& dest, std::string& err);

// Adds zip_list, zip_extract, file_sha256 to the table on top of the stack and paks, pak_staged to its "paths" table.
void KModPak_Register(lua_State* L);
