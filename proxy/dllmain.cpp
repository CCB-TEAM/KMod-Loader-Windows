// dwmapi.dll proxy: the game's own folder is searched first for dwmapi.dll, so this file is loaded in place of the system library.
// All real exports are forwarded to the system dwmapi.dll (exports.inl); on start-up a thread loads kmod\kmod_core.dll and calls
// its kmod_start(). Nothing else happens in DllMain (no loading, no locks).
#include <windows.h>

#include "exports.inl"

static void Note(const wchar_t* dir, const char* text)
{
    wchar_t p[MAX_PATH * 2];
    wsprintfW(p, L"%skmod", dir);
    CreateDirectoryW(p, nullptr);
    wsprintfW(p, L"%skmod\\proxy.log", dir);
    HANDLE f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char line[512];
    int n = wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
    DWORD w; WriteFile(f, line, n, &w, nullptr);
    CloseHandle(f);
}

static DWORD WINAPI Boot(LPVOID self)
{
    wchar_t dir[MAX_PATH * 2];
    GetModuleFileNameW(static_cast<HMODULE>(self), dir, MAX_PATH * 2);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
    wchar_t core[MAX_PATH * 2];
    wsprintfW(core, L"%skmod\\kmod_core.dll", dir);
    HMODULE h = LoadLibraryExW(core, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h)
    {
        char b[96]; wsprintfA(b, "cannot load kmod\\kmod_core.dll (error %lu)", GetLastError());
        Note(dir, b);
        return 1;
    }
    auto start = reinterpret_cast<void (*)()>(GetProcAddress(h, "kmod_start"));
    if (!start) { Note(dir, "kmod_core.dll does not export kmod_start"); return 1; }
    start();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, Boot, inst, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
