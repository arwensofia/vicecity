// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synchronous replacement for PCSX2's MTGS (pcsx2/MTGS.cpp). PCSX2 runs the GS renderer on
// its own thread fed by a ring buffer; vicecity calls the renderer directly on the thread
// that drives the GIF (the EE thread), so GIF packets are drawn as soon as the GIF unit
// completes them. The renderer keeps its own copy of the privileged registers, refreshed
// at vsync, exactly as the MTGS ring buffer did.
//
// The GS is opened/closed by vc::gs::Init/Shutdown (vc_gs.mm), not through MTGS.

#include "pcsx2_host.h"

#include "Common.h"
#include "GS.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "Gif_Unit.h"
#include "MTGS.h"

#include "common/Console.h"
#include "common/FPControl.h"

#include <cstdlib>

namespace
{
	alignas(32) u8 s_renderer_regs[Ps2MemSize::GSregs];

	// GS code expects the default FP environment; GIF packets can arrive from inside the VU1
	// interpreter (XGKICK), which runs with the VU rounding mode.
	struct GSFPEnv
	{
		FPControlRegisterBackup backup{FPControlRegister::GetDefault()};
	};
} // namespace

u8* vc::gs::host::GetRendererRegs()
{
	return s_renderer_regs;
}

// ---------------------------------------------------------------------------------------
// GIF unit -> GS (declared in Gif_Unit.h, defined by MTGS.cpp upstream)
// ---------------------------------------------------------------------------------------

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	if (!gsPack.size)
		return;
	static const bool trace = std::getenv("VC_GS_TRACE") != nullptr;
	if (trace)
		Console.WriteLn("vc::gs: PATH%d packet, %u qw", path + 1, gsPack.size / 16);
	GSFPEnv fp;
	GSgifTransfer(&gifUnit.gifPath[path].buffer[gsPack.offset], gsPack.size / 16);
}

void Gif_AddBlankGSPacket(u32, GIF_PATH)
{
	// Upstream this keeps the MTGS read pointer in step with buffer realignment. Packets are
	// consumed immediately here, so there is nothing outstanding to account for.
}

void Gif_AddGSPacketMTVU(GS_Packet&, GIF_PATH)
{
	pxFailRel("MTVU is not supported");
}

void Gif_MTGS_Wait(bool)
{
}

// ---------------------------------------------------------------------------------------
// MTGS namespace
// ---------------------------------------------------------------------------------------

void MTGS::WaitGS(bool, bool, bool)
{
}

void MTGS::ResetGS(bool hardware_reset)
{
	if (g_gs_renderer)
	{
		GSFPEnv fp;
		GSreset(hardware_reset);
	}
}

void MTGS::PostVsyncStart(bool registers_written)
{
	if (!g_gs_renderer)
		return;

	// Latch the display registers (PMODE..BGCOLOR, CSR, IMR, SIGLBLID) for the renderer.
	std::memcpy(s_renderer_regs, PS2MEM_GS, 0x100);
	std::memcpy(s_renderer_regs + 0x1000, PS2MEM_GS + 0x1000, 0x10);
	std::memcpy(s_renderer_regs + 0x1010, PS2MEM_GS + 0x1010, 0x10);
	std::memcpy(s_renderer_regs + 0x1080, PS2MEM_GS + 0x1080, 0x10);

	GSFPEnv fp;
	// CSR bit 13 (FIELD) selects the field being displayed.
	GSvsync((GSCSRr & 0x2000) ? 0 : 1, registers_written);
}

void MTGS::InitAndReadFIFO(u8* mem, u32 qwc)
{
	if (!g_gs_renderer)
		return;
	GSFPEnv fp;
	GSInitAndReadFIFO(mem, qwc);
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
	// We are the GS thread.
	func();
}

void MTGS::ApplySettings()
{
	if (!g_gs_renderer)
		return;
	GSFPEnv fp;
	GSUpdateConfig(EmuConfig.GS);
}

void MTGS::ToggleSoftwareRendering()
{
	// Hotkey handler in GS.cpp; vicecity has no hotkeys.
}
