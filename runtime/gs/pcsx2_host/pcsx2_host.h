// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal glue between vc::gs (runtime/gs/vc_gs.mm) and the PCSX2 subsystem replacements in
// runtime/gs/pcsx2_host/. Includes PCSX2 headers; not part of the public interface.

#pragma once

#include "vc_gs.h"

#include "common/Pcsx2Defs.h"
#include "common/WindowInfo.h"

#include <optional>

namespace vc::gs::host
{
	// --- window (Host::AcquireRenderWindow) -----------------------------------------------
	void SetWindowInfo(const std::optional<WindowInfo>& wi);
	std::optional<WindowInfo> GetWindowInfo();

	// --- EE-side state (EECore.cpp) ---------------------------------------------------------
	void SetCallbacks(const Callbacks& cb);
	const Callbacks& GetCallbacks();

	// Maps the PS2 memory block (EE RAM/scratchpad/VU memory) and the JIT code area.
	bool AllocateMemory();
	void FreeMemory();
	// Power-on state of EE hardware registers, DMAC, VIF0/1, GIF unit, VU0/1.
	void ResetHardware();

	// Drains pending EE events (DMA, VIF, GIF, VU-finish). Re-entrant calls are ignored.
	void RunEvents();
	// Re-evaluate the INTC/DMAC interrupt lines and call the runtime's callbacks.
	void TestIntcInterrupt();
	void TestDmacInterrupt();

	// Advance the virtual EE cycle counter (used by DMA/VU timing). Called once per vsync.
	void AdvanceCycles(u32 cycles);

	// --- MTGS replacement (SyncMTGS.cpp) ----------------------------------------------------
	// GS register memory seen by the renderer (copied from the EE-side privileged registers
	// at vsync, like PCSX2's MTGS ring buffer does).
	u8* GetRendererRegs();
} // namespace vc::gs::host
