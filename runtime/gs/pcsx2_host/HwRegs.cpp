// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE hardware register dispatch for the registers vc::gs owns: GIF, VIF0/1, the
// VIF/GIF FIFOs, the DMAC and the INTC status/mask. Adapted from PCSX2's pcsx2/HwWrite.cpp and
// pcsx2/HwRead.cpp (pages 0x03-0x0f), dropping the timer, IPU, SIF, SIO and PS1-mode paths
// that belong to other parts of the runtime.

#include "pcsx2_host.h"

#include "Common.h"
#include "GS.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "Hardware.h"
#include "ps2/HwInternal.h"

namespace
{
	constexpr bool IsOwned32(u32 mem)
	{
		const u32 page = (mem >> 12) & 0xf;
		if ((mem & 0xffff0000) != 0x10000000)
			return false;
		if (page >= 0x3 && page <= 0x0e)
			return page != 0x07; // 0x7000 is the IPU FIFO
		return mem == INTC_STAT || mem == INTC_MASK || mem == DMAC_ENABLER || mem == DMAC_ENABLEW;
	}

	u128 ReadFifo(u32 page)
	{
		u128 out = u128::From32(0);
		if (page == 0x05)
			ReadFIFO_VIF1(&out);
		// VIF0 and GIF FIFOs are write-only; reads return 0.
		return out;
	}

	void WriteFifo(u32 page, const u128& value)
	{
		alignas(16) const u128 v = value;
		switch (page)
		{
			case 0x04: WriteFIFO_VIF0(&v); break;
			case 0x05: WriteFIFO_VIF1(&v); break;
			case 0x06: WriteFIFO_GIF(&v); break;
		}
	}
} // namespace

bool vc::gs::HwRead32(uint32_t mem, uint32_t* out)
{
	mem &= ~3u;
	if (!IsOwned32(mem))
		return false;

	const u32 page = (mem >> 12) & 0xf;
	switch (page)
	{
		case 0x03:
			if (mem >= EEMemoryMap::VIF1_Start)
				*out = vifRead32<1>(mem);
			else if (mem >= EEMemoryMap::VIF0_Start)
				*out = vifRead32<0>(mem);
			else
				*out = dmacRead32<0x03>(mem);
			return true;

		case 0x04:
		case 0x05:
		case 0x06:
			*out = ReadFifo(page)._u32[(mem >> 2) & 3];
			return true;

		default:
			*out = psHu32(mem);
			return true;
	}
}

bool vc::gs::HwWrite32(uint32_t mem, uint32_t value)
{
	mem &= ~3u;
	if (!IsOwned32(mem))
		return false;

	const u32 page = (mem >> 12) & 0xf;
	bool writeback = true;
	switch (page)
	{
		case 0x03:
			if (mem >= EEMemoryMap::VIF1_Start)
				writeback = vifWrite32<1>(mem, value);
			else if (mem >= EEMemoryMap::VIF0_Start)
				writeback = vifWrite32<0>(mem, value);
			else if (mem == GIF_CTRL)
			{
				gifRegs.ctrl.write(value & 9);
				if (gifRegs.ctrl.RST)
					gifUnit.Reset(true);
				gifRegs.stat.PSE = gifRegs.ctrl.PSE;
				writeback = false;
			}
			else if (mem == GIF_MODE)
			{
				gifRegs.mode.write(value);
				// Kick the GIF if the PATH3 mask comes off with work pending.
				if (gifRegs.stat.M3R == 1 && gifRegs.mode.M3R == 0 && (gifch.chcr.STR || gif_fifo.fifoSize))
					CPU_INT(DMAC_GIF, 8);
				gifRegs.stat.M3R = gifRegs.mode.M3R;
				gifRegs.stat.IMT = gifRegs.mode.IMT;
				writeback = false;
			}
			break;

		case 0x04:
		case 0x05:
		case 0x06:
		{
			// Sub-128-bit FIFO writes: zero-filled 128-bit write (same assumption as PCSX2).
			u128 v = u128::From32(0);
			v._u32[(mem >> 2) & 3] = value;
			WriteFifo(page, v);
			writeback = false;
			break;
		}

		case 0x08: writeback = dmacWrite32<0x08>(mem, value); break;
		case 0x09: writeback = dmacWrite32<0x09>(mem, value); break;
		case 0x0a: writeback = dmacWrite32<0x0a>(mem, value); break;
		case 0x0b: writeback = dmacWrite32<0x0b>(mem, value); break;
		case 0x0c: writeback = dmacWrite32<0x0c>(mem, value); break;
		case 0x0d: writeback = dmacWrite32<0x0d>(mem, value); break;
		case 0x0e: writeback = dmacWrite32<0x0e>(mem, value); break;

		case 0x0f:
			if (mem == INTC_STAT)
			{
				psHu32(INTC_STAT) &= ~value;
				writeback = false;
			}
			else if (mem == INTC_MASK)
			{
				psHu32(INTC_MASK) ^= static_cast<u16>(value);
				vc::gs::host::TestIntcInterrupt();
				writeback = false;
			}
			else if (mem == DMAC_ENABLER || mem == DMAC_ENABLEW)
			{
				// D_ENABLER is read-only; D_ENABLEW is handled by the DMAC.
				writeback = (mem == DMAC_ENABLEW) ? dmacWrite32<0x0f>(mem, value) : false;
			}
			break;
	}

	if (writeback)
		psHu32(mem) = value;

	vc::gs::host::RunEvents();
	return true;
}

bool vc::gs::HwRead64(uint32_t mem, uint64_t* out)
{
	mem &= ~7u;
	const u32 page = (mem >> 12) & 0xf;
	if ((mem & 0xffff0000) == 0x10000000 && page >= 0x04 && page <= 0x06)
	{
		*out = ReadFifo(page)._u64[(mem >> 3) & 1];
		return true;
	}
	u32 lo;
	if (!HwRead32(mem, &lo))
		return false;
	*out = lo;
	return true;
}

bool vc::gs::HwWrite64(uint32_t mem, uint64_t value)
{
	mem &= ~7u;
	const u32 page = (mem >> 12) & 0xf;
	if ((mem & 0xffff0000) == 0x10000000 && page >= 0x04 && page <= 0x06)
	{
		u128 v = u128::From32(0);
		v._u64[(mem >> 3) & 1] = value;
		WriteFifo(page, v);
		vc::gs::host::RunEvents();
		return true;
	}
	// Everything else only latches the low 32 bits (HwWrite.cpp: "disregard everything
	// except the lower 32 bits").
	return HwWrite32(mem, static_cast<u32>(value));
}

bool vc::gs::HwRead128(uint32_t mem, void* out16)
{
	mem &= ~15u;
	const u32 page = (mem >> 12) & 0xf;
	if ((mem & 0xffff0000) == 0x10000000 && page >= 0x04 && page <= 0x06)
	{
		const u128 v = ReadFifo(page);
		std::memcpy(out16, &v, 16);
		vc::gs::host::RunEvents();
		return true;
	}
	u64 lo;
	if (!HwRead64(mem, &lo))
		return false;
	const u64 q[2] = {lo, 0};
	std::memcpy(out16, q, 16);
	return true;
}

bool vc::gs::HwWrite128(uint32_t mem, const void* in16)
{
	mem &= ~15u;
	const u32 page = (mem >> 12) & 0xf;
	if ((mem & 0xffff0000) == 0x10000000 && page >= 0x04 && page <= 0x06)
	{
		u128 v;
		std::memcpy(&v, in16, 16);
		WriteFifo(page, v);
		vc::gs::host::RunEvents();
		return true;
	}
	u64 lo;
	std::memcpy(&lo, in16, 8);
	return HwWrite64(mem, lo);
}

// ---------------------------------------------------------------------------------------
// GS privileged registers (pcsx2/GS.cpp does the work)
// ---------------------------------------------------------------------------------------

uint64_t vc::gs::GsPrivRead64(uint32_t addr)
{
	return gsRead64(addr & ~7u);
}

uint32_t vc::gs::GsPrivRead32(uint32_t addr)
{
	return gsRead32(addr & ~3u);
}

void vc::gs::GsPrivWrite64(uint32_t addr, uint64_t value)
{
	addr &= ~7u;
	if (addr & 0x1000)
		gsWrite64_page_01(addr, value);
	else
		gsWrite64_page_00(addr, value);
	vc::gs::host::RunEvents();
}

void vc::gs::GsPrivWrite32(uint32_t addr, uint32_t value)
{
	gsWrite32(addr & ~3u, value);
	vc::gs::host::RunEvents();
}
