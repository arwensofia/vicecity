// SPDX-License-Identifier: GPL-3.0-or-later
//
// COP0 (status/interrupt enable, counters), COP2 control (VU0 micro mode, VU1 start) and
// the remaining exceptional hooks of ps2_abi.h.

#include "kernel.h"
#include "log.h"
#include "vc_gs.h"

#include <cstring>
#include <map>

using namespace vc;
using namespace vc::kernel;

namespace {

constexpr uint32_t kStatusEIE = 0x00010000u;
uint32_t g_cop0[32];
uint32_t g_vu0_cmsar0 = 0;
uint32_t g_vu0_cmsar1 = 0;

void LogOncePerPc(const char* what, uint32_t pc, uint32_t code)
{
	static std::map<uint64_t, int> seen;
	const uint64_t key = (static_cast<uint64_t>(pc) << 8) ^ reinterpret_cast<uintptr_t>(what);
	if (seen[key]++ == 0)
		Log("cpu", "%s at %08x (code %x, thread %d)", what, pc, code, CurrentThreadId());
}

} // namespace

extern "C" {

void ps2_break(ps2_cpu* c, uint32_t pc, uint32_t code)
{
	(void)c;
	LogOncePerPc("break", pc, code);
}

void ps2_trap(ps2_cpu* c, uint32_t pc, uint32_t code)
{
	(void)c;
	LogOncePerPc("trap", pc, code);
}

uint32_t ps2_cop0_read(ps2_cpu* c, uint32_t pc, uint32_t reg, uint32_t sel)
{
	(void)c;
	(void)pc;
	switch (reg) {
	case 9: // Count: EE core clock (294.912 MHz)
		return static_cast<uint32_t>(NowNs() * 294912 / 1000000);
	case 12: // Status
		return ThreadCop0Status();
	case 15: // PRId: R5900 rev 2.0
		return 0x2E20;
	case 16: // Config
		return 0x00000440u;
	case 25: // performance counters
		(void)sel;
		return 0;
	default:
		return g_cop0[reg & 31];
	}
}

void ps2_cop0_write(ps2_cpu* c, uint32_t pc, uint32_t reg, uint32_t sel, uint32_t value)
{
	(void)c;
	(void)pc;
	(void)sel;
	if (reg == 12) {
		const bool was = ThreadInterruptsEnabled();
		SetThreadCop0Status(value);
		if (!was && (value & kStatusEIE))
			Poll();
		return;
	}
	g_cop0[reg & 31] = value;
}

void ps2_cop0_op(ps2_cpu* c, uint32_t pc, uint32_t insn)
{
	(void)c;
	switch (insn & 0x3F) {
	case 0x38: // ei
		if (InInterrupt())
			return; // handlers run with interrupts off; the kernel restores Status on return
		SetThreadInterruptsEnabled(true);
		Poll();
		return;
	case 0x39: // di
		if (!InInterrupt())
			SetThreadInterruptsEnabled(false);
		return;
	case 0x18: // eret
		LogOncePerPc("eret (ignored)", pc, insn);
		return;
	default: // tlbr/tlbwi/tlbwr/tlbp: the runtime has no TLB
		return;
	}
}

int ps2_cop0_cond(ps2_cpu* c)
{
	(void)c;
	// CPCOND0: all DMA channels selected in D_PCR.CPC have their D_STAT.CIS bit set.
	uint32_t pcr = 0, stat = 0;
	vc::gs::HwRead32(0x1000E020u, &pcr);
	vc::gs::HwRead32(0x1000E010u, &stat);
	return (((~pcr) | stat) & 0x3FF) == 0x3FF;
}

int ps2_cop2_cond(ps2_cpu* c)
{
	(void)c;
	return 0; // VU0 micro programs run synchronously: never busy
}

void ps2_cache(ps2_cpu* c, uint32_t pc, uint32_t op, uint32_t addr)
{
	(void)c;
	(void)pc;
	(void)op;
	(void)addr;
}

void ps2_vu0_callms(ps2_cpu* c, uint32_t pc, uint32_t addr)
{
	static int logged;
	ps2_u128 before[32];
	const bool log = logged < 4;
	if (log)
		std::memcpy(before, c->vf, sizeof before);
	vc::gs::ExecuteVU0(static_cast<int32_t>((addr >> 3) & 0x1FF), c->vf, &c->vacc, c->vi);
	if (log) {
		logged++;
		int changed = 0;
		for (int i = 0; i < 32; i++)
			changed += std::memcmp(&before[i], &c->vf[i], 16) != 0;
		uint32_t w[2];
		std::memcpy(w, g_vu0_micro + (addr & 0xFF8), 8);
		Log("vu0", "vcallms %03x at %08x: code %08x %08x, %d VF registers changed, TPC %x", addr, pc, w[1], w[0],
			changed, c->vi[26]);
	}
}

void ps2_vu0_wait(ps2_cpu* c) { (void)c; }

uint32_t ps2_vu0_ctrl_read(ps2_cpu* c, uint32_t reg)
{
	switch (reg) {
	case 26: return c->vi[26]; // TPC (written by ExecuteVU0)
	case 27: return g_vu0_cmsar0;
	case 28: return 0; // FBRST
	case 29: return vc::gs::VU1Busy() ? 0x100u : 0u; // VPU-STAT
	case 31: return g_vu0_cmsar1;
	default: return c->vi[reg & 31];
	}
}

void ps2_vu0_ctrl_write(ps2_cpu* c, uint32_t reg, uint32_t value)
{
	switch (reg) {
	case 27:
		g_vu0_cmsar0 = value & 0xFFFF;
		break;
	case 28: // FBRST: RST0 (bit 1), RST1 (bit 9); force-breaks are no-ops (programs finish)
		if (value & 0x2)
			vc::gs::ResetVU0();
		if (value & 0x200)
			vc::gs::ResetVU1();
		break;
	case 31: // CMSAR1: start VU1 at value (64-bit units)
		g_vu0_cmsar1 = value & 0xFFFF;
		vc::gs::StartVU1(value & 0xFFFF);
		if (!InInterrupt())
			Poll();
		break;
	default:
		c->vi[reg & 31] = value;
		break;
	}
}

void ps2_unimplemented(ps2_cpu* c, uint32_t pc, uint32_t insn)
{
	DumpCpu(c);
	Fatal("unimplemented instruction %08x at %08x", insn, pc);
}

} // extern "C"
