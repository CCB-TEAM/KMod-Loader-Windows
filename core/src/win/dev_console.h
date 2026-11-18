// Developer console (dev builds): a console window that shows the kmod log live and takes Lua / console commands (like UE4SS's console tab).
#pragma once
#include <string>

void DevConsole_Start();                                    // opens the window and the input thread (once)
bool DevConsole_Active();
void DevConsole_Write(int level, const std::string& line);  // level: 0 debug, 1 info, 2 warn, 3 error
int DevConsole_Pop(char* buf, int cap);                     // next typed line (length) or -1 when there is none
