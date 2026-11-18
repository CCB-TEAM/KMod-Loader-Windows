// Force-included into the Lua sources: io.open / os.remove / os.rename must take UTF-8 paths on Windows.
#pragma once
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
FILE* kmod_fopen(const char* path, const char* mode);
int kmod_remove(const char* path);
int kmod_rename(const char* from, const char* to);
#ifdef __cplusplus
}
#endif
#define fopen kmod_fopen
#define remove kmod_remove
#define rename kmod_rename
#define l_getc_noargs 1
