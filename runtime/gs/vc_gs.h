// SPDX-License-Identifier: GPL-3.0-or-later
//
// vc::gs — the runtime's interface to the PS2 graphics hardware: GS (rendered with PCSX2's
// GS/Metal renderer), VU1 (PCSX2 interpreter), VIF1, the GIF unit and the parts of the DMAC
// that feed them. See runtime/gs/README.md for the design and threading rules.
//
// Nothing in this header depends on PCSX2, so a native renderer can replace the backend
// later without touching callers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vc::gs {

enum class Renderer : uint8_t
{
	Metal,    // PCSX2 hardware renderer on the Metal device (default)
	Software, // PCSX2 software rasteriser (JIT), presented through Metal
	Null,     // processes everything, draws nothing (tests, headless)
};

struct Callbacks
{
	// EE interrupt lines. Invoked synchronously, on the calling thread, from inside whichever
	// vc::gs call raised them, whenever INTC_STAT & INTC_MASK (int0) or the DMAC
	// CIS & CIM / BEIS condition (int1) becomes true. The runtime's kernel decides when to
	// actually dispatch the EE handlers.
	void (*int0)(void* user) = nullptr;
	void (*int1)(void* user) = nullptr;
	// A DMA channel this library does not implement (IPU, SIF, SPR) was started. The channel
	// registers are readable through HwRead32. `channel` is 0..9 (D0..D9).
	void (*dma_start)(void* user, int channel) = nullptr;
	void* user = nullptr;
};

struct Config
{
	// NSView* to present into. The Metal backend installs its own CAMetalLayer on it (on
	// the main thread). nullptr runs without a window (Null renderer, or offscreen tests).
	void* ns_view = nullptr;
	uint32_t surface_width = 640;  // drawable size in pixels
	uint32_t surface_height = 448;
	float surface_scale = 1.0f;    // backing scale factor
	Renderer renderer = Renderer::Metal;
	float upscale = 1.0f;          // HW renderer internal resolution multiplier
	bool vsync = true;             // present with display sync (FIFO)
	bool log_to_stdout = true;     // route PCSX2 console output to stdout
	Callbacks callbacks;
};

// PS2 memory owned by this library, valid from Init() to Shutdown(). The runtime should hand
// these to the recompiled code (ps2_cpu::ram/spr/vu0_mem) so DMA, VIF and VU see the same
// bytes as the EE.
struct Memory
{
	uint8_t* ee_ram;     // 32 MB, physical 0x00000000
	uint8_t* scratchpad; // 16 KB, 0x70000000
	uint8_t* vu0_micro;  //  4 KB, 0x11000000
	uint8_t* vu0_mem;    //  4 KB, 0x11004000
	uint8_t* vu1_micro;  // 16 KB, 0x11008000
	uint8_t* vu1_mem;    // 16 KB, 0x1100C000
};

bool Init(const Config& config);
void Shutdown();
const Memory& GetMemory();

// ---------------------------------------------------------------------------------------
// Register level. For code that pokes the hardware itself (RenderWare's sky2 driver,
// libgraph/libdma internals). Addresses are EE physical addresses; the Hw* functions return
// false if the address is not one this library owns (timers, IPU, SIF, ... are the
// runtime's), leaving *out untouched.
//
// Owned EE register ranges:
//   0x10003000-0x100037FF  GIF registers          0x10003800-0x10003FFF  VIF0/VIF1 registers
//   0x10004000-0x10006FFF  VIF0/VIF1/GIF FIFOs    0x10008000-0x1000EFFF  DMAC channels + D_CTRL..
//   0x1000F000/0x1000F010  INTC_STAT/INTC_MASK    0x1000F520/0x1000F590  D_ENABLER/D_ENABLEW
// Writes that start DMA run it to completion before returning (see RunEvents()).
// ---------------------------------------------------------------------------------------
bool HwRead32(uint32_t addr, uint32_t* out);
bool HwWrite32(uint32_t addr, uint32_t value);
bool HwRead64(uint32_t addr, uint64_t* out);
bool HwWrite64(uint32_t addr, uint64_t value);
bool HwRead128(uint32_t addr, void* out16);        // FIFO reads (VIF1 FIFO = GS download)
bool HwWrite128(uint32_t addr, const void* in16);

// GS privileged registers, 0x12000000-0x12001FFF (PMODE, SMODE1/2, SRFSH, SYNCH1/2, SYNCV,
// DISPFB1/2, DISPLAY1/2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, CSR, IMR, BUSDIR, SIGLBLID).
// CSR/IMR writes have their side effects (FINISH/SIGNAL acknowledge, interrupt masking).
uint64_t GsPrivRead64(uint32_t addr);
void GsPrivWrite64(uint32_t addr, uint64_t value);
uint32_t GsPrivRead32(uint32_t addr);
void GsPrivWrite32(uint32_t addr, uint32_t value);

// ---------------------------------------------------------------------------------------
// HLE paths. Data pointers are host pointers; `qwc` counts 16-byte quadwords. All of these
// are synchronous: the data has been consumed (and any VU1 program it started has run)
// when they return.
// ---------------------------------------------------------------------------------------

// PATH3: GIF packets (GIFtag + data) straight into the GIF unit, as GIF DMA would.
void SubmitPath3(const void* data, size_t qwc);

// VIF1 command stream, as VIF1 DMA would deliver it (DMA tags already stripped):
// UNPACK/STCYCL/STMASK/... write VU1 data memory, MPG uploads microcode, MSCAL/MSCNT/MSCALF
// run VU1 (whose XGKICKs are PATH1), DIRECT/DIRECTHL are PATH2.
void SubmitPath2(const void* data, size_t qwc);

// Direct VU1 access (equivalent to the MPG/UNPACK VIF commands, or EE writes to VU memory).
// Addresses are byte offsets into micro memory / data memory.
void UploadVU1Micro(uint32_t byte_addr, const void* code, size_t bytes);
void UploadVU1Data(uint32_t byte_addr, const void* data, size_t bytes);
// Start a microprogram at `pc` (in 64-bit instruction units, as MSCAL / CMSAR1) and run it
// to its E-bit. XGKICKs go to the GS as PATH1.
void StartVU1(uint32_t pc);
bool VU1Busy();

// Interrupt sources owned by the runtime (timers, VBLANK, SIF/IPU/SPR DMA completion): set
// INTC_STAT bit `bit` (0..14) / the DMAC D_STAT.CIS bit of `channel` (0..9, 13..15) and
// re-evaluate the interrupt lines, calling int0/int1 if the source is unmasked.
void RaiseIntc(int bit);
void RaiseDmac(int channel);

// VU0 micro mode (vcallms/vcallmsr). `vf` = 32 x 16-byte VF registers, `acc` = 16 bytes,
// `vi` = 32 COP2 registers in ps2_abi.h order (VI0-15, Status, MAC, Clip, -, R, I, Q, ...).
// Copies VF/ACC/VI[0..22] into the VU0 interpreter, runs the microprogram at `pc` (in 64-bit
// instruction units; -1 = continue at TPC) to its E-bit, and copies the registers back
// (vi[26] receives TPC). Synchronous.
void ExecuteVU0(int32_t pc, void* vf, void* acc, uint32_t* vi);
// Reset VU0 (FBRST.RST0) / VU1 (FBRST.RST1).
void ResetVU0();
void ResetVU1();

// Drain pending DMA/VIF/GIF/VU events. Called internally after every Hw* write and HLE
// submission; exposed for runtimes that change DMAC state behind our back.
void RunEvents();

// ---------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------

// HLE of the kernel's SetGsCrt syscall (2): sets the video mode and programs SMODE1, SMODE2
// and SYNCV the way the BIOS does. mode: 2 = NTSC, 3 = PAL (0/1 aliases), as in the syscall.
void SetGsCrt(bool interlaced, uint32_t mode, bool frame_mode);

// Start of vertical blank: toggles CSR.FIELD, raises the GS VSINT interrupt (per IMR),
// latches the display registers, flushes pending drawing and presents the frame.
// The runtime raises INTC VBLANK_S/E itself (they belong to the timer/INTC model).
void VSync();

// The host window changed size.
void ResizeSurface(uint32_t width, uint32_t height, float scale);

// ---------------------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------------------

// GS local memory -> host, as a GS->EE transfer (TRXDIR=1) with the given BITBLTBUF/TRXPOS/
// TRXREG register values. Synchronises the HW renderer's texture cache first. Leaves those
// GS transfer registers set to the given values (games program them before every transfer).
void ReadLocalMemory(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, void* out, size_t qwc);

// The last presented display image (after PCRTC merge), RGBA8, `width`*`height` pixels.
bool CaptureDisplay(uint32_t* width, uint32_t* height, std::vector<uint32_t>* rgba);

} // namespace vc::gs
