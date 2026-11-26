// kmod loader version info.
#pragma once

#define KMOD_VERSION "0.4.1"
#define KMOD_API_LEVEL 1       // mod pack format / Lua API level (kmod.api)

#ifdef KMOD_DEV
#define KMOD_FLAVOR "dev"
#else
#define KMOD_FLAVOR "release"
#endif
