// SPDX-License-Identifier: GPL-3.0-or-later
// Per-function control-flow analysis and C emission.
#pragma once

#include "decoder.h"
#include "image.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace recomp {

struct FuncAnalysis {
	uint32_t start = 0;
	uint32_t limit = 0;                          // exclusive bound for internal code
	std::map<uint32_t, Insn> insns;              // every decoded instruction incl. delay slots
	std::set<uint32_t> standalone;               // reached other than only as a delay slot
	std::set<uint32_t> labels;                   // internal branch / jump-table targets
	std::set<uint32_t> calls;                    // direct call targets (jal, bal, ...)
	std::set<uint32_t> ext_targets;              // branch/jump/fallthrough targets outside [start, limit)
	std::map<uint32_t, std::vector<uint32_t>> jump_tables; // jr pc -> table entries
	std::vector<uint32_t> unresolved_jr;         // jr rs (rs != ra) without a recognised table
	std::vector<std::string> problems;           // reasons this function is not fully translated
};

// Discover the function's reachable code by recursive descent from `start`,
// treating everything outside [start, limit) as external.
FuncAnalysis analyze_function(const Image &img, uint32_t start, uint32_t limit);

struct EmitStats {
	std::array<uint64_t, static_cast<size_t>(Op::COUNT)> op_count{};
	std::map<uint32_t, uint64_t> invalid_words; // major opcode / sub-op key -> count
	uint64_t instructions = 0;
	uint64_t unimplemented = 0;
	uint64_t indirect_jumps = 0;
	uint64_t jump_tables = 0;
	uint64_t delay_slot_branches = 0;
};

struct EmitOptions {
	bool comments = true; // per-instruction "// addr mnemonic" comments
};

// Known entries decide whether a target is a direct call (ps2f_X) or dispatch.
std::string emit_function(const FuncAnalysis &fa, const std::set<uint32_t> &entries,
                          const std::string &comment, const EmitOptions &opt, EmitStats &stats);

// C statement(s) for a single non-control instruction (used for delay slots too).
std::string emit_instruction(const Insn &i, EmitStats &stats);

std::string func_name(uint32_t addr);

} // namespace recomp
