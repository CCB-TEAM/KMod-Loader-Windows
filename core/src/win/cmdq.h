// Two small shared queues for the Windows UI layers (dev console window and ImGui overlay):
//  * commands typed by the user, executed on the game thread by the Lua host (REPL / console commands);
//  * a ring of the latest log lines, shown by the overlay's console.
#pragma once
#include <string>
#include <vector>

void CmdQueue_Push(const std::string& line);
int CmdQueue_Pop(char* buf, int cap);              // length of the next line, or -1 when empty

struct LogLine { int level; unsigned long long seq; std::string text; };
void LogRing_Add(int level, const std::string& text);
std::vector<LogLine> LogRing_Snapshot();           // the retained lines, oldest first
