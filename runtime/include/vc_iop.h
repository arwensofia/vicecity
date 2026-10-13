/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * vc_iop.h -- boundary between the EE side of the runtime (runtime/kernel, runtime/mem,
 * runtime/app) and the IOP/SIF emulation (runtime/iop).
 *
 * Owner of the declarations: runtime/kernel. Implementations:
 *   - "IOP PROVIDES" functions are implemented by runtime/iop. runtime/kernel/iop_weak.c
 *     holds weak no-op defaults so the EE side links before runtime/iop exists; a strong
 *     definition in runtime/iop replaces each one at link time (link runtime/iop as object
 *     files or an OBJECT library, not a static archive, so the strong symbols win).
 *   - "KERNEL PROVIDES" functions are implemented by runtime/kernel and may be called by the
 *     IOP side.
 *
 * Threading: everything here runs on the EE host thread (the thread that runs recompiled
 * code). The IOP side may run its own host threads (e.g. CD reads, audio), but must only call
 * the KERNEL PROVIDES functions from the EE host thread -- typically from vc_iop_poll(),
 * which the kernel calls regularly (every vblank, while all EE threads are blocked, and on
 * SIF register/syscall activity).
 *
 * Memory: EE addresses passed here are raw EE virtual addresses (KSEG0/KSEG1/uncached mirrors
 * included); use vc_ee_ptr() to translate. Main RAM is shared with the recompiled code, so
 * writes are immediately visible to the EE.
 *
 * HLE: the libraries that sit on top of SIF (sceSifInitRpc/BindRpc/CallRpc/CheckStatRpc,
 * sceSifInitCmd/SendCmd, fileio sceOpen/..., sceSifAllocIopHeap, libcdvd, libmc, libpad,
 * libsdr) can be replaced wholesale by defining ps2f_XXXXXXXX overrides (see ps2_abi.h).
 * The register/DMA/syscall entry points below are the fallback for whatever still talks to
 * SIF at the kernel level.
 *
 * runtime/iop notes (documented by runtime/iop, 2026-10-13):
 *   - Build target: `vc_iop`, an OBJECT library in runtime/iop/CMakeLists.txt
 *     (add_subdirectory(iop); target_link_libraries(<app> PRIVATE vc_iop)). It also exports
 *     runtime/iop/vc_pad.h (keyboard / GameController / injected pad input, rumble readback).
 *   - ps2f_ overrides defined by runtime/iop (runtime/kernel must not define these):
 *       SIF cmd:  0x1157a0 sceSifInitCmd, 0x116010 sceSifExitCmd, 0x115a88/0x115ab8
 *                 sceSifAdd/RemoveCmdHandler, 0x115c18/0x115c58 (i)sceSifSendCmd
 *       SIF RPC:  0x115e70 sceSifInitRpc, 0x1164e0 sceSifBindRpc, 0x1166b0 sceSifCallRpc,
 *                 0x1168a0 sceSifCheckStatRpc
 *       IOP:      0x11b668 sceSifRebootIop, 0x11b4c8 sceSifResetIop, 0x11b630 sceSifSyncIop,
 *                 0x11b608 (SMFLG SIFINIT check)
 *       fileio:   0x117438 _sceFsInit, 0x1176c8 sceOpen, 0x117950 sceClose,
 *                 0x117ad0 sceLseek, 0x117d08 sceRead, 0x117f78 sceWrite
 *     The libkernl syscall stubs (sceSifSetDma/DmaStat/SetReg/GetReg/...) are NOT overridden;
 *     the kernel forwards those syscalls to the vc_iop_sif_* functions below (libpad
 *     actuators and the FMV player's sceSdTransToIOP use sceSifSetDma).
 *   - sceSifCallRpc completes synchronously; its end function runs before it returns, via
 *     ps2_call_indirect on the calling EE thread with all EE registers saved/restored (an
 *     interrupt-context callback: libcdvd/libmc end functions call iSignalSema). So
 *     ps2_call_indirect must be re-entrant from inside an override, and the i* syscalls
 *     must work when invoked from thread context.
 *   - vc_iop_vblank() writes the pad DMA frames; vc_iop_poll() currently has nothing to do.
 */
#ifndef VC_IOP_H
#define VC_IOP_H

#include <stddef.h>
#include <stdint.h>

#include "ps2_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VC_IOP_API_VERSION 1

typedef struct vc_iop_config {
	const char *iso_path; /* --iso <path>, or NULL */
	const char *data_dir; /* ~/Library/Application Support/vicecity (memory cards, settings) */
	uint8_t *ee_ram;      /* 32 MB EE main RAM (same buffer as ps2_cpu::ram) */
} vc_iop_config;

/* ------------------------------------------------------------------------------------- */
/* IOP PROVIDES                                                                           */
/* ------------------------------------------------------------------------------------- */

/* Called once by the app after vc::gs::Init and before the ELF is loaded / the EE starts.
 * Returns 0 on success. */
int vc_iop_init(const vc_iop_config *cfg);
void vc_iop_shutdown(void);

/* Read a whole file from the disc image. `path` uses the PS2 form
 * ("cdrom0:\\SLUS_205.52;1"; device prefix and ";1" optional, case-insensitive).
 * On success returns 0 and sets *data (malloc'd, caller frees) and *size. Used by the app to
 * load the executable when no --elf is given. Returns -1 if unavailable. */
int vc_iop_read_disc_file(const char *path, void **data, size_t *size);

/* Called regularly on the EE host thread (see above). Deliver completed asynchronous work
 * here: write results into EE RAM, then raise interrupts with vc_ee_raise_dmac/_intc. */
void vc_iop_poll(void);
/* Called once per vblank (59.94 Hz) after the EE's VBLANK_S interrupt was raised. */
void vc_iop_vblank(void);

/* SIF registers, EE physical 0x1000F200-0x1000F2FF (MSCOM 0x1000F200, SMCOM 0x1000F210,
 * MSFLG 0x1000F220, SMFLG 0x1000F230, CTRL 0x1000F240, BD6 0x1000F260). 32-bit accesses;
 * narrower/wider accesses are widened/narrowed by the caller. */
uint32_t vc_iop_sif_read32(uint32_t addr);
void vc_iop_sif_write32(uint32_t addr, uint32_t value);

/* EE kernel SIF syscalls. `c` is the calling EE context (for memory access via vc_ee_ptr or
 * ps2_read/ps2_write).
 *   sceSifSetDma (119 / -119): `desc` = EE address of `count` sceSifDmaData
 *       { u32 data (EE src); u32 addr (IOP dest); u32 size (bytes); u32 mode; } (16 bytes each).
 *       Returns a non-zero transfer id, or 0 if the queue is full.
 *   sceSifDmaStat (118 / -118): < 0 when the transfer `id` has completed, >= 0 while pending.
 *   sceSifSetDChain (120 / -120): re-arm SIF0 (IOP->EE) channel in chain mode.
 *   sceSifSetReg (121) / sceSifGetReg (122): SIF "registers" (0x1-0x4 are MSCOM/SMCOM/MSFLG/
 *       SMFLG; 0x80000000+n are the software registers used by sceSifInitCmd/InitRpc).
 *   sceSifStopDma (107). */
uint32_t vc_iop_sif_set_dma(ps2_cpu *c, uint32_t desc, uint32_t count);
int32_t vc_iop_sif_dma_stat(ps2_cpu *c, uint32_t id);
void vc_iop_sif_set_dchain(ps2_cpu *c);
uint32_t vc_iop_sif_set_reg(ps2_cpu *c, uint32_t reg, uint32_t value); /* returns old value */
uint32_t vc_iop_sif_get_reg(ps2_cpu *c, uint32_t reg);
void vc_iop_sif_stop_dma(ps2_cpu *c);

/* The EE started DMAC channel 5 (SIF0), 6 (SIF1) or 7 (SIF2) by writing Dn_CHCR.STR.
 * Channel registers can be read/written with vc::gs::HwRead32/HwWrite32 (0x1000C000,
 * 0x1000C400, 0x1000C800). On completion clear STR and call vc_ee_raise_dmac(channel). */
void vc_iop_dma_start(int channel);

/* ------------------------------------------------------------------------------------- */
/* KERNEL PROVIDES                                                                        */
/* ------------------------------------------------------------------------------------- */

/* Host pointer for an EE address in main RAM (any mirror) or scratchpad; NULL otherwise. */
uint8_t *vc_ee_ptr(uint32_t ee_addr);

/* Set the DMAC channel's CIS bit in D_STAT (channel 0..9) / an INTC_STAT bit (0..14, e.g.
 * 1 = SBUS) and let the EE kernel dispatch the registered handlers at its next interrupt
 * point (immediately if called from vc_iop_poll while the EE is idle). */
void vc_ee_raise_dmac(int channel);
void vc_ee_raise_intc(int bit);

/* Microseconds since the EE started (monotonic host time). */
uint64_t vc_ee_time_us(void);

/* printf-style logging into the runtime log (tag e.g. "iop", "cdvd"). */
void vc_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* VC_IOP_H */
