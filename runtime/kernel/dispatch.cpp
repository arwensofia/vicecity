// SPDX-License-Identifier: GPL-3.0-or-later
//
// Indirect control flow (jalr / unresolved jr), helpers to call EE functions from the
// runtime, function-entry tracing and crash diagnostics.

#include "kernel.h"
#include "log.h"
#include "ps2_recomp.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <unistd.h>

namespace vc::kernel {

namespace {

bool g_trace_print = false;

void DumpTrace()
{
	ps2_trace_dump(STDERR_FILENO, 64);
}

void CrashHandler(int sig, siginfo_t* info, void*)
{
	char buf[256];
	char where[96];
	ps2_recomp_format_addr(CurrentCpu() ? CurrentCpu()->pc : 0, where, sizeof where);
	const int n = snprintf(buf, sizeof buf, "\n*** signal %d (%s) at host address %p, EE thread %d, last EE pc %s\n",
		sig, strsignal(sig), info ? info->si_addr : nullptr, CurrentThreadId(), where);
	write(STDERR_FILENO, buf, static_cast<size_t>(n));
	void* frames[64];
	const int count = backtrace(frames, 64);
	backtrace_symbols_fd(frames, count, STDERR_FILENO);
	// Not async-signal-safe, but we are going down anyway and the log is what matters.
	Log("crash", "signal %d (%s) at %p", sig, strsignal(sig), info ? info->si_addr : nullptr);
	char** syms = backtrace_symbols(frames, count);
	for (int i = 0; syms && i < count; i++)
		Log("crash", "  %s", syms[i]);
	DumpTrace();
	if (CurrentCpu())
		DumpCpu(CurrentCpu());
	DumpThreads();
	LogShutdown();
	signal(sig, SIG_DFL);
	raise(sig);
}

[[noreturn]] void BadTarget(const char* kind, ps2_cpu* c, uint32_t pc, uint32_t target)
{
	DumpTrace();
	DumpCpu(c);
	char site[96];
	ps2_recomp_format_addr(pc, site, sizeof site);
	Fatal("%s from %08x (%s) to %08x: not a recompiled function entry", kind, pc, site, target);
}

} // namespace

ps2_func_t LookupFunction(uint32_t addr)
{
	return ps2_lookup_function(ps2_recomp_functions, ps2_recomp_function_count, addr);
}

uint32_t CallEE(ps2_cpu* c, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	auto sext = [](uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); };
	c->r[4].ud[0] = sext(a0);
	c->r[5].ud[0] = sext(a1);
	c->r[6].ud[0] = sext(a2);
	c->r[7].ud[0] = sext(a3);
	ps2_call_indirect(c, 0, func);
	return c->r[2].uw[0];
}

void SetTracePrint(bool on) { g_trace_print = on; }

void InstallCrashHandlers()
{
	static uint8_t altstack[256 * 1024];
	stack_t ss{};
	ss.ss_sp = altstack;
	ss.ss_size = sizeof altstack;
	sigaltstack(&ss, nullptr);
	struct sigaction sa{};
	sa.sa_sigaction = CrashHandler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT})
		sigaction(sig, &sa, nullptr);
	SetFatalHook([] {
		DumpTrace();
		void* frames[48];
		const int count = backtrace(frames, 48);
		char** syms = backtrace_symbols(frames, count);
		for (int i = 0; syms && i < count; i++)
			Log("fatal", "  %s", syms[i]);
		DumpThreads();
	});
}

void DumpCpu(const ps2_cpu* c)
{
	static const char* names[32] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4",
		"t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp",
		"ra"};
	Log("cpu", "EE context %p (thread %d) last hook pc %08x", static_cast<const void*>(c), CurrentThreadId(), c->pc);
	for (int i = 0; i < 32; i += 4)
		Log("cpu", "  %s=%016llx %s=%016llx %s=%016llx %s=%016llx", names[i],
			static_cast<unsigned long long>(c->r[i].ud[0]), names[i + 1],
			static_cast<unsigned long long>(c->r[i + 1].ud[0]), names[i + 2],
			static_cast<unsigned long long>(c->r[i + 2].ud[0]), names[i + 3],
			static_cast<unsigned long long>(c->r[i + 3].ud[0]));
}

} // namespace vc::kernel

using namespace vc;
using namespace vc::kernel;

extern "C" {

void ps2_call_indirect(ps2_cpu* c, uint32_t pc, uint32_t target)
{
	ps2_func_t f = LookupFunction(target);
	if (__builtin_expect(f == nullptr, 0))
		BadTarget("call", c, pc, target);
	f(c);
}

void ps2_jump_indirect(ps2_cpu* c, uint32_t pc, uint32_t target)
{
	ps2_func_t f = LookupFunction(target);
	if (__builtin_expect(f == nullptr, 0))
		BadTarget("jump", c, pc, target);
	f(c);
}

// Function-entry hook for recompiled code built with PS2_TRACE bit 0 (VC_GAME_TRACE=1):
// record into the recomp support ring buffer and optionally print (--trace).
void ps2_trace_fn(ps2_cpu* c, uint32_t entry)
{
	ps2_trace_record(c, entry, PS2_TRACE_KIND_FN);
	if (g_trace_print) {
		char name[96];
		ps2_recomp_format_addr(entry, name, sizeof name);
		Log("trace", "[t%d] %s", CurrentThreadId(), name);
	}
}

// Back-edge hook (every backward branch: if (--ps2_backedge_budget < 0) ps2_backedge(...)):
// the interrupt delivery point inside loops that spin on RAM (main's wait for 100 vblanks,
// RPC status polls). Interrupt handlers run on their own EE context (intr.cpp), so *c is
// not modified except by a thread switch, which preserves it.
void ps2_backedge(ps2_cpu* c, uint32_t loop_head)
{
	(void)c;
	(void)loop_head;
	ps2_backedge_budget = PS2_BACKEDGE_DEFAULT_BUDGET;
	if (!InInterrupt())
		Poll();
}

// Basic-block hook (VC_GAME_TRACE bit 1, debugging only): record into the trace ring.
void ps2_trace_bb(ps2_cpu* c, uint32_t addr)
{
	ps2_trace_record(c, addr, PS2_TRACE_KIND_BB);
}

} // extern "C"
