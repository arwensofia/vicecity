// SPDX-License-Identifier: GPL-3.0-or-later
//
// vc::gs implementation on top of PCSX2's GS/VU/VIF/GIF code (third_party/pcsx2).

#include "vc_gs.h"

#include "pcsx2_host/pcsx2_host.h"

#include "Common.h"
#include "Config.h"
#include "GS.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "Hardware.h"
#include "VUmicro.h"
#include "Vif.h"
#include "Vif_Dynarec.h"
#include "ps2/HwInternal.h"

#include "common/Console.h"
#include "common/FPControl.h"

#include <algorithm>
#include <cstring>

namespace
{
	bool s_initialized = false;
	vc::gs::Memory s_memory = {};

	// The GS renderer expects the default FP environment; recompiled EE code runs with
	// round-toward-zero (ps2_abi.h), so every entry point that may reach the GS switches.
	struct DefaultFP
	{
		FPControlRegisterBackup backup{FPControlRegister::GetDefault()};
	};

	GSRendererType ToPcsx2(vc::gs::Renderer r)
	{
		switch (r)
		{
			case vc::gs::Renderer::Software: return GSRendererType::SW;
			case vc::gs::Renderer::Null: return GSRendererType::Null;
			case vc::gs::Renderer::Metal:
			default: return GSRendererType::Metal;
		}
	}

	std::optional<WindowInfo> MakeWindowInfo(void* view, uint32_t w, uint32_t h, float scale)
	{
		WindowInfo wi;
		wi.type = view ? WindowInfo::Type::MacOS : WindowInfo::Type::Surfaceless;
		wi.window_handle = view;
		wi.surface_width = w;
		wi.surface_height = h;
		wi.surface_scale = scale;
		return wi;
	}
} // namespace

bool vc::gs::Init(const Config& config)
{
	if (s_initialized)
		return true;

	if (config.log_to_stdout)
		Log::SetConsoleOutputLevel(LOGLEVEL_INFO);

	host::SetCallbacks(config.callbacks);
	if (!host::AllocateMemory())
		return false;

	VifUnpackSSE_Init();

	// Emulation settings: PCSX2 defaults, VU1 runs to completion when started, no MTVU.
	EmuConfig = Pcsx2Config();
	EmuConfig.Speedhacks.vu1Instant = true;
	EmuConfig.Speedhacks.vuThread = false;
	Pcsx2Config::GSOptions& gs = EmuConfig.GS;
	gs.Renderer = ToPcsx2(config.renderer);
	gs.UpscaleMultiplier = std::max(1.0f, config.upscale);
	gs.VsyncEnable = config.vsync;
	gs.OsdShowGPU = false;
	gs.OsdShowGPUStats = false;

	host::ResetHardware();

	host::SetWindowInfo(MakeWindowInfo(config.ns_view, config.surface_width, config.surface_height,
		config.surface_scale));

	{
		DefaultFP fp;
		const GSVSyncMode vsync = config.vsync ? GSVSyncMode::FIFO : GSVSyncMode::Disabled;
		std::memcpy(host::GetRendererRegs(), PS2MEM_GS, Ps2MemSize::GSregs);
		if (!GSopen(EmuConfig.GS, gs.Renderer, host::GetRendererRegs(), vsync, false))
		{
			Console.Error("vc::gs: failed to open the GS renderer");
			host::FreeMemory();
			return false;
		}
		GSreset(true);
	}

	s_memory.ee_ram = eeMem->Main;
	s_memory.scratchpad = eeMem->Scratch;
	s_memory.vu0_micro = VU0.Micro;
	s_memory.vu0_mem = VU0.Mem;
	s_memory.vu1_micro = VU1.Micro;
	s_memory.vu1_mem = VU1.Mem;

	// Power-on DMAC state the kernel would leave behind: DMAC enabled.
	psHu32(DMAC_CTRL) = 1;

	s_initialized = true;
	return true;
}

void vc::gs::Shutdown()
{
	if (!s_initialized)
		return;
	{
		DefaultFP fp;
		GSclose();
	}
	if constexpr (newVifDynaRec)
	{
		dVifRelease(1);
		dVifRelease(0);
	}
	host::SetWindowInfo(std::nullopt);
	host::FreeMemory();
	s_memory = {};
	s_initialized = false;
}

const vc::gs::Memory& vc::gs::GetMemory()
{
	return s_memory;
}

// ---------------------------------------------------------------------------------------
// HLE paths
// ---------------------------------------------------------------------------------------

void vc::gs::SubmitPath3(const void* data, size_t qwc)
{
	const u8* p = static_cast<const u8*>(data);
	size_t bytes = qwc * 16;
	int stalls = 0;
	while (bytes)
	{
		// Same entry point as GIF DMA: the GIF unit arbitrates against PATH1/PATH2 and honours
		// the PATH3 mask/IMT. The data is copied into the path buffer before use.
		const u32 chunk = static_cast<u32>(std::min<size_t>(bytes, 0x10000 * 16));
		const u32 done = gifUnit.TransferGSPacketData(GIF_TRANS_DMA, const_cast<u8*>(p), chunk);
		if (done == 0)
		{
			// PATH3 not available right now (another path mid-packet, or masked): let pending
			// DMA/VU work finish, then retry; give up if nothing changes.
			host::RunEvents();
			if (++stalls > 4)
			{
				Console.Error("vc::gs: PATH3 stalled with %zu bytes left (APATH=%u M3R=%u)", bytes,
					gifRegs.stat.APATH, gifRegs.stat.M3R);
				return;
			}
			continue;
		}
		stalls = 0;
		p += done;
		bytes -= done;
	}
	host::RunEvents();
}

void vc::gs::SubmitPath2(const void* data, size_t qwc)
{
	// Through the VIF1 FIFO, which VIF1 treats exactly like DMA data (minus the DMA tags).
	const u8* p = static_cast<const u8*>(data);
	for (size_t i = 0; i < qwc; i++, p += 16)
	{
		alignas(16) u128 q;
		std::memcpy(&q, p, 16);
		WriteFIFO_VIF1(&q);
		if (vif1.vifstalled.enabled || vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			host::RunEvents();
			if (vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS | VIF1_STAT_INT))
			{
				// Stopped by an interrupt bit / STOP: the EE must acknowledge via VIF1_FBRST.
				Console.Warning("vc::gs: VIF1 stalled (STAT=%08x) with %zu qw left", vif1Regs.stat._u32,
					qwc - i - 1);
				return;
			}
		}
	}
	host::RunEvents();
}

void vc::gs::UploadVU1Micro(uint32_t byte_addr, const void* code, size_t bytes)
{
	byte_addr &= VU1_PROGMASK;
	bytes = std::min<size_t>(bytes, VU1_PROGSIZE - byte_addr);
	vu1Finish(false);
	std::memcpy(VU1.Micro + byte_addr, code, bytes);
	CpuVU1->Clear(byte_addr, static_cast<u32>(bytes));
}

void vc::gs::UploadVU1Data(uint32_t byte_addr, const void* data, size_t bytes)
{
	byte_addr &= VU1_MEMMASK;
	bytes = std::min<size_t>(bytes, VU1_MEMSIZE - byte_addr);
	vu1Finish(false);
	std::memcpy(VU1.Mem + byte_addr, data, bytes);
}

void vc::gs::StartVU1(uint32_t pc)
{
	// As ctc2 to CMSAR1 (VU0.cpp CTC2): wait for the current program, then start.
	vu1Finish(true);
	vu1ExecMicro(pc);
	host::RunEvents();
}

bool vc::gs::VU1Busy()
{
	return (VU0.VI[REG_VPU_STAT].UL & 0x100) != 0;
}

void vc::gs::RunEvents()
{
	host::RunEvents();
}

// ---------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------

void vc::gs::SetGsCrt(bool interlaced, uint32_t mode, bool frame_mode)
{
	// What the kernel's SetGsCrt programs into the CRTC. SMODE1 from the field values documented
	// in PCSX2's GSRegs.h (NTSC/PAL); SYNCV: VS/VDP/VBPE/VBP/VFPE/VFP for 480/576 lines.
	const bool pal = (mode == 1 || mode == 3);
	gsVideoMode = pal ? GS_VideoMode::PAL : GS_VideoMode::NTSC;
	gsWrite64_page_00(GS_SMODE1, pal ? 0x0000000740816504ull : 0x0000000740814504ull);
	gsWrite64_page_00(GS_SMODE2, (interlaced ? 1u : 0u) | (frame_mode ? 2u : 0u));
	gsWrite64_page_00(GS_SYNCV, pal ? 0x00A9000502101401ull : 0x00C7800601A01801ull);
}

void vc::gs::VSync()
{
	DefaultFP fp;

	// About one NTSC field of EE cycles, so DMA/VU timestamps keep moving.
	host::AdvanceCycles(294912000 / 60);

	// Counters.cpp VSyncStart: hand the frame to the GS (flush + present).
	gsPostVsyncStart();

	// Counters.cpp GSVSync: swap the field and raise VSINT.
	if (!GSSMODE1reg.SINT)
	{
		const bool progressive = !(*(u32*)PS2GS_BASE(GS_SYNCV) & 0x1) || !(*(u32*)PS2GS_BASE(GS_SMODE1) & 0x6000);
		if (progressive)
			CSRreg.SetField();
		else
			CSRreg.SwapField();
		if (!CSRreg.VSINT)
		{
			CSRreg.VSINT = true;
			if (!GSIMR.VSMSK)
				gsIrq();
		}
	}
	host::RunEvents();
}

void vc::gs::ResizeSurface(uint32_t width, uint32_t height, float scale)
{
	DefaultFP fp;
	auto wi = host::GetWindowInfo();
	if (wi)
	{
		wi->surface_width = width;
		wi->surface_height = height;
		wi->surface_scale = scale;
		host::SetWindowInfo(wi);
	}
	if (g_gs_renderer)
		GSResizeDisplayWindow(width, height, scale);
}

// ---------------------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------------------

void vc::gs::ReadLocalMemory(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, void* out, size_t qwc)
{
	if (!g_gs_renderer)
		return;
	DefaultFP fp;
	// A GS->host transfer, as the EE would start it: program the transfer registers through
	// an A+D packet with TRXDIR = 1, then drain the FIFO (what VIF1 FIFO reads do). Everything
	// the GIF has accepted so far has already been handed to the GS, so ordering holds.
	alignas(16) const u64 packet[] = {
		4 | (1ull << 15) | (1ull << 60), 0xe, // GIFtag: NLOOP 4, EOP, PACKED, A+D
		bitbltbuf, 0x50,                      // BITBLTBUF
		trxpos, 0x51,                         // TRXPOS
		trxreg, 0x52,                         // TRXREG
		1, 0x53,                              // TRXDIR: local -> host
	};
	GSgifTransfer(reinterpret_cast<const u8*>(packet), sizeof(packet) / 16);
	GSInitAndReadFIFO(static_cast<u8*>(out), static_cast<u32>(qwc));
}

bool vc::gs::CaptureDisplay(uint32_t* width, uint32_t* height, std::vector<uint32_t>* rgba)
{
	DefaultFP fp;
	u32 w = 0, h = 0;
	if (!GSSaveSnapshotToMemory(0, 0, false, false, &w, &h, rgba))
		return false;
	*width = w;
	*height = h;
	return true;
}
