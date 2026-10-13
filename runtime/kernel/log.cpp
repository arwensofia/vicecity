// SPDX-License-Identifier: GPL-3.0-or-later

#include "log.h"

#include "vc_iop.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace vc {

namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
void (*g_fatal_hook)() = nullptr;

double Seconds()
{
	static timespec start = [] {
		timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		return t;
	}();
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return static_cast<double>(t.tv_sec - start.tv_sec) + static_cast<double>(t.tv_nsec - start.tv_nsec) * 1e-9;
}
} // namespace

void LogInit(const char* path)
{
	Seconds();
	if (path) {
		g_file = std::fopen(path, "w");
		if (!g_file)
			std::fprintf(stderr, "vicecity: cannot open log file %s\n", path);
	}
}

void LogShutdown()
{
	std::lock_guard lock(g_mutex);
	if (g_file)
		std::fclose(g_file);
	g_file = nullptr;
}

void LogV(const char* tag, const char* fmt, va_list ap)
{
	char msg[2048];
	std::vsnprintf(msg, sizeof msg, fmt, ap);
	std::lock_guard lock(g_mutex);
	const double t = Seconds();
	std::fprintf(stderr, "[%9.4f] %-6s %s\n", t, tag, msg);
	if (g_file) {
		std::fprintf(g_file, "[%9.4f] %-6s %s\n", t, tag, msg);
		std::fflush(g_file);
	}
}

void Log(const char* tag, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	LogV(tag, fmt, ap);
	va_end(ap);
}

void LogRaw(const char* text, size_t len)
{
	std::lock_guard lock(g_mutex);
	std::fwrite(text, 1, len, stdout);
	std::fflush(stdout);
	if (g_file) {
		std::fwrite(text, 1, len, g_file);
		std::fflush(g_file);
	}
}

void SetFatalHook(void (*hook)()) { g_fatal_hook = hook; }

void Fatal(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	LogV("FATAL", fmt, ap);
	va_end(ap);
	if (g_fatal_hook) {
		auto hook = g_fatal_hook;
		g_fatal_hook = nullptr;
		hook();
	}
	LogShutdown();
	std::fflush(stderr);
	std::abort();
}

} // namespace vc

extern "C" void vc_log(const char* tag, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vc::LogV(tag, fmt, ap);
	va_end(ap);
}
