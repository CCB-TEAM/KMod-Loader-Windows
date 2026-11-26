// Native UI (kmod.h level 2 panels) does not exist on Windows: those calls report "not available". What Windows has are the choosers
// behind Media.pick_image / pick_file / save_file: standard file dialogs on their own thread (the game keeps running); the result is
// queued and delivered on the game thread by NativeUi_Drain.
#include <windows.h>
#include <commdlg.h>

#include <deque>
#include <filesystem>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kmod_log.h"
#include "lua_host.h"

namespace
{
struct PickCb { void (*cb)(int, const char*, const char*, void*); void* user; };
std::map<int, PickCb> g_pending;           // game thread only
int g_nextToken = 1;
struct Result { int token; std::string value; };
std::deque<Result> g_results;
std::mutex g_mu;

const wchar_t kNul = L'\0';

std::wstring Wide(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, kNul);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string Utf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// the game's own window, so the dialog opens in front of it
BOOL CALLBACK FindWindowProc(HWND h, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr)
    {
        *reinterpret_cast<HWND*>(out) = h;
        return FALSE;
    }
    return TRUE;
}

HWND GameWindow()
{
    HWND owner = nullptr;
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&owner));
    return owner;
}

// a double-NUL terminated filter: "<label>\0<patterns>\0All files\0*.*\0\0"
std::wstring MakeFilter(const std::wstring& label, const std::wstring& patterns)
{
    std::wstring f = label;
    f += kNul;
    f += patterns;
    f += kNul;
    f += L"All files";
    f += kNul;
    f += L"*.*";
    f += kNul;
    f += kNul;
    return f;
}

std::string Choose(const std::string& destNoExt)
{
    wchar_t file[MAX_PATH * 2] = {};
    std::wstring spec = MakeFilter(L"Images (*.png;*.jpg;*.jpeg;*.bmp)", L"*.png;*.jpg;*.jpeg;*.bmp");
    OPENFILENAMEW o = {};
    o.lStructSize = sizeof(o);
    o.hwndOwner = GameWindow();
    o.lpstrFilter = spec.c_str();
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    o.lpstrTitle = L"Select image";
    if (!GetOpenFileNameW(&o)) return CommDlgExtendedError() == 0 ? "" : "!the file dialog failed (" + std::to_string(CommDlgExtendedError()) + ")";

    std::filesystem::path src(file);
    std::wstring ext = Lower(src.extension().wstring());
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".bmp") return "!only PNG, JPG and BMP images are supported";
    std::filesystem::path dst(Wide(destNoExt) + ext);
    std::error_code ec;
    if (dst.has_parent_path()) std::filesystem::create_directories(dst.parent_path(), ec);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return "!could not copy the image (error " + std::to_string(GetLastError()) + ")";
    return Utf8(dst.wstring());
}

// any file with one of the extensions (comma separated, no dots): copied byte for byte to dest + its extension; "<path>\n<file name>"
std::string ChooseFile(const std::string& destNoExt, const std::string& exts)
{
    std::vector<std::wstring> list;
    {
        size_t i = 0;
        while (i <= exts.size())
        {
            size_t j = exts.find(',', i);
            if (j == std::string::npos) j = exts.size();
            std::wstring e = Lower(Wide(exts.substr(i, j - i)));
            if (!e.empty()) list.push_back(e);
            i = j + 1;
        }
    }
    std::wstring label = L"(", patterns;
    for (size_t k = 0; k < list.size(); k++)
    {
        if (k) { label += L"; "; patterns += L";"; }
        label += L"*." + list[k];
        patterns += L"*." + list[k];
    }
    label += L")";

    wchar_t file[MAX_PATH * 2] = {};
    std::wstring spec = MakeFilter(label, patterns);
    OPENFILENAMEW o = {};
    o.lStructSize = sizeof(o);
    o.hwndOwner = GameWindow();
    o.lpstrFilter = spec.c_str();
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&o)) return CommDlgExtendedError() == 0 ? "" : "!the file dialog failed (" + std::to_string(CommDlgExtendedError()) + ")";

    std::filesystem::path src(file);
    std::wstring ext = Lower(src.extension().wstring());
    if (!ext.empty()) ext.erase(0, 1);
    bool ok = false;
    for (auto& e : list) if (e == ext) ok = true;
    if (!ok) return "!unsupported file type: ." + Utf8(ext);
    std::filesystem::path dst(Wide(destNoExt) + L"." + ext);
    std::error_code ec;
    if (dst.has_parent_path()) std::filesystem::create_directories(dst.parent_path(), ec);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return "!could not copy the file (error " + std::to_string(GetLastError()) + ")";
    return Utf8(dst.wstring()) + "\n" + Utf8(src.filename().wstring());
}

// "Save as": a save dialog, then the file is copied there. Returns the chosen path.
std::string SaveAs(const std::string& srcPath, const std::string& suggested)
{
    wchar_t file[MAX_PATH * 2] = {};
    std::wstring sug = Wide(suggested);
    wcsncpy_s(file, sug.c_str(), _TRUNCATE);
    std::wstring ext = std::filesystem::path(sug).extension().wstring();
    if (!ext.empty()) ext.erase(0, 1);
    std::wstring spec = MakeFilter(ext.empty() ? L"All files" : (L"*." + ext), ext.empty() ? L"*.*" : (L"*." + ext));
    OPENFILENAMEW o = {};
    o.lStructSize = sizeof(o);
    o.hwndOwner = GameWindow();
    o.lpstrFilter = spec.c_str();
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    o.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&o)) return CommDlgExtendedError() == 0 ? "" : "!the file dialog failed (" + std::to_string(CommDlgExtendedError()) + ")";
    if (!CopyFileW(Wide(srcPath).c_str(), file, FALSE)) return "!could not write the file (error " + std::to_string(GetLastError()) + ")";
    return Utf8(file);
}

bool Launch(std::function<std::string()> work, void (*cb)(int, const char*, const char*, void*), void* user)
{
    int token = g_nextToken++;
    g_pending[token] = {cb, user};
    try
    {
        std::thread([token, work]() {
            std::string v = work();
            std::lock_guard<std::mutex> g(g_mu);
            g_results.push_back({token, v});
        }).detach();
    }
    catch (...) { g_pending.erase(token); return false; }
    return true;
}
}   // namespace

bool NativeUi_PickImage(const char* destNoExt, void (*cb)(int, const char*, const char*, void*), void* user)
{
    if (!destNoExt || !cb) return false;
    std::string d = destNoExt;
    return Launch([d]() { return Choose(d); }, cb, user);
}

bool NativeUi_PickFile(const char* destNoExt, const char* exts, void (*cb)(int, const char*, const char*, void*), void* user)
{
    if (!destNoExt || !exts || !cb) return false;
    std::string d = destNoExt, e = exts;
    return Launch([d, e]() { return ChooseFile(d, e); }, cb, user);
}

bool NativeUi_SaveFile(const char* src, const char* suggestedName, void (*cb)(int, const char*, const char*, void*), void* user)
{
    if (!src || !suggestedName || !cb) return false;
    std::string s = src, n = suggestedName;
    return Launch([s, n]() { return SaveAs(s, n); }, cb, user);
}

// ---- keyboard: "F8", "CTRL+SHIFT+K": every part must be down, and the game window must have the focus
namespace
{
int VkOf(const std::string& name)
{
    std::string n;
    for (char c : name) n += (char)toupper((unsigned char)c);
    if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
    if (n.size() >= 2 && n[0] == 'F')
    {
        int k = atoi(n.c_str() + 1);
        if (k >= 1 && k <= 24) return VK_F1 + k - 1;
    }
    static const struct { const char* n; int vk; } keys[] = {
        {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"SHIFT", VK_SHIFT}, {"ALT", VK_MENU}, {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE},
        {"ENTER", VK_RETURN}, {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME},
        {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"TILDE", VK_OEM_3}, {"GRAVE", VK_OEM_3}, {"LMB", VK_LBUTTON}, {"MOUSE1", VK_LBUTTON}, {"RMB", VK_RBUTTON}, {"MOUSE2", VK_RBUTTON}};
    for (auto& k : keys) if (n == k.n) return k.vk;
    return 0;
}
}   // namespace

bool Platform_KeyDown(const char* spec)
{
    if (!spec || !*spec) return false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return false;
    std::string s = spec;
    size_t i = 0;
    bool any = false;
    while (i <= s.size())
    {
        size_t j = s.find('+', i);
        if (j == std::string::npos) j = s.size();
        std::string part = s.substr(i, j - i);
        if (!part.empty())
        {
            int vk = VkOf(part);
            if (!vk || !(GetAsyncKeyState(vk) & 0x8000)) return false;
            any = true;
        }
        i = j + 1;
    }
    return any;
}

// game thread, every tick
void NativeUi_Drain()
{
    std::deque<Result> done;
    { std::lock_guard<std::mutex> g(g_mu); done.swap(g_results); }
    for (Result& r : done)
    {
        auto it = g_pending.find(r.token);
        if (it == g_pending.end()) continue;
        PickCb pc = it->second;
        g_pending.erase(it);
        pc.cb(-r.token, "$picked", r.value.c_str(), pc.user);
    }
}

int NativeUi_Show(const char*, void (*)(int, const char*, const char*, void*), void*) { return -1; }
void NativeUi_Set(int, const char*, const char*) {}
void NativeUi_Close(int) {}
void* NativeUi_JavaVM() { return nullptr; }
void* NativeUi_Env() { return nullptr; }
void* NativeUi_AppClassLoader() { return nullptr; }
void* NativeUi_Activity() { return nullptr; }
void NativeUi_RunOnUi(void (*)(void*), void*) {}
