// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal implementation of the ps2_abi.h runtime hooks for unit tests.
#include "test_env.h"

#include <cstring>

HookLog g_log;

namespace {
// Slow path: RAM mirrors (0x20000000, 0x30000000, KSEG0/1) and a fake I/O
// register window at 0x10000000 that reads back (addr ^ 0x5A5A5A5A).
uint8_t *mirror(ps2_cpu *c, uint32_t a)
{
	uint32_t top = a >> 28;
	if ((top == 2 || top == 3 || top == 8 || top == 0xA) && (a & 0x0FFFFFFFu) < PS2_RAM_SIZE) return c->ram + (a & 0x0FFFFFFFu);
	return nullptr;
}
uint64_t io_read(uint32_t a) { return static_cast<uint64_t>(a ^ 0x5A5A5A5Au) * 0x100000001ull; }
void io_write(uint32_t a, uint64_t v)
{
	g_log.io_writes++;
	g_log.io_last_write_addr = a;
	g_log.io_last_write_val = v;
}
} // namespace

extern "C" {

#define SLOW_RW(bits, type)                                                         \
	type ps2_mem_read##bits##_slow(ps2_cpu *c, uint32_t a)                          \
	{                                                                               \
		if (uint8_t *p = mirror(c, a)) {                                            \
			type v;                                                                 \
			std::memcpy(&v, p, sizeof v);                                           \
			return v;                                                               \
		}                                                                           \
		return static_cast<type>(io_read(a));                                       \
	}                                                                               \
	void ps2_mem_write##bits##_slow(ps2_cpu *c, uint32_t a, type v)                 \
	{                                                                               \
		if (uint8_t *p = mirror(c, a)) {                                            \
			std::memcpy(p, &v, sizeof v);                                           \
			return;                                                                 \
		}                                                                           \
		io_write(a, v);                                                             \
	}
SLOW_RW(8, uint8_t)
SLOW_RW(16, uint16_t)
SLOW_RW(32, uint32_t)
SLOW_RW(64, uint64_t)

void ps2_mem_read128_slow(ps2_cpu *c, uint32_t a, ps2_u128 *out)
{
	if (uint8_t *p = mirror(c, a)) {
		std::memcpy(out, p, 16);
		return;
	}
	out->ud[0] = io_read(a);
	out->ud[1] = io_read(a + 8);
}
void ps2_mem_write128_slow(ps2_cpu *c, uint32_t a, const ps2_u128 *v)
{
	if (uint8_t *p = mirror(c, a)) {
		std::memcpy(p, v, 16);
		return;
	}
	io_write(a, v->ud[0]);
}

void ps2_syscall(ps2_cpu *c, uint32_t pc, uint32_t code)
{
	g_log.syscalls++;
	g_log.last_pc = pc;
	g_log.last_code = code;
	g_log.syscall_v1 = c->r[3].uw[0];
	c->r[2].ud[0] = 0x5C; // pretend result
}
void ps2_break(ps2_cpu *, uint32_t pc, uint32_t code)
{
	g_log.breaks++;
	g_log.last_pc = pc;
	g_log.last_code = code;
}
void ps2_trap(ps2_cpu *, uint32_t pc, uint32_t code)
{
	g_log.traps++;
	g_log.last_pc = pc;
	g_log.last_code = code;
}
uint32_t ps2_cop0_read(ps2_cpu *, uint32_t pc, uint32_t reg, uint32_t sel)
{
	g_log.last_pc = pc;
	return 0x80000000u | (reg << 8) | sel;
}
void ps2_cop0_write(ps2_cpu *, uint32_t pc, uint32_t reg, uint32_t sel, uint32_t value)
{
	g_log.last_pc = pc;
	g_log.last_reg = reg | (sel << 16);
	g_log.last_value = value;
}
void ps2_cop0_op(ps2_cpu *, uint32_t pc, uint32_t insn)
{
	g_log.cop0_ops++;
	g_log.last_pc = pc;
	g_log.last_insn = insn;
}
int ps2_cop0_cond(ps2_cpu *) { return g_log.cop0_cond; }
int ps2_cop2_cond(ps2_cpu *) { return g_log.cop2_cond; }
void ps2_cache(ps2_cpu *, uint32_t pc, uint32_t op, uint32_t addr)
{
	g_log.caches++;
	g_log.last_pc = pc;
	g_log.last_code = op;
	g_log.last_addr = addr;
}
void ps2_vu0_callms(ps2_cpu *, uint32_t pc, uint32_t addr)
{
	g_log.callms++;
	g_log.last_pc = pc;
	g_log.last_addr = addr;
}
void ps2_vu0_wait(ps2_cpu *) { g_log.waits++; }
uint32_t ps2_vu0_ctrl_read(ps2_cpu *, uint32_t reg) { return g_log.vu_ctrl[reg & 31]; }
void ps2_vu0_ctrl_write(ps2_cpu *, uint32_t reg, uint32_t value) { g_log.vu_ctrl[reg & 31] = value; }
void ps2_unimplemented(ps2_cpu *, uint32_t pc, uint32_t insn)
{
	g_log.unimpl++;
	g_log.last_pc = pc;
	g_log.last_insn = insn;
}

void ps2_call_indirect(ps2_cpu *c, uint32_t, uint32_t target)
{
	g_log.indirect_calls.push_back(target);
	if (ps2_func_t f = ps2_lookup_function(ps2_recomp_functions, ps2_recomp_function_count, target)) f(c);
}
void ps2_jump_indirect(ps2_cpu *c, uint32_t, uint32_t target)
{
	g_log.indirect_jumps.push_back(target);
	if (ps2_func_t f = ps2_lookup_function(ps2_recomp_functions, ps2_recomp_function_count, target)) f(c);
}

// Strong definition overriding the weak recompiled callee at 0x00300000
// (verifies the HLE override mechanism documented in ps2_abi.h).
void ps2f_00300000(ps2_cpu *c)
{
	g_log.callee1_hits++;
	g_log.callee1_ra = c->r[31].ud[0];
	g_log.callee1_a0 = c->r[4].ud[0];
	c->r[2].ud[0] = 0xCA11;
}

} // extern "C"
