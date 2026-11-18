// kmod logging: logcat + a rotating log file, plus a native crash handler that writes the fatal signal and a backtrace.
//
//   <root>/logs/kmod.log      current run
//   <root>/logs/kmod.1.log    previous run ... kmod.4.log (the 5 most recent runs are kept)
//
// File line format:  2026-11-16 12:34:56.789 I/lua: message        (D/I/W/E)
// Level filter: dev builds log DEBUG and up, release builds INFO and up.
#pragma once
#include <string>

enum KLogLevel { KLOG_DEBUG = 0, KLOG_INFO = 1, KLOG_WARN = 2, KLOG_ERROR = 3 };

void KLogInit();                                                  // idempotent; rotates old logs, writes the header
void KLog(KLogLevel level, const char* tag, const char* fmt, ...);
void KLogMsg(KLogLevel level, const char* tag, const std::string& msg);
const std::string& KLogDir();
void KLogInstallCrashHandlers();                                  // safe to call again: re-hooks if the engine replaced ours
