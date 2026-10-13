// SPDX-License-Identifier: GPL-3.0-or-later
//
// HLE overrides of individual EE functions (strong ps2f_XXXXXXXX definitions replace the
// recompiler's weak ones; see ps2_abi.h "Overriding"). Addresses are SLUS_205.52 (NTSC-U
// v1.40); evidence in tools/library_names.csv and docs/MODULES.md.

#include "kernel.h"
#include "log.h"
#include "ps2_recomp.h"

using namespace vc;
using namespace vc::kernel;

extern "C" {

// Recompiled functions called from the overrides.
void ps2f_0011c040(ps2_cpu* c); // libkernl: creates the two library semaphores
void ps2f_00114428(ps2_cpu* c); // libkernl InitThread: helper thread for i*Thread requests

// libkernl _InitSys (called by crt0). The original also installs kernel-mode patches with
// SetSyscall/kCopy (alarms, TLB helpers, OSD config, a syscall-table probe via syscall
// 0x83). The runtime kernel implements those syscalls natively, so only the parts that
// create user-visible state are kept.
void ps2f_0011c140(ps2_cpu* c)
{
	Log("hle", "_InitSys");
	ps2f_0011c040(c);
	ps2f_00114428(c);
	c->r[2].ud[0] = 0;
}

// libgcc/libstdc++ setjmp/longjmp exception throw paths (__sjthrow 0x11d088, __throw and
// rethrow variants 0x11d200, 0x11da60, 0x11dc40): they unwind into a __builtin_setjmp
// receiver in the middle of a function, which statically recompiled code cannot resume.
// Nothing is expected to throw in this game; stop with diagnostics if it does.
[[noreturn]] static void UncaughtException(ps2_cpu* c, uint32_t fn)
{
	char caller[96];
	ps2_recomp_format_addr(c->r[31].uw[0], caller, sizeof caller);
	ps2_trace_dump(2, 64);
	DumpCpu(c);
	Fatal("uncaught C++ exception: %08x called from %s", fn, caller);
}

void ps2f_0011d088(ps2_cpu* c) { UncaughtException(c, 0x0011d088u); }
void ps2f_0011d200(ps2_cpu* c) { UncaughtException(c, 0x0011d200u); }
void ps2f_0011da60(ps2_cpu* c) { UncaughtException(c, 0x0011da60u); }
void ps2f_0011dc40(ps2_cpu* c) { UncaughtException(c, 0x0011dc40u); }

// Game FMV player: play one movie (path, a1, a2) through libmpeg + IPU (BOOT.md 1.4; only
// caller is the logo sequence 0x273920: VCNTSC.PSS, VICE.PSS). The IPU is not emulated yet
// (PCSX2's IPU is not imported), so the player would spin on IPU_CTRL forever; skip movies.
void ps2f_003819b0(ps2_cpu* c)
{
	Log("hle", "FMV '%s' skipped (no IPU emulation)", LoadCString(c->r[4].uw[0]).c_str());
	c->r[2].ud[0] = 0;
}

} // extern "C"
