// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE memory map outside the ps2_abi.h fast path (main RAM below 32 MB and the scratchpad):
// RAM mirrors, EE hardware registers, VU memory, GS privileged registers, BIOS ROM.
//
//   0x10000000-0x10001FFF  timers                     -> kernel (intr.cpp)
//   0x10002000-0x10002FFF  IPU registers              -> not implemented (logged)
//   0x10003000-0x1000EFFF  GIF/VIF/FIFOs/DMAC         -> vc::gs::Hw*
//   0x1000F000/F010        INTC_STAT/INTC_MASK        -> vc::gs::Hw*
//   0x1000F180             KPUTCHAR (SIO TX)          -> runtime log
//   0x1000F200-0x1000F2FF  SIF                        -> vc_iop_sif_*
//   0x1000F520/F590        D_ENABLER/D_ENABLEW        -> vc::gs::Hw*
//   0x11000000-0x1100FFFF  VU0/VU1 micro + data memory
//   0x12000000-0x12001FFF  GS privileged registers    -> vc::gs::GsPriv*
//   0x1FC00000-0x1FFFFFFF  BIOS ROM                   -> reads as zero
// Register accesses are interrupt delivery points (kernel Poll()).

#include "kernel.h"
#include "log.h"
#include "ps2_recomp.h"
#include "vc_gs.h"
#include "vc_iop.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace vc::kernel {

uint8_t* g_ram = nullptr;
uint8_t* g_spr = nullptr;
uint8_t* g_vu0_mem = nullptr;
uint8_t* g_vu0_micro = nullptr;
uint8_t* g_vu1_mem = nullptr;
uint8_t* g_vu1_micro = nullptr;

void InitMemory(uint8_t* ram, uint8_t* spr, uint8_t* vu0_mem, uint8_t* vu0_micro, uint8_t* vu1_mem,
	uint8_t* vu1_micro)
{
	g_ram = ram;
	g_spr = spr;
	g_vu0_mem = vu0_mem;
	g_vu0_micro = vu0_micro;
	g_vu1_mem = vu1_mem;
	g_vu1_micro = vu1_micro;
}

namespace {

enum class Region
{
	Ram,
	Spr,
	Timer,
	Hw,     // vc_gs-owned or other EE registers
	Vu,
	GsPriv,
	Rom,
	Unmapped,
};

// Translate an EE virtual address to a physical one (KSEG0/1 and the uncached mirrors).
inline uint32_t Phys(uint32_t addr)
{
	const uint32_t top = addr >> 28;
	if (top >= 0x8 && top <= 0xB)
		return addr & 0x1FFFFFFFu;
	if (top == 0x2 || top == 0x3) // uncached / uncached-accelerated RAM mirrors
		return addr & 0x0FFFFFFFu;
	return addr;
}

inline Region Classify(uint32_t addr, uint32_t* phys)
{
	if ((addr & ~(PS2_SPR_SIZE - 1)) == PS2_SPR_BASE) {
		*phys = addr & (PS2_SPR_SIZE - 1);
		return Region::Spr;
	}
	const uint32_t p = Phys(addr);
	*phys = p;
	if (p < PS2_RAM_SIZE)
		return Region::Ram;
	if (p >= 0x10000000u && p < 0x10002000u)
		return Region::Timer;
	if (p >= 0x10002000u && p < 0x10010000u)
		return Region::Hw;
	if (p >= 0x11000000u && p < 0x11010000u)
		return Region::Vu;
	if (p >= 0x12000000u && p < 0x12002000u)
		return Region::GsPriv;
	if (p >= 0x1FC00000u && p < 0x20000000u)
		return Region::Rom;
	return Region::Unmapped;
}

uint8_t* VuPtr(uint32_t p)
{
	if (p >= 0x1100C000u)
		return g_vu1_mem + (p & 0x3FFF);
	if (p >= 0x11008000u)
		return g_vu1_micro + (p & 0x3FFF);
	if (p >= 0x11004000u)
		return g_vu0_mem + (p & 0xFFF);
	return g_vu0_micro + (p & 0xFFF);
}

void LogOnce(const char* what, uint32_t addr, uint64_t value)
{
	static std::map<uint64_t, int> seen;
	const uint64_t key = (static_cast<uint64_t>(addr) << 4) ^ reinterpret_cast<uintptr_t>(what);
	int& n = seen[key];
	if (n++ == 0) {
		Log("mem", "%s %08x (value %llx, thread %d, pc %08x)", what, addr, static_cast<unsigned long long>(value),
			CurrentThreadId(), CurrentCpu() ? CurrentCpu()->pc : 0);
		static bool dumped;
		if (!dumped && std::strstr(what, "unmapped") && std::getenv("VC_TRACE_ON_UNMAPPED")) {
			dumped = true; // needs VC_GAME_TRACE >= 1 to have anything in the ring
			ps2_trace_dump(2, static_cast<unsigned>(std::atoi(std::getenv("VC_TRACE_ON_UNMAPPED"))));
			if (CurrentCpu())
				DumpCpu(CurrentCpu());
		}
	}
}

std::string g_tty_line;

void KPutChar(uint8_t ch)
{
	if (ch == '\n') {
		Log("tty", "%s", g_tty_line.c_str());
		g_tty_line.clear();
	} else if (ch != '\r') {
		g_tty_line.push_back(static_cast<char>(ch));
	}
}

// --- 32-bit register access ---------------------------------------------------------

uint32_t HwRead32(uint32_t p)
{
	uint32_t v = 0;
	if (p >= 0x1000F200u && p < 0x1000F300u)
		return vc_iop_sif_read32(p & ~3u);
	if (vc::gs::HwRead32(p, &v))
		return v;
	switch (p & ~3u) {
	case 0x1000F130u: // SIO status
		return 0;
	case 0x1000F430u: // MCH_RICM
	case 0x1000F440u: // MCH_DRD
		return 0;
	default:
		LogOnce("unhandled register read", p, 0);
		return 0;
	}
}

void HwWrite32(uint32_t p, uint32_t v)
{
	if (p >= 0x1000F200u && p < 0x1000F300u) {
		vc_iop_sif_write32(p & ~3u, v);
		return;
	}
	if (p == 0x1000F180u) {
		KPutChar(static_cast<uint8_t>(v));
		return;
	}
	if (vc::gs::HwWrite32(p, v))
		return;
	LogOnce("unhandled register write", p, v);
}

uint32_t RegRead32(Region r, uint32_t p)
{
	switch (r) {
	case Region::Timer: return TimerRead(p);
	case Region::Hw: return HwRead32(p);
	case Region::GsPriv: return vc::gs::GsPrivRead32(p);
	default: return 0;
	}
}

void RegWrite32(Region r, uint32_t p, uint32_t v)
{
	switch (r) {
	case Region::Timer: TimerWrite(p, v); break;
	case Region::Hw: HwWrite32(p, v); break;
	case Region::GsPriv: vc::gs::GsPrivWrite32(p, v); break;
	default: break;
	}
}

inline void AfterRegisterAccess()
{
	if (SprDmaPending())
		SprDmaRunPending();
	if (!InInterrupt())
		Poll();
}

template <typename T>
T ReadSlow(ps2_cpu* c, uint32_t addr)
{
	(void)c;
	uint32_t p;
	const Region r = Classify(addr, &p);
	T v{};
	switch (r) {
	case Region::Ram: std::memcpy(&v, g_ram + p, sizeof v); return v;
	case Region::Spr: std::memcpy(&v, g_spr + p, sizeof v); return v;
	case Region::Vu: std::memcpy(&v, VuPtr(p), sizeof v); return v;
	case Region::Rom: return 0;
	case Region::Unmapped: LogOnce("read from unmapped address", addr, 0); return 0;
	default: break;
	}
	if constexpr (sizeof(T) == 8) {
		if (r == Region::GsPriv) {
			v = vc::gs::GsPrivRead64(p);
		} else if (r == Region::Hw) {
			uint64_t x = 0;
			if (!vc::gs::HwRead64(p, &x))
				x = HwRead32(p);
			v = x;
		} else {
			v = RegRead32(r, p);
		}
	} else {
		const uint32_t word = RegRead32(r, p & ~3u);
		v = static_cast<T>(word >> ((p & 3) * 8));
	}
	AfterRegisterAccess();
	return v;
}

template <typename T>
void WriteSlow(ps2_cpu* c, uint32_t addr, T v)
{
	(void)c;
	uint32_t p;
	const Region r = Classify(addr, &p);
	switch (r) {
	case Region::Ram: std::memcpy(g_ram + p, &v, sizeof v); return;
	case Region::Spr: std::memcpy(g_spr + p, &v, sizeof v); return;
	case Region::Vu: std::memcpy(VuPtr(p), &v, sizeof v); return;
	case Region::Rom: return;
	case Region::Unmapped: LogOnce("write to unmapped address", addr, static_cast<uint64_t>(v)); return;
	default: break;
	}
	if constexpr (sizeof(T) == 8) {
		if (r == Region::GsPriv)
			vc::gs::GsPrivWrite64(p, v);
		else if (r == Region::Hw) {
			if (!vc::gs::HwWrite64(p, v))
				HwWrite32(p, static_cast<uint32_t>(v));
		} else
			RegWrite32(r, p, static_cast<uint32_t>(v));
	} else if constexpr (sizeof(T) == 4) {
		RegWrite32(r, p, v);
	} else {
		// Byte/halfword register writes: the EE bus merges them into the 32-bit register.
		if (r == Region::Hw && p == 0x1000F180u)
			KPutChar(static_cast<uint8_t>(v));
		else if (r == Region::GsPriv)
			vc::gs::GsPrivWrite32(p & ~3u, (vc::gs::GsPrivRead32(p & ~3u) & ~(((1u << (8 * sizeof(T))) - 1) << ((p & 3) * 8))) |
				(static_cast<uint32_t>(v) << ((p & 3) * 8)));
		else
			RegWrite32(r, p & ~3u, static_cast<uint32_t>(v) << ((p & 3) * 8));
	}
	AfterRegisterAccess();
}

} // namespace

// --- helpers for the kernel ------------------------------------------------------------

uint8_t* EePtr(uint32_t addr, uint32_t size)
{
	uint32_t p;
	const Region r = Classify(addr, &p);
	if (r == Region::Ram && p + size <= PS2_RAM_SIZE)
		return g_ram + p;
	if (r == Region::Spr && p + size <= PS2_SPR_SIZE)
		return g_spr + p;
	return nullptr;
}

uint32_t Load32(uint32_t addr)
{
	uint32_t v = 0;
	if (const uint8_t* p = EePtr(addr, 4))
		std::memcpy(&v, p, 4);
	else
		LogOnce("kernel read outside RAM", addr, 0);
	return v;
}

void Store32(uint32_t addr, uint32_t v)
{
	if (uint8_t* p = EePtr(addr, 4))
		std::memcpy(p, &v, 4);
	else
		LogOnce("kernel write outside RAM", addr, v);
}

void Store64(uint32_t addr, uint64_t v)
{
	if (uint8_t* p = EePtr(addr, 8))
		std::memcpy(p, &v, 8);
	else
		LogOnce("kernel write outside RAM", addr, v);
}

std::string LoadCString(uint32_t addr, size_t max)
{
	std::string s;
	for (size_t i = 0; i < max; i++) {
		const uint8_t* p = EePtr(addr + static_cast<uint32_t>(i), 1);
		if (!p || !*p)
			break;
		s.push_back(static_cast<char>(*p));
	}
	return s;
}

} // namespace vc::kernel

using namespace vc;
using namespace vc::kernel;

extern "C" {

uint8_t ps2_mem_read8_slow(ps2_cpu* c, uint32_t addr) { return ReadSlow<uint8_t>(c, addr); }
uint16_t ps2_mem_read16_slow(ps2_cpu* c, uint32_t addr) { return ReadSlow<uint16_t>(c, addr); }
uint32_t ps2_mem_read32_slow(ps2_cpu* c, uint32_t addr) { return ReadSlow<uint32_t>(c, addr); }
uint64_t ps2_mem_read64_slow(ps2_cpu* c, uint32_t addr) { return ReadSlow<uint64_t>(c, addr); }
void ps2_mem_write8_slow(ps2_cpu* c, uint32_t addr, uint8_t v) { WriteSlow<uint8_t>(c, addr, v); }
void ps2_mem_write16_slow(ps2_cpu* c, uint32_t addr, uint16_t v) { WriteSlow<uint16_t>(c, addr, v); }
void ps2_mem_write32_slow(ps2_cpu* c, uint32_t addr, uint32_t v) { WriteSlow<uint32_t>(c, addr, v); }
void ps2_mem_write64_slow(ps2_cpu* c, uint32_t addr, uint64_t v) { WriteSlow<uint64_t>(c, addr, v); }

void ps2_mem_read128_slow(ps2_cpu* c, uint32_t addr, ps2_u128* out)
{
	uint32_t p;
	const Region r = Classify(addr, &p);
	switch (r) {
	case Region::Ram: std::memcpy(out, g_ram + p, 16); return;
	case Region::Spr: std::memcpy(out, g_spr + p, 16); return;
	case Region::Vu: std::memcpy(out, VuPtr(p), 16); return;
	case Region::Hw:
		if (vc::gs::HwRead128(p, out)) {
			AfterRegisterAccess();
			return;
		}
		break;
	default: break;
	}
	out->ud[0] = ReadSlow<uint64_t>(c, addr);
	out->ud[1] = 0;
}

void ps2_mem_write128_slow(ps2_cpu* c, uint32_t addr, const ps2_u128* v)
{
	uint32_t p;
	const Region r = Classify(addr, &p);
	switch (r) {
	case Region::Ram: std::memcpy(g_ram + p, v, 16); return;
	case Region::Spr: std::memcpy(g_spr + p, v, 16); return;
	case Region::Vu: std::memcpy(VuPtr(p), v, 16); return;
	case Region::Hw:
		if (vc::gs::HwWrite128(p, v)) {
			AfterRegisterAccess();
			return;
		}
		break;
	default: break;
	}
	WriteSlow<uint64_t>(c, addr, v->ud[0]);
}

// --- vc_iop.h KERNEL PROVIDES ----------------------------------------------------------

uint8_t* vc_ee_ptr(uint32_t ee_addr) { return EePtr(ee_addr, 1); }
void vc_ee_raise_dmac(int channel) { vc::gs::RaiseDmac(channel); }
void vc_ee_raise_intc(int bit) { vc::gs::RaiseIntc(bit); }
uint64_t vc_ee_time_us(void) { return NowNs() / 1000; }

} // extern "C"
