/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ps2_recomp.h -- what the recompiler (vc_game library) provides to the runtime
 * beyond ps2_abi.h: symbol table, trace hooks + ring buffer, default indirect
 * dispatch with an unknown-target hook, crash helpers.
 *
 * Tables are generated into recomp_out/ (ps2_names.c); the functions are in
 * recomp/support/ps2_recomp_support.c, compiled into vc_game. Everything marked
 * WEAK has a default implementation that the runtime may replace by defining a
 * strong symbol of the same name.
 */
#ifndef PS2_RECOMP_H
#define PS2_RECOMP_H

#include "ps2_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Symbols (recomp_out/ps2_names.c)                                          */
/* ------------------------------------------------------------------------ */

typedef struct ps2_recomp_symbol {
	uint32_t addr;      /* entry address (same order as ps2_recomp_functions[]) */
	uint32_t size;      /* bytes of reached code from addr (0 if unknown) */
	const char *name;   /* tools/library_names.csv / Ghidra name, or NULL */
	const char *module; /* out/modules.csv module or library namespace, or NULL */
} ps2_recomp_symbol;

extern const ps2_recomp_symbol ps2_recomp_symbols[]; /* sorted by addr */
extern const uint32_t ps2_recomp_symbol_count;

/* Function containing EE address `addr` (entry <= addr < entry + size), or NULL. */
const ps2_recomp_symbol *ps2_recomp_find_symbol(uint32_t addr);

/* "sceMcInit+0x24 [libmc]", "ps2f_00123456+0x8 [game]" or "0x01234567 (?)".
 * Returns buf. Async-signal-safe (no allocation, no stdio). */
char *ps2_recomp_format_addr(uint32_t addr, char *buf, size_t size);

/* EE entry address of the recompiled function containing host code address
 * `host_pc` (from a signal context or backtrace), or 0. Uses the ps2f_XXXXXXXX
 * symbol from dladdr(), so it needs an unstripped binary; follows HLE overrides
 * only if they keep the ps2f_ name. Not async-signal-safe (dladdr). */
uint32_t ps2_recomp_host_to_ee(const void *host_pc);

/* ------------------------------------------------------------------------ */
/* Tracing. Compile-time switches for the generated code (CMake options      */
/* VC_GAME_TRACE / VC_GAME_TRACK_PC, see ps2_ops.h):                         */
/*   PS2_TRACE   bit 0 -> ps2_trace_fn at every function entry               */
/*               bit 1 -> ps2_trace_bb at every basic block                  */
/*   PS2_TRACK_PC 1 -> c->pc = current block, 2 -> also before each ld/st     */
/* With both 0 (default) the generated code contains no trace code at all.   */
/* ------------------------------------------------------------------------ */

enum ps2_trace_kind {
	PS2_TRACE_KIND_FN = 0,   /* function entry */
	PS2_TRACE_KIND_BB = 1,   /* basic block */
	PS2_TRACE_KIND_MISS = 2, /* unknown indirect target (addr = target) */
	PS2_TRACE_KIND_USER = 3, /* runtime-defined (ps2_trace_record) */
};

typedef struct ps2_trace_entry {
	uint32_t addr_kind; /* EE address | kind (addresses are 4-aligned) */
	uint32_t sp;        /* EE $sp at the time (call depth / thread hint) */
} ps2_trace_entry;

#define PS2_TRACE_RING_SIZE 8192u /* power of two */

typedef struct ps2_trace_ring {
	uint32_t head; /* total records written (index = head & (SIZE-1)) */
	ps2_trace_entry e[PS2_TRACE_RING_SIZE];
} ps2_trace_ring;

extern ps2_trace_ring ps2_trace_buffer;

/* WEAK. Called by generated code (when enabled). Defaults record into
 * ps2_trace_buffer. */
void ps2_trace_fn(ps2_cpu *c, uint32_t entry);
void ps2_trace_bb(ps2_cpu *c, uint32_t addr);

/* Append a record (any thread; lock-free, entries may tear under contention). */
void ps2_trace_record(ps2_cpu *c, uint32_t addr, unsigned kind);

/* Write the last `max` records (oldest first), symbolized, to `fd`.
 * Async-signal-safe: usable from a SIGSEGV/SIGBUS handler. */
void ps2_trace_dump(int fd, unsigned max);

/* ------------------------------------------------------------------------ */
/* Loop back-edge hook (CMake VC_GAME_BACKEDGE -> -DPS2_BACKEDGE_HOOK=1)       */
/* Every backward branch inside a recompiled function executes               */
/*     if (--ps2_backedge_budget < 0) ps2_backedge(c, loop_head);            */
/* The runtime overrides ps2_backedge to poll for pending interrupts (INTC/  */
/* DMAC handlers, vblank, timers, alarms, SIF completions) and must reload   */
/* ps2_backedge_budget before returning. Setting the budget to 0 (EE thread) */
/* forces a poll at the next back edge. The handler may run EE code          */
/* (interrupt handlers) on *c's thread; it must preserve all of *c that the  */
/* interrupted code can observe (save/restore the context, like an           */
/* exception), since the loop resumes right after it returns.               */
/* ------------------------------------------------------------------------ */

#define PS2_BACKEDGE_DEFAULT_BUDGET 1024

extern int32_t ps2_backedge_budget;
/* WEAK. Default: reload the budget and return. */
void ps2_backedge(ps2_cpu *c, uint32_t loop_head);

/* ------------------------------------------------------------------------ */
/* Indirect dispatch                                                         */
/* ------------------------------------------------------------------------ */

enum ps2_dispatch_kind {
	PS2_DISPATCH_CALL = 0, /* jalr: ps2_call_indirect */
	PS2_DISPATCH_JUMP = 1, /* jr rs / unknown branch target / jump-table default */
};

/* WEAK defaults for the ps2_abi.h dispatch entry points: binary search of
 * ps2_recomp_functions[]; on a miss they call ps2_dispatch_unknown and return.
 * A runtime that defines its own ps2_call_indirect/ps2_jump_indirect should
 * call ps2_dispatch_unknown for misses too. */
void ps2_call_indirect(ps2_cpu *c, uint32_t pc, uint32_t target);
void ps2_jump_indirect(ps2_cpu *c, uint32_t pc, uint32_t target);

/* WEAK. Unknown indirect target. `site` is the EE address of the jr/jalr (or
 * of the branch), `target` the EE address it wanted; the caller function is
 * ps2_recomp_find_symbol(site). Default: record a MISS trace entry, log one
 * line to stderr (first 32 distinct sites, each once), dump the last 64 trace
 * records on the first miss, then return. Returning means: a call is skipped
 * (v0/v1 unchanged), a jump returns from the current recompiled function. */
void ps2_dispatch_unknown(ps2_cpu *c, uint32_t site, uint32_t target, int kind);

/* Number of ps2_dispatch_unknown calls so far (default implementation). */
extern uint32_t ps2_dispatch_unknown_count;

#ifdef __cplusplus
}
#endif

#endif /* PS2_RECOMP_H */
