// Windows implementations of the POSIX-style helpers declared in compat.h, plus KModRoot().
#include "compat.h"

#include <string>
#include <vector>

extern "C" {
FILE* kmod_fopen(const char* path, const char* mode)
{
    std::wstring w = KWide(path), m = KWide(mode);
    FILE* f = nullptr;
    if (_wfopen_s(&f, w.c_str(), m.c_str()) != 0) return nullptr;
    return f;
}
int kmod_remove(const char* path)
{
    std::wstring w = KWide(path);
    if (DeleteFileW(w.c_str())) return 0;
    if (RemoveDirectoryW(w.c_str())) return 0;                 // C's remove() also removes empty directories on Windows
    return -1;
}
int kmod_rename(const char* from, const char* to)
{
    return MoveFileExW(KWide(from).c_str(), KWide(to).c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
}

int kmod_mkdir(const char* path, int) { return CreateDirectoryW(KWide(path).c_str(), nullptr) ? 0 : -1; }
int kmod_rmdir(const char* path) { return RemoveDirectoryW(KWide(path).c_str()) ? 0 : -1; }
int kmod_unlink(const char* path) { return DeleteFileW(KWide(path).c_str()) ? 0 : -1; }
int kmod_access(const char* path, int) { return GetFileAttributesW(KWide(path).c_str()) != INVALID_FILE_ATTRIBUTES ? 0 : -1; }

int kmod_stat_fn(const char* path, struct kmod_stat_fn* st)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(KWide(path).c_str(), GetFileExInfoStandard, &d)) return -1;
    st->st_size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    ULARGE_INTEGER t; t.LowPart = d.ftLastWriteTime.dwLowDateTime; t.HighPart = d.ftLastWriteTime.dwHighDateTime;
    st->st_mtime = (long long)(t.QuadPart / 10000000ULL) - 11644473600LL;
    st->st_mode = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0040000 : 0100000;
    return 0;
}

struct DIR { HANDLE h; WIN32_FIND_DATAW data; bool first; dirent ent; };

DIR* opendir(const char* path)
{
    std::wstring pat = KWide(path);
    if (!pat.empty() && pat.back() != L'/' && pat.back() != L'\\') pat += L'\\';
    pat += L'*';
    DIR* d = new DIR();
    d->h = FindFirstFileW(pat.c_str(), &d->data);
    if (d->h == INVALID_HANDLE_VALUE) { delete d; return nullptr; }
    d->first = true;
    return d;
}

dirent* readdir(DIR* d)
{
    if (!d) return nullptr;
    if (d->first) d->first = false;
    else if (!FindNextFileW(d->h, &d->data)) return nullptr;
    std::string u = KUtf8(d->data.cFileName);
    strncpy_s(d->ent.d_name, sizeof(d->ent.d_name), u.c_str(), _TRUNCATE);
    return &d->ent;
}

int closedir(DIR* d)
{
    if (!d) return -1;
    FindClose(d->h);
    delete d;
    return 0;
}

int clock_gettime(int clk, struct timespec* ts)
{
    if (clk == CLOCK_MONOTONIC)
    {
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        ts->tv_sec = (time_t)(c.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((c.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    }
    else
    {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER t; t.LowPart = ft.dwLowDateTime; t.HighPart = ft.dwHighDateTime;
        unsigned long long u = t.QuadPart - 116444736000000000ULL;
        ts->tv_sec = (time_t)(u / 10000000ULL);
        ts->tv_nsec = (long)((u % 10000000ULL) * 100);
    }
    return 0;
}

// The folder kmod_core.dll was loaded from: <game>/kards/Binaries/Win64/kmod
std::string KModRoot()
{
    static const std::string root = []
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&KModRoot, &m);
        wchar_t buf[MAX_PATH * 4] = {};
        GetModuleFileNameW(m, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        std::string p = KUtf8(buf);
        for (char& c : p) if (c == '\\') c = '/';
        size_t slash = p.rfind('/');
        return slash == std::string::npos ? std::string("kmod") : p.substr(0, slash);
    }();
    return root;
}
