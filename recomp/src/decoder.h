// SPDX-License-Identifier: GPL-3.0-or-later
// R5900 (Emotion Engine) instruction decoder.
#pragma once

#include <cstdint>
#include <string>

namespace recomp {

enum class Op : uint16_t {
	INVALID = 0,
#define X(e, m, c) e,
#include "ops.def"
#undef X
	COUNT
};

enum class OpClass : uint8_t { INVALID, CPU, BR, MMI, FPU, VU0, COP0, SYS };

const char *op_mnemonic(Op op);
OpClass op_class(Op op);
const char *op_class_name(OpClass c);

// Kind of control transfer for BR-class instructions.
enum class Flow : uint8_t {
	None,      // ordinary instruction
	Branch,    // conditional PC-relative branch (cond evaluated before delay slot)
	Jump,      // j: unconditional direct jump
	Call,      // jal, bltzal/bgezal(+l): direct call (conditional for the REGIMM forms)
	JumpReg,   // jr
	CallReg,   // jalr
};

struct Insn {
	uint32_t pc = 0;
	uint32_t word = 0;
	Op op = Op::INVALID;
	uint8_t rs = 0, rt = 0, rd = 0, sa = 0;
	int32_t imm = 0;     // sign-extended 16-bit immediate
	uint32_t uimm = 0;   // zero-extended 16-bit immediate
	uint32_t target = 0; // branch / jump target (Branch, Jump, Call)
	Flow flow = Flow::None;
	bool likely = false;      // branch-likely: delay slot annulled when not taken
	bool interlock = false;   // COP2 transfer .i bit
	uint8_t dest = 0;         // VU0 field mask (x=8, y=4, z=2, w=1)
	uint8_t bc = 0;           // VU0 broadcast field (0=x .. 3=w)

	bool valid() const { return op != Op::INVALID; }
	bool has_delay_slot() const { return flow != Flow::None; }
	// VU0 register fields (aliases)
	uint8_t ft() const { return rt; }
	uint8_t fs() const { return rd; }
	uint8_t fd() const { return sa; }
};

Insn decode(uint32_t pc, uint32_t word);

// Short human-readable form ("addiu r29, r29, -16") for comments and tests.
std::string disasm(const Insn &i);

} // namespace recomp
