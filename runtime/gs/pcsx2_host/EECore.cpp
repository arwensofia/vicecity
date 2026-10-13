// SPDX-License-Identifier: GPL-3.0-or-later
//
// The slice of PCSX2's EE core that the imported DMAC/VIF/GIF/VU code depends on, rebuilt
// for a statically recompiled game:
//
//  * EE hardware register block (eeHw), PS2 memory (eeMem, VU memory), JIT code area
//  * the EE event scheduler (CPU_INT & co. from pcsx2/R5900.cpp). PCSX2 fires events as the
//    interpreted EE advances its cycle counter; here the recompiled EE has no cycle counter,
//    so RunEvents() fires pending events in due order and fast-forwards a virtual cycle
//    count, i.e. DMA, VIF and VU1 work completes synchronously.
//  * INTC/DMAC interrupt lines, forwarded to the runtime's callbacks
//  * stubs for the DMA channels and subsystems we don't import (IPU, SIF, SPR, timers,
//    SPU2, USB), which hwReset()/dmacWrite32() reference.

#include "pcsx2_host.h"

#include "Common.h"
#include "Counters.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "Hardware.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "MTVU.h"
#include "SPR.h"
#include "SPU2/spu2.h"
#include "Sif.h"
#include "USB/USB.h"
#include "VUmicro.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"

#include "common/Console.h"

#include <sys/mman.h>

#include <mutex>

// ---------------------------------------------------------------------------------------
// Globals normally defined by Memory.cpp / R5900.cpp / Counters.cpp / GS.cpp
// ---------------------------------------------------------------------------------------

alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];
EEVM_MemoryAllocMess* eeMem = nullptr;
u32 Ps2MemSize::ExposedRam = Ps2MemSize::MainRam;
alignas(16) cpuRegistersPack _cpuRegistersPack;
GS_VideoMode gsVideoMode = GS_VideoMode::Uninitialized;
bool FMVstarted = false;
Counter counters[4];

namespace
{
	vc::gs::Callbacks s_callbacks;
	std::optional<WindowInfo> s_window_info;
	std::mutex s_window_mutex;

	u8* s_data_memory = nullptr; // HostMemoryMap "Main" layout up to the end of VU memory
	constexpr size_t kDataSize = HostMemoryMap::VUmemOffset + HostMemoryMap::VUmemSize;
	u8* s_code_memory = nullptr; // HostMemoryMap "Code" layout, RWX (MAP_JIT)
	constexpr size_t kCodeSize = HostMemoryMap::CodeSize;

	bool s_in_run_events = false;
} // namespace

void vc::gs::host::SetCallbacks(const Callbacks& cb) { s_callbacks = cb; }
const vc::gs::Callbacks& vc::gs::host::GetCallbacks() { return s_callbacks; }

void vc::gs::host::SetWindowInfo(const std::optional<WindowInfo>& wi)
{
	std::lock_guard lock(s_window_mutex);
	s_window_info = wi;
}

std::optional<WindowInfo> vc::gs::host::GetWindowInfo()
{
	std::lock_guard lock(s_window_mutex);
	return s_window_info;
}

// ---------------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------------

u8* SysMemory::GetDataPtr(size_t offset)
{
	pxAssert(offset <= kDataSize);
	return s_data_memory + offset;
}

u8* SysMemory::GetCodePtr(size_t offset)
{
	pxAssert(offset <= kCodeSize);
	return s_code_memory + offset;
}

bool vc::gs::host::AllocateMemory()
{
	// Untouched pages cost nothing, so keep PCSX2's offsets (EE RAM incl. the 96 MB devkit
	// expansion it reserves, IOP RAM it never uses here) and let GetDataPtr() stay trivial.
	void* data = mmap(nullptr, kDataSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (data == MAP_FAILED)
	{
		Console.Error("vc::gs: failed to map %zu MB of PS2 memory", kDataSize >> 20);
		return false;
	}
	void* code = mmap(nullptr, kCodeSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
	if (code == MAP_FAILED)
	{
		Console.Error("vc::gs: failed to map %zu MB of JIT memory", kCodeSize >> 20);
		munmap(data, kDataSize);
		return false;
	}
	s_data_memory = static_cast<u8*>(data);
	s_code_memory = static_cast<u8*>(code);

	eeMem = reinterpret_cast<EEVM_MemoryAllocMess*>(SysMemory::GetEEMem());
	vuMemAllocate();
	return true;
}

void vc::gs::host::FreeMemory()
{
	vuMemRelease();
	eeMem = nullptr;
	if (s_code_memory)
		munmap(s_code_memory, kCodeSize);
	if (s_data_memory)
		munmap(s_data_memory, kDataSize);
	s_code_memory = s_data_memory = nullptr;
}

void vc::gs::host::ResetHardware()
{
	std::memset(&cpuRegs, 0, sizeof(cpuRegs));
	std::memset(&fpuRegs, 0, sizeof(fpuRegs));
	std::memset(eeMem->Main, 0, Ps2MemSize::MainRam);
	std::memset(eeMem->Scratch, 0, Ps2MemSize::Scratch);
	std::memset(eeMem->ZeroRead, 0, sizeof(eeMem->ZeroRead));

	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
	vuMemReset();
	CpuVU0->Reset();
	CpuVU1->Reset();
	if constexpr (newVifDynaRec)
	{
		dVifReset(0);
		dVifReset(1);
	}

	hwReset(); // eeHw, D_ENABLE, GS privileged regs (gsReset), GIF unit, VIF0/1, GIF FIFO
}

// ---------------------------------------------------------------------------------------
// Physical memory access used by the GIF/VIF MFIFO and VU0 code paths.
// ---------------------------------------------------------------------------------------

void* vtlb_GetPhyPtr(u32 paddr)
{
	paddr &= 0x1fffffff;
	if (paddr < Ps2MemSize::ExposedRam)
		return &eeMem->Main[paddr];
	if (paddr >= 0x11000000 && paddr < 0x11010000)
	{
		if (paddr >= 0x1100c000)
			return VU1.Mem + (paddr & 0x3fff);
		if (paddr >= 0x11008000)
			return VU1.Micro + (paddr & 0x3fff);
		if (paddr >= 0x11004000)
			return VU0.Mem + (paddr & 0xfff);
		return VU0.Micro + (paddr & 0xfff);
	}
	return nullptr;
}

static u8* EEAddrToHost(u32 mem)
{
	if ((mem & 0xf0000000) == 0x70000000)
		return &eeMem->Scratch[mem & 0x3fff];
	return static_cast<u8*>(vtlb_GetPhyPtr(mem));
}

RETURNS_R128 vtlb_memRead128(u32 mem)
{
	if (const u8* p = EEAddrToHost(mem & ~0xfu))
		return r128_load(p);
	Console.Error("vc::gs: 128-bit read from unmapped address %08x", mem);
	return r128_zero();
}

void TAKES_R128 vtlb_memWrite128(u32 mem, r128 value)
{
	if (u8* p = EEAddrToHost(mem & ~0xfu))
		r128_store(p, value);
	else
		Console.Error("vc::gs: 128-bit write to unmapped address %08x", mem);
}

// ---------------------------------------------------------------------------------------
// EE event scheduler (replaces pcsx2/R5900.cpp's CPU_INT/TESTINT machinery)
// ---------------------------------------------------------------------------------------

void CPU_INT(EE_EventType n, s32 ecycle)
{
	cpuRegs.interrupt |= 1u << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = static_cast<u32>(std::max<s32>(ecycle, 0));
}

void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	if (set)
		cpuRegs.dmastall |= 1u << n;
	else
		cpuRegs.dmastall &= ~(1u << n);
}

int cpuGetCycles(int interrupt)
{
	if (interrupt == VU_MTVU_BUSY && (!THREAD_VU1 || INSTANT_VU1))
		return 1;
	const s64 cycles = static_cast<s64>(cpuRegs.sCycle[interrupt] + cpuRegs.eCycle[interrupt]) -
		static_cast<s64>(cpuRegs.cycle);
	return static_cast<int>(std::max<s64>(1, cycles));
}

void cpuClearInt(uint n)
{
	pxAssume(n < 32);
	cpuRegs.interrupt &= ~(1u << n);
	cpuRegs.dmastall &= ~(1u << n);
}

void cpuSetNextEventDelta(s32)
{
	// Events are polled by RunEvents(); nothing to schedule.
}

void intUpdateCPUCycles()
{
	// Only meaningful for the EE interpreter (VU0 sync); the recompiled EE has no cycle count.
}

namespace
{
	struct EventHandler
	{
		EE_EventType type;
		void (*fn)();
	};

	// Same order as R5900.cpp's _cpuTestInterrupts.
	constexpr EventHandler kHandlers[] = {
		{VU_MTVU_BUSY, MTVUInterrupt},
		{DMAC_VIF1, vif1Interrupt},
		{DMAC_GIF, gifInterrupt},
		{DMAC_VIF0, vif0Interrupt},
		{DMAC_MFIFO_VIF, vifMFIFOInterrupt},
		{DMAC_MFIFO_GIF, gifMFIFOInterrupt},
		{VIF_VU0_FINISH, vif0VUFinish},
		{VIF_VU1_FINISH, vif1VUFinish},
	};

	constexpr u32 HandledMask()
	{
		u32 m = 0;
		for (const EventHandler& h : kHandlers)
			m |= 1u << h.type;
		return m;
	}

	bool DmacRunning()
	{
		return dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1);
	}
} // namespace

void vc::gs::host::RunEvents()
{
	if (s_in_run_events)
		return;
	s_in_run_events = true;

	// A transfer that keeps rescheduling itself without making progress would spin forever;
	// real chains finish in far fewer steps than this.
	constexpr int kMaxSteps = 1 << 22;
	int steps = 0;
	for (; steps < kMaxSteps; steps++)
	{
		if (!DmacRunning())
			break;
		const u32 pending = cpuRegs.interrupt & HandledMask();
		if (!pending)
			break;

		// Fire the earliest-due event, fast-forwarding virtual time to it.
		const EventHandler* next = nullptr;
		u64 next_due = 0;
		for (const EventHandler& h : kHandlers)
		{
			if (!(pending & (1u << h.type)))
				continue;
			const u64 due = cpuRegs.sCycle[h.type] + cpuRegs.eCycle[h.type];
			if (!next || due < next_due)
			{
				next = &h;
				next_due = due;
			}
		}
		if (next_due > cpuRegs.cycle)
			cpuRegs.cycle = next_due;
		cpuClearInt(next->type);
		next->fn();
	}
	if (steps == kMaxSteps)
		Console.Error("vc::gs: DMA/VIF events did not settle (pending %08x)", cpuRegs.interrupt);

	s_in_run_events = false;
}

void vc::gs::host::AdvanceCycles(u32 cycles)
{
	cpuRegs.cycle += cycles;
}

// ---------------------------------------------------------------------------------------
// Interrupt lines
// ---------------------------------------------------------------------------------------

void vc::gs::host::TestIntcInterrupt()
{
	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) && s_callbacks.int0)
		s_callbacks.int0(s_callbacks.user);
}

void vc::gs::host::TestDmacInterrupt()
{
	const bool pending = (psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) || (psHu16(DMAC_STAT) & 0x8000);
	if (pending && DmacRunning() && s_callbacks.int1)
		s_callbacks.int1(s_callbacks.user);
}

void cpuTestINTCInts()
{
	vc::gs::host::TestIntcInterrupt();
}

void cpuTestDMACInts()
{
	vc::gs::host::TestDmacInterrupt();
}

// ---------------------------------------------------------------------------------------
// DMA channels implemented by the runtime (or not at all yet)
// ---------------------------------------------------------------------------------------

namespace
{
	void ForwardDma(int channel)
	{
		if (s_callbacks.dma_start)
		{
			s_callbacks.dma_start(s_callbacks.user, channel);
			return;
		}
		static bool warned[10];
		if (!warned[channel])
		{
			warned[channel] = true;
			Console.Warning("vc::gs: DMA channel %d started but not implemented", channel);
		}
	}
} // namespace

void dmaIPU0() { ForwardDma(3); }
void dmaIPU1() { ForwardDma(4); }
void dmaSIF0() { ForwardDma(5); }
void dmaSIF1() { ForwardDma(6); }
void dmaSIF2() { ForwardDma(7); }
void dmaSPR0() { ForwardDma(8); }
void dmaSPR1() { ForwardDma(9); }
void SPRFROMinterrupt() {}
void SPRTOinterrupt() {}

// Subsystems hwReset() resets.
void rcntInit() {}
u32 rcntRcount(int) { return 0; }
void SPU2::Reset(bool) {}
void sifReset() {}
void ipuReset() {}
void USBreset() {}
