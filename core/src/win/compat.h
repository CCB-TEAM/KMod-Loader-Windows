// POSIX-style helpers for the shared (Android-derived) sources on Windows. Force-included into every core source (/FI).
// All paths in kmod are UTF-8: the wrappers below convert to UTF-16 and call the wide Win32/CRT functions.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// windows.h defines a global TCHAR that clashes with the SDK's (char16_t): rename it while windows.h is read
#define TCHAR kmod_win_TCHAR
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <time.h>
#undef TCHAR

typedef int pid_t;
#define STDIN_FILENO 0

inline std::wstring KWide(const char* s)
{
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
inline std::wstring KWide(const std::string& s) { return KWide(s.c_str()); }
inline std::string KUtf8(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// ---- files (UTF-8 paths)
extern "C" {
FILE* kmod_fopen(const char* path, const char* mode);
int kmod_remove(const char* path);
int kmod_rename(const char* from, const char* to);
}
int kmod_mkdir(const char* path, int mode = 0);
int kmod_rmdir(const char* path);
int kmod_unlink(const char* path);
int kmod_access(const char* path, int mode);
// "struct stat st; stat(path, &st)" in the sources: a struct and a function can share a name, as in POSIX
struct kmod_stat_fn { unsigned long long st_size; long long st_mtime; unsigned st_mode; };
int kmod_stat_fn(const char* path, struct kmod_stat_fn* st);

#define fopen kmod_fopen
#define mkdir kmod_mkdir
#define rmdir kmod_rmdir
#define unlink kmod_unlink
#define access kmod_access
#define stat kmod_stat_fn
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define S_ISREG(m) (((m) & 0170000) == 0100000)
#define R_OK 4
#define W_OK 2
#define F_OK 0

// ---- directories
struct dirent { char d_name[520]; };
struct DIR;
DIR* opendir(const char* path);
dirent* readdir(DIR* d);
int closedir(DIR* d);

// ---- misc
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
inline pid_t getpid() { return (pid_t)GetCurrentProcessId(); }
inline pid_t gettid() { return (pid_t)GetCurrentThreadId(); }
inline void usleep(unsigned us) { Sleep(us < 1000 ? 1 : us / 1000); }
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0
struct timespec_k { long long tv_sec; long tv_nsec; };
#endif
int clock_gettime(int clk, struct timespec* ts);
#define PATH_SEP "/"
