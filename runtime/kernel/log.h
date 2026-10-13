// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runtime log: stderr plus an optional file (--log). C callers use vc_log() from vc_iop.h.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace vc {

void LogInit(const char* path); // nullptr = stderr only
void LogShutdown();
void LogV(const char* tag, const char* fmt, va_list ap);
void Log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// Write raw text (EE TTY output) without a prefix.
void LogRaw(const char* text, size_t len);
[[noreturn]] void Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Hook run by Fatal() before aborting (the kernel prints EE thread state).
void SetFatalHook(void (*hook)());

} // namespace vc
