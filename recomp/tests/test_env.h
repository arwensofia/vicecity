// SPDX-License-Identifier: GPL-3.0-or-later
// Test runtime: records every hook call made by recompiled test code.
#pragma once

#include "ps2_abi.h"

#include <cstdint>
#include <vector>

struct HookLog {
	int syscalls = 0, breaks = 0, traps = 0, unimpl = 0, cop0_ops = 0, caches = 0, waits = 0, callms = 0;
	uint32_t last_pc = 0, last_code = 0, last_insn = 0, last_addr = 0, last_reg = 0, last_value = 0;
	uint32_t syscall_v1 = 0;
	int cop0_cond = 0, cop2_cond = 0;
	std::vector<uint32_t> indirect_calls, indirect_jumps;
	int callee1_hits = 0;
	uint64_t callee1_ra = 0, callee1_a0 = 0;
	// slow-path I/O window at 0x10000000
	uint32_t io_last_write_addr = 0;
	uint64_t io_last_write_val = 0;
	int io_writes = 0;
	uint32_t vu_ctrl[32] = {};
};

extern HookLog g_log;
