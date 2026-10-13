// SPDX-License-Identifier: GPL-3.0-or-later
#include "translator.h"

#include <cstdio>
#include <optional>

namespace recomp {

std::string func_name(uint32_t addr)
{
	char b[24];
	std::snprintf(b, sizeof b, "ps2f_%08x", addr);
	return b;
}

namespace {

std::string hex32(uint32_t v)
{
	char b[16];
	std::snprintf(b, sizeof b, "0x%08xu", v);
	return b;
}

// GPR written by an instruction, or -1.
int gpr_dest(const Insn &i)
{
	switch (i.op) {
	// rt-destination I-type / loads / coprocessor moves
	case Op::ADDI: case Op::ADDIU: case Op::DADDI: case Op::DADDIU: case Op::SLTI: case Op::SLTIU:
	case Op::ANDI: case Op::ORI: case Op::XORI: case Op::LUI:
	case Op::LB: case Op::LBU: case Op::LH: case Op::LHU: case Op::LW: case Op::LWU: case Op::LD:
	case Op::LQ: case Op::LWL: case Op::LWR: case Op::LDL: case Op::LDR:
	case Op::MFC0: case Op::MFC1: case Op::CFC1: case Op::QMFC2: case Op::CFC2:
		return i.rt;
	case Op::JAL: case Op::BLTZAL: case Op::BGEZAL: case Op::BLTZALL: case Op::BGEZALL:
		return 31;
	case Op::JALR:
		return i.rd;
	default:
		break;
	}
	switch (op_class(i.op)) {
	case OpClass::CPU:
	case OpClass::MMI:
		// remaining R-type ops write rd (mult/div write rd=0 normally)
		switch (i.op) {
		case Op::SB: case Op::SH: case Op::SW: case Op::SD: case Op::SQ: case Op::SWL: case Op::SWR:
		case Op::SDL: case Op::SDR: case Op::MTHI: case Op::MTLO: case Op::MTHI1: case Op::MTLO1:
		case Op::MTSA: case Op::MTSAB: case Op::MTSAH: case Op::PREF: case Op::SYNC: case Op::PMTHI:
		case Op::PMTLO: case Op::PMTHL: case Op::DIV: case Op::DIVU: case Op::DIV1: case Op::DIVU1:
			return -1;
		default:
			return i.rd;
		}
	default:
		return -1;
	}
}

enum class Cond { Always, Never, Expr };

struct CondInfo {
	Cond kind = Cond::Expr;
	std::string expr;
};

std::string g64(int r) { return r ? "GPR_U64(" + std::to_string(r) + ")" : std::string("0ull"); }
std::string s64(int r) { return r ? "GPR_S64(" + std::to_string(r) + ")" : std::string("0ll"); }

CondInfo branch_cond(const Insn &i)
{
	CondInfo c;
	switch (i.op) {
	case Op::BEQ: case Op::BEQL:
		if (i.rs == i.rt) c.kind = Cond::Always;
		else c.expr = g64(i.rs) + " == " + g64(i.rt);
		break;
	case Op::BNE: case Op::BNEL:
		if (i.rs == i.rt) c.kind = Cond::Never;
		else c.expr = g64(i.rs) + " != " + g64(i.rt);
		break;
	case Op::BLEZ: case Op::BLEZL:
		if (i.rs == 0) c.kind = Cond::Always;
		else c.expr = s64(i.rs) + " <= 0";
		break;
	case Op::BGTZ: case Op::BGTZL:
		if (i.rs == 0) c.kind = Cond::Never;
		else c.expr = s64(i.rs) + " > 0";
		break;
	case Op::BLTZ: case Op::BLTZL: case Op::BLTZAL: case Op::BLTZALL:
		if (i.rs == 0) c.kind = Cond::Never;
		else c.expr = s64(i.rs) + " < 0";
		break;
	case Op::BGEZ: case Op::BGEZL: case Op::BGEZAL: case Op::BGEZALL:
		if (i.rs == 0) c.kind = Cond::Always;
		else c.expr = s64(i.rs) + " >= 0";
		break;
	case Op::BC0F: case Op::BC0FL: c.expr = "!ps2_cop0_cond(c)"; break;
	case Op::BC0T: case Op::BC0TL: c.expr = "ps2_cop0_cond(c)"; break;
	case Op::BC1F: case Op::BC1FL: c.expr = "!PS2_FPU_C"; break;
	case Op::BC1T: case Op::BC1TL: c.expr = "PS2_FPU_C"; break;
	case Op::BC2F: case Op::BC2FL: c.expr = "!ps2_cop2_cond(c)"; break;
	case Op::BC2T: case Op::BC2TL: c.expr = "ps2_cop2_cond(c)"; break;
	case Op::JAL: c.kind = Cond::Always; break;
	default: c.kind = Cond::Always; break;
	}
	return c;
}

// ---------------------------------------------------------------------------
// Jump table recognition: jr rs <- lw rs, off(base), base = table + index.
// ---------------------------------------------------------------------------

std::optional<Insn> find_def(const Image &img, int reg, uint32_t from, uint32_t lower)
{
	for (uint32_t pc = from; pc >= lower + 4 && from - pc < 4 * 40;) {
		pc -= 4;
		uint32_t w;
		if (!img.read32(pc, w)) return std::nullopt;
		Insn i = decode(pc, w);
		if (i.flow == Flow::Jump || i.flow == Flow::JumpReg) return std::nullopt;
		if (gpr_dest(i) == reg) return i;
	}
	return std::nullopt;
}

// Constant part of a register value; at most one non-constant (index) addend.
bool base_const(const Image &img, int reg, uint32_t pc, uint32_t lower, int depth, bool &index_used, uint32_t &out)
{
	if (reg == 0) {
		out = 0;
		return true;
	}
	if (depth > 6) return false;
	auto d = find_def(img, reg, pc, lower);
	if (!d) return false;
	uint32_t a = 0, b = 0;
	switch (d->op) {
	case Op::LUI:
		out = d->uimm << 16;
		return true;
	case Op::ADDIU: case Op::DADDIU:
		if (!base_const(img, d->rs, d->pc, lower, depth + 1, index_used, a)) return false;
		out = a + static_cast<uint32_t>(d->imm);
		return true;
	case Op::ORI:
		if (!base_const(img, d->rs, d->pc, lower, depth + 1, index_used, a)) return false;
		out = a | d->uimm;
		return true;
	case Op::ADDU: case Op::DADDU: {
		bool iu1 = index_used, iu2 = index_used;
		bool ka = base_const(img, d->rs, d->pc, lower, depth + 1, iu1, a);
		bool kb = base_const(img, d->rt, d->pc, lower, depth + 1, iu2, b);
		if (ka && kb) {
			out = a + b;
			index_used = iu1 || iu2;
			return true;
		}
		if (index_used) return false;
		if (ka) { out = a; index_used = true; return true; }
		if (kb) { out = b; index_used = true; return true; }
		return false;
	}
	default:
		return false;
	}
}

bool detect_jump_table(const Image &img, const Insn &jr, uint32_t start, uint32_t limit, std::vector<uint32_t> &targets)
{
	auto lw = find_def(img, jr.rs, jr.pc, start);
	if (!lw || lw->op != Op::LW) return false;
	bool index_used = false;
	uint32_t base = 0;
	if (!base_const(img, lw->rs, lw->pc, start, 0, index_used, base) || !index_used) return false;
	base += static_cast<uint32_t>(lw->imm);

	// Bound from a preceding "sltiu rX, rIdx, N".
	uint32_t count = 0;
	for (uint32_t pc = jr.pc; pc >= start + 4 && jr.pc - pc < 4 * 40;) {
		pc -= 4;
		uint32_t w;
		if (!img.read32(pc, w)) break;
		Insn i = decode(pc, w);
		if (i.op == Op::SLTIU && i.imm > 0 && i.imm <= 4096) {
			count = static_cast<uint32_t>(i.imm);
			break;
		}
	}
	auto ok = [&](uint32_t t) { return (t & 3) == 0 && t >= start && t < limit; };
	targets.clear();
	const uint32_t maxn = count ? count : 1024;
	for (uint32_t k = 0; k < maxn; k++) {
		uint32_t t;
		if (!img.read32(base + 4 * k, t) || !ok(t)) {
			if (count) return false;
			break;
		}
		targets.push_back(t);
	}
	return targets.size() >= 2 || (count && !targets.empty());
}

} // namespace

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

FuncAnalysis analyze_function(const Image &img, uint32_t start, uint32_t limit)
{
	FuncAnalysis fa;
	fa.start = start;
	fa.limit = limit;
	auto inside = [&](uint32_t a) { return a >= start && a < limit; };
	auto get = [&](uint32_t a, Insn &out) -> bool {
		auto it = fa.insns.find(a);
		if (it != fa.insns.end()) {
			out = it->second;
			return true;
		}
		uint32_t w;
		if (!img.read32(a, w)) return false;
		out = decode(a, w);
		fa.insns.emplace(a, out);
		return true;
	};

	std::vector<uint32_t> work{start};
	while (!work.empty()) {
		uint32_t a = work.back();
		work.pop_back();
		for (;;) {
			if (!inside(a)) {
				fa.ext_targets.insert(a);
				break;
			}
			if (!fa.standalone.insert(a).second) break;
			Insn i;
			if (!get(a, i)) {
				char b[64];
				std::snprintf(b, sizeof b, "unreadable code at %08x", a);
				fa.problems.push_back(b);
				break;
			}
			if (i.flow == Flow::None) {
				if (i.op == Op::ERET) break;
				a += 4;
				continue;
			}
			Insn d;
			if (!get(a + 4, d)) {
				char b[64];
				std::snprintf(b, sizeof b, "unreadable delay slot at %08x", a + 4);
				fa.problems.push_back(b);
			}
			auto add_target = [&](uint32_t t) {
				if (inside(t)) {
					fa.labels.insert(t);
					work.push_back(t);
				} else {
					fa.ext_targets.insert(t);
				}
			};
			bool stop = false;
			switch (i.flow) {
			case Flow::Branch: {
				CondInfo ci = branch_cond(i);
				if (ci.kind != Cond::Never) add_target(i.target);
				if (ci.kind == Cond::Always) stop = true;
				break;
			}
			case Flow::Jump:
				add_target(i.target);
				stop = true;
				break;
			case Flow::Call:
				if (branch_cond(i).kind != Cond::Never) fa.calls.insert(i.target);
				break;
			case Flow::JumpReg:
				if (i.rs != 31) {
					std::vector<uint32_t> t;
					if (detect_jump_table(img, i, start, limit, t)) {
						fa.jump_tables[a] = t;
						for (uint32_t x : t) add_target(x);
					} else {
						fa.unresolved_jr.push_back(a);
					}
				}
				stop = true;
				break;
			case Flow::CallReg:
				break;
			case Flow::None:
				break;
			}
			if (stop) break;
			a += 8;
		}
	}
	return fa;
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

namespace {

struct FuncEmitter {
	const FuncAnalysis &fa;
	const std::set<uint32_t> &entries;
	const EmitOptions &opt;
	EmitStats &st;
	std::set<uint32_t> labels_needed;

	bool internal(uint32_t t) const { return t >= fa.start && t < fa.limit && fa.standalone.count(t); }

	std::string transfer(uint32_t pc, uint32_t target)
	{
		if (internal(target)) {
			labels_needed.insert(target);
			char b[32];
			std::snprintf(b, sizeof b, "goto L_%08x;", target);
			return b;
		}
		if (entries.count(target)) return "{ PS2_TAILCALL(" + func_name(target) + "); }";
		return "{ ps2_jump_indirect(c, " + hex32(pc) + ", " + hex32(target) + "); return; }";
	}

	std::string call(uint32_t pc, uint32_t target)
	{
		if (entries.count(target)) return func_name(target) + "(c);";
		return "ps2_call_indirect(c, " + hex32(pc) + ", " + hex32(target) + ");";
	}

	std::string delay(const Insn &br)
	{
		auto it = fa.insns.find(br.pc + 4);
		if (it == fa.insns.end()) {
			st.unimplemented++;
			return "ps2_unimplemented(c, " + hex32(br.pc + 4) + ", 0u);";
		}
		const Insn &d = it->second;
		if (d.has_delay_slot()) {
			st.delay_slot_branches++;
			st.unimplemented++;
			return "ps2_unimplemented(c, " + hex32(d.pc) + ", " + hex32(d.word) + "); /* branch in delay slot */";
		}
		st.instructions++;
		st.op_count[static_cast<size_t>(d.op)]++;
		return emit_instruction(d, st);
	}

	static std::string join(const std::string &a, const std::string &b)
	{
		if (a.empty()) return b;
		if (b.empty()) return a;
		return a + " " + b;
	}

	// Returns code; sets `fall` to the fall-through address (if any).
	std::string control(const Insn &i, std::optional<uint32_t> &fall)
	{
		const uint32_t pc = i.pc;
		std::string ds = delay(i);
		std::string ra = "GPR_U64(31) = " + hex32(pc + 8) + ";";
		switch (i.flow) {
		case Flow::Branch: {
			CondInfo ci = branch_cond(i);
			std::string tr = transfer(pc, i.target);
			fall = pc + 8;
			if (ci.kind == Cond::Always) {
				fall.reset();
				return "{ " + join(ds, tr) + " }";
			}
			if (ci.kind == Cond::Never) return i.likely ? std::string() : ds;
			if (i.likely) return "if (" + ci.expr + ") { " + join(ds, tr) + " }";
			if (ds.empty()) return "if (" + ci.expr + ") " + tr;
			return "{ const int br = " + ci.expr + "; " + ds + " if (br) " + tr + " }";
		}
		case Flow::Jump:
			fall.reset();
			return "{ " + join(ds, transfer(pc, i.target)) + " }";
		case Flow::Call: {
			CondInfo ci = branch_cond(i);
			fall = pc + 8;
			std::string cl = call(pc, i.target);
			if (ci.kind == Cond::Always) return join(join(ra, ds), cl);
			if (ci.kind == Cond::Never) return i.likely ? ra : join(ra, ds);
			if (i.likely) return "if (" + ci.expr + ") { " + join(join(ra, ds), cl) + " } else { " + ra + " }";
			return "{ const int br = " + ci.expr + "; " + join(ra, ds) + " if (br) " + cl + " }";
		}
		case Flow::JumpReg: {
			fall.reset();
			if (i.rs == 31) return "{ " + join(ds, "return;") + " }";
			std::string tgt = "const uint32_t jt = " + std::string(i.rs ? "GPR_U32(" + std::to_string(i.rs) + ")" : "0u") + ";";
			auto jt = fa.jump_tables.find(pc);
			if (jt != fa.jump_tables.end()) {
				st.jump_tables++;
				std::set<uint32_t> uniq(jt->second.begin(), jt->second.end());
				std::string sw = "switch (jt) {";
				for (uint32_t t : uniq) sw += " case " + hex32(t) + ": " + transfer(pc, t);
				sw += " default: ps2_jump_indirect(c, " + hex32(pc) + ", jt); return; }";
				return "{ " + join(join(tgt, ds), sw) + " }";
			}
			st.indirect_jumps++;
			return "{ " + join(tgt, ds) + " ps2_jump_indirect(c, " + hex32(pc) + ", jt); return; }";
		}
		case Flow::CallReg: {
			fall = pc + 8;
			std::string tgt = "const uint32_t jt = " + std::string(i.rs ? "GPR_U32(" + std::to_string(i.rs) + ")" : "0u") + ";";
			std::string link = i.rd ? "GPR_U64(" + std::to_string(i.rd) + ") = " + hex32(pc + 8) + ";" : "";
			return "{ " + join(join(join(tgt, link), ds), "ps2_call_indirect(c, " + hex32(pc) + ", jt);") + " }";
		}
		case Flow::None:
			break;
		}
		return {};
	}

	struct Unit {
		uint32_t addr;
		std::string code;
		bool is_insn;
	};

	std::string run(const std::string &comment)
	{
		std::vector<Unit> units;
		std::optional<uint32_t> fall;
		for (uint32_t a : fa.standalone) {
			if (fall && *fall != a) units.push_back({a, transfer(*fall - 4, *fall), false});
			const Insn &i = fa.insns.at(a);
			std::optional<uint32_t> nf;
			std::string code;
			st.instructions++;
			st.op_count[static_cast<size_t>(i.op)]++;
			if (i.has_delay_slot()) {
				code = control(i, nf);
			} else {
				code = emit_instruction(i, st);
				if (i.op != Op::ERET) nf = a + 4;
			}
			if (opt.comments) {
				char b[48];
				std::snprintf(b, sizeof b, "%s// %08x %s", code.empty() ? "" : " ", i.pc, op_mnemonic(i.op));
				code += b;
			}
			units.push_back({a, std::move(code), true});
			fall = nf;
		}
		if (fall) units.push_back({*fall, transfer(*fall - 4, *fall), false});

		std::string out;
		out.reserve(units.size() * 64 + 256);
		out += "/* " + comment + " */\n";
		out += "PS2_FN(" + func_name(fa.start) + ")\n{\n";
		for (const Unit &u : units) {
			if (u.is_insn && labels_needed.count(u.addr)) {
				char b[32];
				std::snprintf(b, sizeof b, "L_%08x:;\n", u.addr);
				out += b;
			}
			if (u.code.empty()) continue;
			out += "\t";
			out += u.code;
			out += "\n";
		}
		out += "}\n\n";
		return out;
	}
};

} // namespace

std::string emit_function(const FuncAnalysis &fa, const std::set<uint32_t> &entries, const std::string &comment,
                          const EmitOptions &opt, EmitStats &stats)
{
	FuncEmitter fe{fa, entries, opt, stats, {}};
	return fe.run(comment);
}

} // namespace recomp
