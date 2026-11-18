// Crash recovery. The crash handler records which mod's callback (or which pak mount) was running on the game thread;
// the next start quarantines that mod/pak. Crashes with no known cause, repeated, put kmod into SAFE MODE (no mods, no paks)
// until the user turns it off. State lives in <root>/boot/.
#pragma once
#include <string>
#include <vector>

void KBoot_Init();                                       // once, at load (after KLogInit)
bool KBoot_SafeMode();
void KBoot_SetGameThread(int tid);
int KBoot_GameThread();                       // the crash is only blamed on a mod when it happened on this thread
void KBoot_SetActivity(const char* kind, const char* name, const char* what);   // game thread; kind nullptr clears. e.g. ("mod","card_art","hook")
void KBoot_GetActivity(char* out, size_t cap);           // copy of the current activity (for save/restore around nested calls)
void KBoot_RestoreActivity(const char* saved);
void KBoot_MarkStable();                                 // the run has been healthy for a while: crash counters reset
void KBoot_OnCrash();                                    // called by the crash handler (async-signal-safe)
bool KBoot_IsQuarantined(const std::string& kind, const std::string& name);
std::vector<std::string> KBoot_QuarantineList(const std::string& kind);
void KBoot_Add(const std::string& kind, const std::string& name);     // kinds: "mod", "pak" (crashed), "off" (a pak the user switched off)
void KBoot_Unquarantine(const std::string& kind, const std::string& name);
void KBoot_SafeOff();                                    // leave safe mode (takes effect immediately for the next mod load)
int KBoot_CrashCount();
const std::string& KBoot_LastCrash();                    // what the previous run was doing when it crashed ("" = nothing recorded)
