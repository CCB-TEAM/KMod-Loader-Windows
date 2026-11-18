#include "dev_console.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "compat.h"
#include "cmdq.h"

namespace
{
std::mutex g_mu;
std::deque<std::string> g_input;
std::atomic<bool> g_up{false};
HANDLE g_out = INVALID_HANDLE_VALUE, g_in = INVALID_HANDLE_VALUE;

BOOL WINAPI CtrlHandler(DWORD type)
{
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ? TRUE : FALSE;      // closing the console must not kill the game
}

void Put(const std::wstring& w)
{
    DWORD n;
    WriteConsoleW(g_out, w.c_str(), (DWORD)w.size(), &n, nullptr);
}

void ReaderMain()
{
    wchar_t buf[8192];
    for (;;)
    {
        DWORD n = 0;
        if (!ReadConsoleW(g_in, buf, (DWORD)(sizeof(buf) / sizeof(buf[0]) - 1), &n, nullptr) || n == 0) { Sleep(200); continue; }
        std::wstring w(buf, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n')) w.pop_back();
        if (w.empty()) continue;
        CmdQueue_Push(KUtf8(w.c_str()));
    }
}
}

void DevConsole_Start()
{
    if (g_up.exchange(true)) return;
    if (!AllocConsole()) { g_up = false; return; }
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(g_out, &mode)) SetConsoleMode(g_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleTitleW(L"kmod dev console  -  type 'help'");
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    if (HWND w = GetConsoleWindow()) DeleteMenu(GetSystemMenu(w, FALSE), SC_CLOSE, MF_BYCOMMAND);      // no close button: it would end the game
    Put(L"\x1b[1;36mkmod dev console\x1b[0m - the log is shown here; type a console command (help) or a Lua expression and press Enter.\r\n");
    std::thread(ReaderMain).detach();
}

bool DevConsole_Active() { return g_up.load(); }

void DevConsole_Write(int level, const std::string& line)
{
    if (!g_up.load()) return;
    const wchar_t* color = level >= 3 ? L"\x1b[91m" : level == 2 ? L"\x1b[93m" : level == 0 ? L"\x1b[90m" : L"\x1b[0m";
    std::lock_guard<std::mutex> g(g_mu);
    Put(std::wstring(color) + KWide(line) + L"\x1b[0m\r\n");
}

int DevConsole_Pop(char* buf, int cap) { return CmdQueue_Pop(buf, cap); }
