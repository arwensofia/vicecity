/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ps2_abi.h -- the contract between recompiled EE code and the runtime.
 *
 * Owner: recompiler (recomp/). The runtime implements every function declared
 * "RUNTIME PROVIDES" below; recompiled code (recomp_out/ps2_funcs_NNN.c) and the
 * recompiler's helper header (recomp/include/ps2_ops.h) only use what is in
 * this file. Any change here must bump PS2_ABI_VERSION and be noted in the
 * changelog at the bottom.
 *
 * ---------------------------------------------------------------------------
 * Execution model
 * ---------------------------------------------------------------------------
 *  - Every EE function found by analysis becomes one C function
 *        void ps2f_XXXXXXXX(ps2_cpu *c);       (XXXXXXXX = entry, lower hex)
 *    All EE state lives in *c; the host C stack only holds return linkage.
 *  - `jal` / `bal` / `bgezal` to a known entry is a direct C call. The callee
 *    returns when it executes `jr $ra`; execution then continues after the
 *    caller's delay slot. $ra is still written (= pc + 8) for code that reads it.
 *  - `jalr`, and `jr rs` (rs != $ra) that is not a recognised jump table, go
 *    through ps2_call_indirect / ps2_jump_indirect with the EE target address.
 *    The runtime resolves it via ps2_recomp_functions[] (sorted by address,
 *    emitted by the recompiler into recomp_out/ps2_dispatch.c).
 *  - Branches/jumps that leave the function become tail calls: a direct call to
 *    ps2f_target followed by `return`, or ps2_jump_indirect + `return` when the
 *    target is not a known entry.
 *  - Overriding (HLE): every recompiled function is emitted as a *weak*
 *    definition. The runtime replaces an EE function (e.g. a Sony SDK routine)
 *    by defining a strong symbol with the same name, e.g.
 *        void ps2f_00123456(ps2_cpu *c) { ... }
 *    Direct calls and the dispatch table both resolve to the override at link
 *    time. An override must follow the EE calling convention on *c (args in
 *    a0-a3/t0-t3 = r4-r11, f12-f19; results in v0/v1 = r2/r3, f0).
 *
 * ---------------------------------------------------------------------------
 * Register semantics (R5900)
 * ---------------------------------------------------------------------------
 *  - GPRs are 128 bits. Ordinary MIPS-III instructions read/write only the low
 *    64 bits (ud[0]) and leave ud[1] untouched; lq/sq/lqc2/sqc2, qmfc2/qmtc2 and
 *    MMI instructions use all 128 bits. r[0] is always zero; recompiled code
 *    never writes it, the runtime must not either.
 *  - hi/lo: ud[0] is HI/LO (pipeline 0), ud[1] is HI1/LO1 (pipeline 1, used by
 *    mult1/div1/madd1/... and the MMI multiply-accumulate instructions).
 *  - sa: shift amount register in BYTES (0..15), as set by mtsab/mtsah and
 *    consumed by qfsrv. mfsa/mtsa copy it verbatim (only used for save/restore).
 *  - FPU (COP1): 32 single-precision registers held as raw bits, plus the
 *    accumulator used by adda/madda/... . fcr31 bit 23 is the condition flag C.
 *    fcr0 reads as 0x2E00. Arithmetic follows the EE: no NaN/Inf/denormals,
 *    results that overflow clamp to +/-FLT_MAX, denormal inputs and results
 *    flush to signed zero.
 *  - VU0 (COP2, macro mode): vf[32] (vf[0] is the constant (0,0,0,1) and is never
 *    written), vacc, and vi[32] laid out exactly as COP2 control registers:
 *        vi[0..15]  VI integer regs (16-bit values, zero-extended; vi[0] == 0)
 *        vi[16] Status   vi[17] MAC   vi[18] Clipping   vi[20] R   vi[21] I
 *        vi[22] Q        vi[26] TPC   vi[27] CMSAR0     vi[28] FBRST
 *        vi[29] VPU-STAT vi[31] CMSAR1
 *    Recompiled code accesses vi[0..22] directly. cfc2/ctc2 of 26..31 go through
 *    ps2_vu0_ctrl_read / ps2_vu0_ctrl_write so the runtime can react (start VU0
 *    or VU1 microprograms, reset, report busy state). The runtime's VU0 micro
 *    interpreter must use these same fields so macro and micro mode share state.
 *    Note: the numbering matches PCSX2's VU0.VI[], but PCSX2 pads each REG_VI to
 *    16 bytes, so the layouts are not memory-compatible. A runtime that drives
 *    PCSX2's VU0 interpreter copies vf[], vacc and vi[] into VURegs before the
 *    microprogram and back afterwards (inside ps2_vu0_callms).
 *
 * ---------------------------------------------------------------------------
 * Host floating-point environment (REQUIRED)
 * ---------------------------------------------------------------------------
 *  The EE FPU and VU0 round toward zero. Before running any recompiled code on
 *  a thread, the runtime must set the AArch64 FPCR to:
 *      RMode = 0b11 (round toward zero), FZ = 1 (flush denormals to zero),
 *      exception traps disabled.                       see ps2_set_host_fpcr()
 *  Recompiled code is built with -ffp-contract=off (no fused multiply-add):
 *  madd.s/vmadd round the product before the add, like the hardware.
 *
 * ---------------------------------------------------------------------------
 * Memory
 * ---------------------------------------------------------------------------
 *  c->ram   : 32 MB EE main RAM, physical address 0 (host buffer, 16-byte aligned)
 *  c->spr   : 16 KB scratchpad, EE address 0x70000000
 *  c->vu0_mem: 4 KB VU0 data memory (EE address 0x11004000; also used by
 *              vilwr/viswr/vlqi/vsqi with VI addresses in 16-byte units)
 *  The inline ps2_read* / ps2_write* helpers below take the fast path for
 *  addresses below 32 MB and for the scratchpad. Everything else calls the
 *  *_slow functions, which the runtime must implement for at least:
 *    0x20000000-0x21FFFFFF  uncached RAM mirror       (-> ram)
 *    0x30000000-0x31FFFFFF  uncached-accelerated mirror (-> ram)
 *    0x80000000-0x81FFFFFF  KSEG0, 0xA0000000-0xA1FFFFFF KSEG1 (-> ram)
 *    0x10000000-0x1000FFFF  EE hardware registers (timers, DMAC, INTC, VIF, GIF, IPU)
 *    0x11000000-0x1100FFFF  VU0/VU1 micro and data memory
 *    0x12000000-0x12001FFF  GS privileged registers
 *    0x1FC00000-0x1FFFFFFF  BIOS ROM
 *  Address arithmetic wraps at 32 bits. lq/sq/lqc2/sqc2 addresses are already
 *  16-byte aligned (low 4 bits cleared) when the helpers are called; ld/sd 8-,
 *  lw/sw 4-, lh/sh 2-byte aligned (the EE would raise an address error
 *  otherwise; the fast path tolerates misalignment).
 *  Host is little-endian like the EE, so values are stored natively.
 *
 * ---------------------------------------------------------------------------
 * Threads
 * ---------------------------------------------------------------------------
 *  Each EE thread needs its own ps2_cpu (or a swapped copy) and its own host
 *  stack (e.g. a coroutine/fiber): recompiled code keeps return linkage on the
 *  host stack, so an EE context switch inside a syscall must switch host stacks
 *  too. ram/spr/vu0_mem pointers are shared by all contexts.
 *  The runtime (runtime/kernel) gives each EE thread its own ps2_cpu and fiber;
 *  COP2 (vf/vacc/vi) is one piece of hardware, copied to the incoming thread's
 *  ps2_cpu on every switch. Interrupt handlers run on a separate ps2_cpu with
 *  their own EE stack, so a hook never modifies the interrupted *c except
 *  through a thread switch, which preserves it.
 *
 * ---------------------------------------------------------------------------
 * Interrupt delivery points
 * ---------------------------------------------------------------------------
 *  Recompiled code is never preempted asynchronously. The runtime delivers
 *  interrupts (INTC/DMAC handlers, alarms, vblank) and may switch threads only
 *  inside hooks: ps2_syscall, ps2_mem_*_slow for hardware registers, ei / mtc0
 *  Status, ps2_vu0_ctrl_write(CMSAR1), and the back-edge hook
 *  ps2_backedge(c, loop_head) that recompiled code calls on backward branches
 *  when ps2_backedge_budget runs out (declared in recomp/include/ps2_ops.h;
 *  weak default in vc_game, overridden by the runtime, which reloads the budget
 *  to PS2_BACKEDGE_DEFAULT_BUDGET). This is what lets loops that spin on RAM
 *  written by interrupt handlers terminate.
 *  The i* syscalls (negative numbers) may be called from thread context (e.g.
 *  an RPC end function run synchronously by an HLE override); they then defer
 *  any reschedule to the next delivery point, as on the EE.
 *
 * ---------------------------------------------------------------------------
 * Diagnostics fields
 * ---------------------------------------------------------------------------
 *  c->pc is written by every hook call site and, when the recompiled code is
 *  built with PS2_TRACK_PC >= 1 (VC_GAME_TRACK_PC), by trace points at basic
 *  blocks (and loads/stores with 2). It is for diagnostics only.
 */
#ifndef PS2_ABI_H
#define PS2_ABI_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_ABI_VERSION 2

#if defined(__GNUC__) || defined(__clang__)
#define PS2_LIKELY(x) __builtin_expect(!!(x), 1)
#define PS2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PS2_ALIGN16 __attribute__((aligned(16)))
#else
#define PS2_LIKELY(x) (x)
#define PS2_UNLIKELY(x) (x)
#define PS2_ALIGN16
#endif

/* ------------------------------------------------------------------------ */
/* Register types                                                            */
/* ------------------------------------------------------------------------ */

/* 128-bit register. Element 0 is the least significant (little-endian). */
typedef union PS2_ALIGN16 ps2_u128 {
	uint64_t ud[2];
	int64_t sd[2];
	uint32_t uw[4];
	int32_t sw[4];
	uint16_t uh[8];
	int16_t sh[8];
	uint8_t ub[16];
	int8_t sb[16];
	float f[4]; /* VU0 x,y,z,w */
} ps2_u128;

/* 32-bit FPU register, raw bits. */
typedef union ps2_f32 {
	float f;
	uint32_t u;
	int32_t s;
} ps2_f32;

/* COP2 control register numbers (index into ps2_cpu.vi). */
enum {
	PS2_VI_STATUS = 16,
	PS2_VI_MAC = 17,
	PS2_VI_CLIP = 18,
	PS2_VI_R = 20,
	PS2_VI_I = 21,
	PS2_VI_Q = 22,
	PS2_VI_TPC = 26,
	PS2_VI_CMSAR0 = 27,
	PS2_VI_FBRST = 28,
	PS2_VI_VPU_STAT = 29,
	PS2_VI_CMSAR1 = 31,
};

/* FCR31 bits. */
#define PS2_FCR31_C 0x00800000u  /* condition */
#define PS2_FCR31_I 0x00020000u  /* invalid (sqrt/rsqrt of negative, 0/0) */
#define PS2_FCR31_D 0x00010000u  /* divide by zero */
#define PS2_FCR31_O 0x00008000u
#define PS2_FCR31_U 0x00004000u
#define PS2_FCR31_SI 0x00000040u /* sticky I */
#define PS2_FCR31_SD 0x00000020u /* sticky D */
#define PS2_FCR31_SO 0x00000010u
#define PS2_FCR31_SU 0x00000008u

typedef struct ps2_cpu {
	/* EE core */
	ps2_u128 r[32]; /* GPRs; r[0] == 0 */
	ps2_u128 hi;    /* ud[0] = HI, ud[1] = HI1 */
	ps2_u128 lo;    /* ud[0] = LO, ud[1] = LO1 */
	uint32_t sa;    /* shift amount, bytes */
	uint32_t pc;    /* EE pc of the last hook call (diagnostics only) */

	/* COP1 */
	ps2_f32 f[32];
	ps2_f32 facc;
	uint32_t fcr31;
	uint32_t fcr0; /* 0x2E00 */

	/* COP2 / VU0 */
	ps2_u128 vf[32]; /* vf[0] = (0,0,0,1) */
	ps2_u128 vacc;
	uint32_t vi[32]; /* see PS2_VI_* */

	/* Memory (owned by the runtime, shared between EE thread contexts) */
	uint8_t *ram;     /* 32 MB */
	uint8_t *spr;     /* 16 KB */
	uint8_t *vu0_mem; /* 4 KB */

	void *user; /* runtime-private (thread object, etc.) */
} ps2_cpu;

/* ------------------------------------------------------------------------ */
/* Memory                                                                    */
/* ------------------------------------------------------------------------ */

#define PS2_RAM_SIZE 0x02000000u
#define PS2_SPR_BASE 0x70000000u
#define PS2_SPR_SIZE 0x00004000u
#define PS2_VU0_MEM_SIZE 0x00001000u

/* RUNTIME PROVIDES: everything outside main RAM [0, 32 MB) and scratchpad. */
uint8_t ps2_mem_read8_slow(ps2_cpu *c, uint32_t addr);
uint16_t ps2_mem_read16_slow(ps2_cpu *c, uint32_t addr);
uint32_t ps2_mem_read32_slow(ps2_cpu *c, uint32_t addr);
uint64_t ps2_mem_read64_slow(ps2_cpu *c, uint32_t addr);
void ps2_mem_read128_slow(ps2_cpu *c, uint32_t addr, ps2_u128 *out);
void ps2_mem_write8_slow(ps2_cpu *c, uint32_t addr, uint8_t v);
void ps2_mem_write16_slow(ps2_cpu *c, uint32_t addr, uint16_t v);
void ps2_mem_write32_slow(ps2_cpu *c, uint32_t addr, uint32_t v);
void ps2_mem_write64_slow(ps2_cpu *c, uint32_t addr, uint64_t v);
void ps2_mem_write128_slow(ps2_cpu *c, uint32_t addr, const ps2_u128 *v);

/* Host pointer for a fast-path address, or NULL. */
static inline uint8_t *ps2_fast_ptr(ps2_cpu *c, uint32_t addr)
{
	if (PS2_LIKELY(addr < PS2_RAM_SIZE))
		return c->ram + addr;
	if ((addr & ~(PS2_SPR_SIZE - 1)) == PS2_SPR_BASE)
		return c->spr + (addr & (PS2_SPR_SIZE - 1));
	return NULL;
}

#define PS2_DEFINE_RW(bits, type)                                                \
	static inline type ps2_read##bits(ps2_cpu *c, uint32_t addr)                 \
	{                                                                            \
		uint8_t *p = ps2_fast_ptr(c, addr);                                      \
		if (PS2_LIKELY(p != NULL)) {                                             \
			type v;                                                              \
			memcpy(&v, p, sizeof v);                                             \
			return v;                                                            \
		}                                                                        \
		return ps2_mem_read##bits##_slow(c, addr);                               \
	}                                                                            \
	static inline void ps2_write##bits(ps2_cpu *c, uint32_t addr, type v)        \
	{                                                                            \
		uint8_t *p = ps2_fast_ptr(c, addr);                                      \
		if (PS2_LIKELY(p != NULL)) {                                             \
			memcpy(p, &v, sizeof v);                                             \
			return;                                                              \
		}                                                                        \
		ps2_mem_write##bits##_slow(c, addr, v);                                  \
	}

PS2_DEFINE_RW(8, uint8_t)
PS2_DEFINE_RW(16, uint16_t)
PS2_DEFINE_RW(32, uint32_t)
PS2_DEFINE_RW(64, uint64_t)
#undef PS2_DEFINE_RW

/* addr must be 16-byte aligned. */
static inline void ps2_read128(ps2_cpu *c, uint32_t addr, ps2_u128 *out)
{
	uint8_t *p = ps2_fast_ptr(c, addr);
	if (PS2_LIKELY(p != NULL)) {
		memcpy(out, p, 16);
		return;
	}
	ps2_mem_read128_slow(c, addr, out);
}

static inline void ps2_write128(ps2_cpu *c, uint32_t addr, const ps2_u128 *v)
{
	uint8_t *p = ps2_fast_ptr(c, addr);
	if (PS2_LIKELY(p != NULL)) {
		memcpy(p, v, 16);
		return;
	}
	ps2_mem_write128_slow(c, addr, v);
}

/* ------------------------------------------------------------------------ */
/* Hooks (RUNTIME PROVIDES)                                                  */
/* `pc` is the EE address of the instruction; `insn` its raw encoding.       */
/* ------------------------------------------------------------------------ */

/* `syscall`: the call number is in v1 (r[3]); args in a0..; result to v0.
 * code = the instruction's 20-bit code field (normally 0). */
void ps2_syscall(ps2_cpu *c, uint32_t pc, uint32_t code);
/* `break`, and trap instructions (teq/tne/tge/... incl. immediate forms) whose
 * condition holds. */
void ps2_break(ps2_cpu *c, uint32_t pc, uint32_t code);
void ps2_trap(ps2_cpu *c, uint32_t pc, uint32_t code);

/* COP0. reg = rd field (Count = 9, Status = 12, Cause = 13, EPC = 14, ...);
 * sel = low 6 bits of the instruction (selects BPC/IAB/... for reg 24 and
 * PCCR/PCR0/PCR1 for reg 25). */
uint32_t ps2_cop0_read(ps2_cpu *c, uint32_t pc, uint32_t reg, uint32_t sel);
void ps2_cop0_write(ps2_cpu *c, uint32_t pc, uint32_t reg, uint32_t sel, uint32_t value);
/* ei, di, eret, tlbr, tlbwi, tlbwr, tlbp (and any other COP0 C0 op). */
void ps2_cop0_op(ps2_cpu *c, uint32_t pc, uint32_t insn);
/* Condition signal tested by bc0f/bc0t (CPCOND0, DMA busy). */
int ps2_cop0_cond(ps2_cpu *c);
/* Condition signal tested by bc2f/bc2t (CPCOND2, VU0 busy). */
int ps2_cop2_cond(ps2_cpu *c);
/* `cache` instruction (op = rt field, addr = effective address). Usually a no-op. */
void ps2_cache(ps2_cpu *c, uint32_t pc, uint32_t op, uint32_t addr);

/* VU0. vcallms/vcallmsr: run the VU0 microprogram at byte address `addr` in VU0
 * micro memory (vcallms imm15 * 8, or CMSAR0 * 8). May run to completion
 * synchronously; then ps2_vu0_wait (called for interlocked .i transfers) can
 * be a no-op. */
void ps2_vu0_callms(ps2_cpu *c, uint32_t pc, uint32_t addr);
void ps2_vu0_wait(ps2_cpu *c);
/* cfc2/ctc2 for reg 26..31 (TPC, CMSAR0, FBRST, VPU-STAT, CMSAR1). */
uint32_t ps2_vu0_ctrl_read(ps2_cpu *c, uint32_t reg);
void ps2_vu0_ctrl_write(ps2_cpu *c, uint32_t reg, uint32_t value);

/* Instruction the recompiler could not translate (reserved/invalid encoding,
 * a branch in a delay slot, ...). The runtime should log and abort or skip. */
void ps2_unimplemented(ps2_cpu *c, uint32_t pc, uint32_t insn);

/* ------------------------------------------------------------------------ */
/* Indirect control flow                                                     */
/* ------------------------------------------------------------------------ */

typedef void (*ps2_func_t)(ps2_cpu *c);

typedef struct ps2_func_entry {
	uint32_t addr;
	ps2_func_t fn;
} ps2_func_entry;

/* RECOMPILER PROVIDES (recomp_out/ps2_dispatch.c): all function entries,
 * sorted by ascending addr. */
extern const ps2_func_entry ps2_recomp_functions[];
extern const uint32_t ps2_recomp_function_count;

/* RECOMPILER PROVIDES: entries of the executable's .ctors table, in table
 * order. The game's own startup code walks .ctors itself (via jalr ->
 * ps2_call_indirect), so the runtime only needs this if it bypasses that code. */
extern const uint32_t ps2_recomp_ctors[];
extern const uint32_t ps2_recomp_ctor_count;

/* RUNTIME PROVIDES.
 * ps2_call_indirect: jalr. Call the function at `target` and return when it
 *   returns. $ra has already been set by the caller.
 * ps2_jump_indirect: jr rs (rs != ra) or a branch/jump leaving the function to
 *   an address that is not a known entry. Semantically a tail call: after it
 *   returns, the recompiled function returns too.
 * Unknown targets: log (pc, target) and fail gracefully.
 * vc_game (recomp/support/ps2_recomp_support.c, ps2_recomp.h) has weak default
 * implementations (binary search, misses go to ps2_dispatch_unknown and
 * return); runtime/kernel/dispatch.cpp overrides both and stops with a
 * diagnostic (trace dump, registers, thread list) on a miss. */
void ps2_call_indirect(ps2_cpu *c, uint32_t pc, uint32_t target);
void ps2_jump_indirect(ps2_cpu *c, uint32_t pc, uint32_t target);

/* Reference lookup over ps2_recomp_functions (binary search). Returns NULL for
 * an address that is not a function entry. */
static inline ps2_func_t ps2_lookup_function(const ps2_func_entry *table, uint32_t count,
                                             uint32_t addr)
{
	uint32_t lo = 0, hi = count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (table[mid].addr < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo < count && table[lo].addr == addr) ? table[lo].fn : NULL;
}

/* ------------------------------------------------------------------------ */
/* Host FP environment                                                       */
/* ------------------------------------------------------------------------ */

/* Set FPCR as required above (round toward zero, flush-to-zero). Returns the
 * previous FPCR so callers can restore it. */
static inline uint64_t ps2_set_host_fpcr(void)
{
#if defined(__aarch64__)
	uint64_t old, v;
	__asm__ volatile("mrs %0, fpcr" : "=r"(old));
	v = old;
	v &= ~((uint64_t)0x9F00u);          /* trap enables off */
	v |= (uint64_t)3u << 22;            /* RMode = RZ */
	v |= (uint64_t)1u << 24;            /* FZ */
	__asm__ volatile("msr fpcr, %0" : : "r"(v));
	return old;
#else
	return 0; /* other hosts: set rounding mode with fesetround(FE_TOWARDZERO) */
#endif
}

static inline void ps2_restore_host_fpcr(uint64_t old)
{
#if defined(__aarch64__)
	__asm__ volatile("msr fpcr, %0" : : "r"(old));
#else
	(void)old;
#endif
}

/* Initialise architectural reset values that recompiled code relies on. */
static inline void ps2_cpu_reset_regs(ps2_cpu *c)
{
	uint8_t *ram = c->ram, *spr = c->spr, *vu0 = c->vu0_mem;
	void *user = c->user;
	memset(c, 0, sizeof *c);
	c->ram = ram;
	c->spr = spr;
	c->vu0_mem = vu0;
	c->user = user;
	c->fcr0 = 0x2E00;
	c->vf[0].f[3] = 1.0f;
	c->vi[PS2_VI_R] = 0x3F800000u;
}

/*
 * Changelog
 *  1 (2026-10-13) initial version (incl. ps2_recomp_ctors).
 *  2 (2026-10-13) documentation only, no layout/signature change: interrupt
 *    delivery points incl. the back-edge hook, thread/COP2 model, i* syscalls
 *    from thread context, weak dispatch defaults in vc_game, c->pc written by
 *    trace points. See runtime/include/vc_iop.h for the IOP boundary.
 */

#ifdef __cplusplus
}
#endif

#endif /* PS2_ABI_H */
