// SPDX-License-Identifier: GPL-3.0-or-later
// C emission for individual (non-control-transfer) R5900 instructions.
// Helpers referenced here live in recomp/include/ps2_ops.h.
#include "translator.h"

#include <cstdio>

namespace recomp {

namespace {

std::string num(int v) { return std::to_string(v); }

std::string hex32(uint32_t v)
{
	char b[16];
	std::snprintf(b, sizeof b, "0x%08xu", v);
	return b;
}

std::string hex64(uint64_t v)
{
	char b[24];
	std::snprintf(b, sizeof b, "0x%016llxull", static_cast<unsigned long long>(v));
	return b;
}

// Register operand expressions; r0 folds to a constant.
std::string u64(int r) { return r ? "GPR_U64(" + num(r) + ")" : std::string("0ull"); }
std::string s64(int r) { return r ? "GPR_S64(" + num(r) + ")" : std::string("0ll"); }
std::string u32(int r) { return r ? "GPR_U32(" + num(r) + ")" : std::string("0u"); }
std::string s32(int r) { return r ? "GPR_S32(" + num(r) + ")" : std::string("0"); }
std::string dst(int r) { return "GPR_U64(" + num(r) + ")"; }

// Effective address rs + imm (32-bit wrap).
std::string ea(const Insn &i)
{
	std::string base = u32(i.rs);
	if (i.imm == 0) return base;
	if (i.rs == 0) return hex32(static_cast<uint32_t>(i.imm));
	char b[24];
	if (i.imm < 0)
		std::snprintf(b, sizeof b, " - 0x%xu", static_cast<unsigned>(-i.imm));
	else
		std::snprintf(b, sizeof b, " + 0x%xu", static_cast<unsigned>(i.imm));
	return "(" + base + b + ")";
}

std::string vf(int r) { return "c->vf[" + num(r) + "]"; }
std::string vdst(int fd) { return fd ? "&c->vf[" + num(fd) + "]" : std::string("(ps2_u128 *)0"); }

const char *vu_kind(Op op)
{
	switch (op) {
	case Op::VADDbc: case Op::VADDq: case Op::VADDi: case Op::VADD:
	case Op::VADDAbc: case Op::VADDAq: case Op::VADDAi: case Op::VADDA:
		return "PS2_VU_ADD";
	case Op::VSUBbc: case Op::VSUBq: case Op::VSUBi: case Op::VSUB:
	case Op::VSUBAbc: case Op::VSUBAq: case Op::VSUBAi: case Op::VSUBA:
		return "PS2_VU_SUB";
	case Op::VMULbc: case Op::VMULq: case Op::VMULi: case Op::VMUL:
	case Op::VMULAbc: case Op::VMULAq: case Op::VMULAi: case Op::VMULA:
		return "PS2_VU_MUL";
	case Op::VMADDbc: case Op::VMADDq: case Op::VMADDi: case Op::VMADD:
	case Op::VMADDAbc: case Op::VMADDAq: case Op::VMADDAi: case Op::VMADDA:
		return "PS2_VU_MADD";
	default:
		return "PS2_VU_MSUB";
	}
}

// Second operand of a VU FMAC op: full vector, broadcast field, Q or I.
std::string vu_t(const Insn &i)
{
	switch (i.op) {
	case Op::VADDbc: case Op::VSUBbc: case Op::VMADDbc: case Op::VMSUBbc: case Op::VMULbc:
	case Op::VMAXbc: case Op::VMINIbc:
	case Op::VADDAbc: case Op::VSUBAbc: case Op::VMADDAbc: case Op::VMSUBAbc: case Op::VMULAbc:
		return "ps2_vsplat(c->vf[" + num(i.ft()) + "].uw[" + num(i.bc) + "])";
	case Op::VMULq: case Op::VADDq: case Op::VMADDq: case Op::VSUBq: case Op::VMSUBq:
	case Op::VMULAq: case Op::VADDAq: case Op::VMADDAq: case Op::VSUBAq: case Op::VMSUBAq:
		return "ps2_vsplat(c->vi[PS2_VI_Q])";
	case Op::VMAXi: case Op::VMULi: case Op::VMINIi: case Op::VADDi: case Op::VMADDi: case Op::VSUBi:
	case Op::VMSUBi: case Op::VMULAi: case Op::VADDAi: case Op::VMADDAi: case Op::VSUBAi: case Op::VMSUBAi:
		return "ps2_vsplat(c->vi[PS2_VI_I])";
	default:
		return vf(i.ft());
	}
}

bool vu_acc_dest(Op op)
{
	switch (op) {
	case Op::VADDAbc: case Op::VSUBAbc: case Op::VMADDAbc: case Op::VMSUBAbc: case Op::VMULAbc:
	case Op::VMULAq: case Op::VMULAi: case Op::VADDAq: case Op::VMADDAq: case Op::VADDAi:
	case Op::VMADDAi: case Op::VSUBAq: case Op::VMSUBAq: case Op::VSUBAi: case Op::VMSUBAi:
	case Op::VADDA: case Op::VMADDA: case Op::VMULA: case Op::VSUBA: case Op::VMSUBA:
		return true;
	default:
		return false;
	}
}

std::string mmi_name(Op op)
{
	std::string m = op_mnemonic(op);
	return "ps2_mmi_" + m;
}

} // namespace

std::string emit_instruction(const Insn &i, EmitStats &st)
{
	const int rs = i.rs, rt = i.rt, rd = i.rd, sa = i.sa;
	const std::string pc = hex32(i.pc);
	auto unimpl = [&]() {
		st.unimplemented++;
		uint32_t key = (i.word >> 26) << 8;
		switch (i.word >> 26) {
		case 0x00: case 0x1C: key |= i.word & 63; break;   // SPECIAL/MMI funct
		case 0x01: key |= i.rt; break;                       // REGIMM rt
		case 0x10: case 0x11: case 0x12: key |= i.rs; break; // COPz rs
		default: break;
		}
		st.invalid_words[key]++;
		return "ps2_unimplemented(c, " + pc + ", " + hex32(i.word) + ");";
	};
	// rd/rt-destination helpers: writes to r0 are dropped.
	auto set_rd = [&](const std::string &expr) { return rd ? dst(rd) + " = " + expr + ";" : std::string(); };
	auto set_rt = [&](const std::string &expr) { return rt ? dst(rt) + " = " + expr + ";" : std::string(); };
	auto load = [&](const std::string &expr) { return rt ? dst(rt) + " = " + expr + ";" : "(void)" + expr + ";"; };
	const std::string I = num(rd) + ", " + num(rs) + ", " + num(rt); // "rd, rs, rt"

	switch (i.op) {
	case Op::INVALID:
		return unimpl();

	// --- shifts ---
	case Op::SLL: return set_rd("SEXT32(" + u32(rt) + " << " + num(sa) + ")");
	case Op::SRL: return set_rd("SEXT32(" + u32(rt) + " >> " + num(sa) + ")");
	case Op::SRA: return set_rd("SEXT32((uint32_t)(" + s32(rt) + " >> " + num(sa) + "))");
	case Op::SLLV: return set_rd("SEXT32(" + u32(rt) + " << (" + u32(rs) + " & 31))");
	case Op::SRLV: return set_rd("SEXT32(" + u32(rt) + " >> (" + u32(rs) + " & 31))");
	case Op::SRAV: return set_rd("SEXT32((uint32_t)(" + s32(rt) + " >> (" + u32(rs) + " & 31)))");
	case Op::DSLL: return set_rd(u64(rt) + " << " + num(sa));
	case Op::DSRL: return set_rd(u64(rt) + " >> " + num(sa));
	case Op::DSRA: return set_rd("(uint64_t)(" + s64(rt) + " >> " + num(sa) + ")");
	case Op::DSLL32: return set_rd(u64(rt) + " << " + num(sa + 32));
	case Op::DSRL32: return set_rd(u64(rt) + " >> " + num(sa + 32));
	case Op::DSRA32: return set_rd("(uint64_t)(" + s64(rt) + " >> " + num(sa + 32) + ")");
	case Op::DSLLV: return set_rd(u64(rt) + " << (" + u32(rs) + " & 63)");
	case Op::DSRLV: return set_rd(u64(rt) + " >> (" + u32(rs) + " & 63)");
	case Op::DSRAV: return set_rd("(uint64_t)(" + s64(rt) + " >> (" + u32(rs) + " & 63))");

	// --- ALU (add/sub overflow traps are not modelled: identical to addu/subu) ---
	case Op::ADD: case Op::ADDU:
		if (rt == 0) return set_rd("SEXT32(" + u32(rs) + ")");
		if (rs == 0) return set_rd("SEXT32(" + u32(rt) + ")");
		return set_rd("SEXT32(" + u32(rs) + " + " + u32(rt) + ")");
	case Op::SUB: case Op::SUBU: return set_rd("SEXT32(" + u32(rs) + " - " + u32(rt) + ")");
	case Op::DADD: case Op::DADDU:
		if (rt == 0) return set_rd(u64(rs));
		if (rs == 0) return set_rd(u64(rt));
		return set_rd(u64(rs) + " + " + u64(rt));
	case Op::DSUB: case Op::DSUBU: return set_rd(u64(rs) + " - " + u64(rt));
	case Op::AND: return set_rd(u64(rs) + " & " + u64(rt));
	case Op::OR:
		if (rt == 0) return set_rd(u64(rs));
		if (rs == 0) return set_rd(u64(rt));
		return set_rd(u64(rs) + " | " + u64(rt));
	case Op::XOR: return set_rd(u64(rs) + " ^ " + u64(rt));
	case Op::NOR: return set_rd("~(" + u64(rs) + " | " + u64(rt) + ")");
	case Op::SLT: return set_rd("(uint64_t)(" + s64(rs) + " < " + s64(rt) + ")");
	case Op::SLTU: return set_rd("(uint64_t)(" + u64(rs) + " < " + u64(rt) + ")");
	case Op::MOVZ: return rd ? "if (" + u64(rt) + " == 0) " + dst(rd) + " = " + u64(rs) + ";" : "";
	case Op::MOVN: return rd ? "if (" + u64(rt) + " != 0) " + dst(rd) + " = " + u64(rs) + ";" : "";
	case Op::ADDI: case Op::ADDIU:
		if (rs == 0) return set_rt(hex64(static_cast<uint64_t>(static_cast<int64_t>(i.imm))));
		return set_rt("SEXT32(" + u32(rs) + " + " + hex32(static_cast<uint32_t>(i.imm)) + ")");
	case Op::DADDI: case Op::DADDIU:
		if (rs == 0) return set_rt(hex64(static_cast<uint64_t>(static_cast<int64_t>(i.imm))));
		return set_rt(u64(rs) + " + " + hex64(static_cast<uint64_t>(static_cast<int64_t>(i.imm))));
	case Op::SLTI: return set_rt("(uint64_t)(" + s64(rs) + " < " + std::to_string(i.imm) + "ll)");
	case Op::SLTIU:
		return set_rt("(uint64_t)(" + u64(rs) + " < " + hex64(static_cast<uint64_t>(static_cast<int64_t>(i.imm))) + ")");
	case Op::ANDI: return set_rt(u64(rs) + " & " + hex64(i.uimm));
	case Op::ORI:
		if (rs == 0) return set_rt(hex64(i.uimm));
		return set_rt(u64(rs) + " | " + hex64(i.uimm));
	case Op::XORI: return set_rt(u64(rs) + " ^ " + hex64(i.uimm));
	case Op::LUI: return set_rt(hex64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(i.uimm << 16)))));

	// --- HI/LO ---
	case Op::MFHI: return set_rd("c->hi.ud[0]");
	case Op::MFLO: return set_rd("c->lo.ud[0]");
	case Op::MTHI: return "c->hi.ud[0] = " + u64(rs) + ";";
	case Op::MTLO: return "c->lo.ud[0] = " + u64(rs) + ";";
	case Op::MFHI1: return set_rd("c->hi.ud[1]");
	case Op::MFLO1: return set_rd("c->lo.ud[1]");
	case Op::MTHI1: return "c->hi.ud[1] = " + u64(rs) + ";";
	case Op::MTLO1: return "c->lo.ud[1] = " + u64(rs) + ";";
	case Op::MULT: return "ps2_op_mult(c, " + I + ", 0);";
	case Op::MULTU: return "ps2_op_multu(c, " + I + ", 0);";
	case Op::MULT1: return "ps2_op_mult(c, " + I + ", 1);";
	case Op::MULTU1: return "ps2_op_multu(c, " + I + ", 1);";
	case Op::MADD: return "ps2_op_madd(c, " + I + ", 0);";
	case Op::MADDU: return "ps2_op_maddu(c, " + I + ", 0);";
	case Op::MADD1: return "ps2_op_madd(c, " + I + ", 1);";
	case Op::MADDU1: return "ps2_op_maddu(c, " + I + ", 1);";
	case Op::DIV: return "ps2_op_div(c, " + num(rs) + ", " + num(rt) + ", 0);";
	case Op::DIVU: return "ps2_op_divu(c, " + num(rs) + ", " + num(rt) + ", 0);";
	case Op::DIV1: return "ps2_op_div(c, " + num(rs) + ", " + num(rt) + ", 1);";
	case Op::DIVU1: return "ps2_op_divu(c, " + num(rs) + ", " + num(rt) + ", 1);";
	case Op::MFSA: return set_rd("(uint64_t)c->sa");
	case Op::MTSA: return "c->sa = " + u32(rs) + ";";
	case Op::MTSAB: return "c->sa = (" + u32(rs) + " & 15) ^ " + num(i.imm & 15) + "u;";
	case Op::MTSAH: return "c->sa = ((" + u32(rs) + " & 7) ^ " + num(i.imm & 7) + "u) << 1;";

	// --- loads / stores ---
	case Op::LB: return load("(uint64_t)(int64_t)(int8_t)ps2_read8(c, " + ea(i) + ")");
	case Op::LBU: return load("(uint64_t)ps2_read8(c, " + ea(i) + ")");
	case Op::LH: return load("(uint64_t)(int64_t)(int16_t)ps2_read16(c, " + ea(i) + ")");
	case Op::LHU: return load("(uint64_t)ps2_read16(c, " + ea(i) + ")");
	case Op::LW: return load("SEXT32(ps2_read32(c, " + ea(i) + "))");
	case Op::LWU: return load("(uint64_t)ps2_read32(c, " + ea(i) + ")");
	case Op::LD: return load("ps2_read64(c, " + ea(i) + ")");
	case Op::LQ: return "ps2_op_lq(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::LWL: return "ps2_op_lwl(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::LWR: return "ps2_op_lwr(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::LDL: return "ps2_op_ldl(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::LDR: return "ps2_op_ldr(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::SB: return "ps2_write8(c, " + ea(i) + ", (uint8_t)" + u32(rt) + ");";
	case Op::SH: return "ps2_write16(c, " + ea(i) + ", (uint16_t)" + u32(rt) + ");";
	case Op::SW: return "ps2_write32(c, " + ea(i) + ", " + u32(rt) + ");";
	case Op::SD: return "ps2_write64(c, " + ea(i) + ", " + u64(rt) + ");";
	case Op::SQ: return "ps2_write128(c, " + ea(i) + " & ~15u, &c->r[" + num(rt) + "]);";
	case Op::SWL: return "ps2_op_swl(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::SWR: return "ps2_op_swr(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::SDL: return "ps2_op_sdl(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::SDR: return "ps2_op_sdr(c, " + num(rt) + ", " + ea(i) + ");";
	case Op::LWC1: return "c->f[" + num(rt) + "].u = ps2_read32(c, " + ea(i) + ");";
	case Op::SWC1: return "ps2_write32(c, " + ea(i) + ", c->f[" + num(rt) + "].u);";
	case Op::LQC2:
		if (rt) return "ps2_read128(c, " + ea(i) + " & ~15u, &c->vf[" + num(rt) + "]);";
		return "{ ps2_u128 v; ps2_read128(c, " + ea(i) + " & ~15u, &v); }";
	case Op::SQC2: return "ps2_write128(c, " + ea(i) + " & ~15u, &c->vf[" + num(rt) + "]);";
	case Op::CACHE: return "ps2_cache(c, " + pc + ", " + num(rt) + "u, " + ea(i) + ");";
	case Op::PREF: case Op::SYNC: return "";

	// --- system ---
	case Op::SYSCALL: return "c->pc = " + pc + "; ps2_syscall(c, " + pc + ", " + hex32((i.word >> 6) & 0xFFFFF) + ");";
	case Op::BREAK: return "c->pc = " + pc + "; ps2_break(c, " + pc + ", " + hex32((i.word >> 6) & 0xFFFFF) + ");";
	case Op::TGE: case Op::TGEU: case Op::TLT: case Op::TLTU: case Op::TEQ: case Op::TNE: {
		const char *op = i.op == Op::TGE || i.op == Op::TGEU ? ">=" : i.op == Op::TLT || i.op == Op::TLTU ? "<" : i.op == Op::TEQ ? "==" : "!=";
		bool uns = i.op == Op::TGEU || i.op == Op::TLTU;
		std::string a = uns ? u64(rs) : s64(rs), b = uns ? u64(rt) : s64(rt);
		return "if (" + a + " " + op + " " + b + ") ps2_trap(c, " + pc + ", " + hex32((i.word >> 6) & 0x3FF) + ");";
	}
	case Op::TGEI: case Op::TGEIU: case Op::TLTI: case Op::TLTIU: case Op::TEQI: case Op::TNEI: {
		const char *op = i.op == Op::TGEI || i.op == Op::TGEIU ? ">=" : i.op == Op::TLTI || i.op == Op::TLTIU ? "<" : i.op == Op::TEQI ? "==" : "!=";
		bool uns = i.op == Op::TGEIU || i.op == Op::TLTIU;
		std::string a = uns ? u64(rs) : s64(rs);
		std::string b = uns ? hex64(static_cast<uint64_t>(static_cast<int64_t>(i.imm))) : std::to_string(i.imm) + "ll";
		return "if (" + a + " " + op + " " + b + ") ps2_trap(c, " + pc + ", 0u);";
	}

	// --- COP0 ---
	case Op::MFC0: {
		std::string call = "ps2_cop0_read(c, " + pc + ", " + num(rd) + "u, " + num(i.word & 0x7FF) + "u)";
		return rt ? dst(rt) + " = SEXT32(" + call + ");" : "(void)" + call + ";";
	}
	case Op::MTC0: return "ps2_cop0_write(c, " + pc + ", " + num(rd) + "u, " + num(i.word & 0x7FF) + "u, " + u32(rt) + ");";
	case Op::TLBR: case Op::TLBWI: case Op::TLBWR: case Op::TLBP: case Op::EI: case Op::DI:
		return "ps2_cop0_op(c, " + pc + ", " + hex32(i.word) + ");";
	case Op::ERET: return "ps2_cop0_op(c, " + pc + ", " + hex32(i.word) + "); return;";

	// --- MMI ---
	case Op::PLZCW: return "ps2_mmi_plzcw(c, " + num(rd) + ", " + num(rs) + ");";
	case Op::PMFHL: return "ps2_mmi_pmfhl(c, " + num(rd) + ", " + num(sa) + ");";
	case Op::PMTHL: return "ps2_mmi_pmthl(c, " + num(rs) + ", " + num(sa) + ");";
	case Op::PSLLH: case Op::PSRLH: case Op::PSRAH: case Op::PSLLW: case Op::PSRLW: case Op::PSRAW:
		return mmi_name(i.op) + "(c, " + num(rd) + ", " + num(rt) + ", " + num(sa) + ");";
	case Op::PMFHI: return rd ? "c->r[" + num(rd) + "] = c->hi;" : "";
	case Op::PMFLO: return rd ? "c->r[" + num(rd) + "] = c->lo;" : "";
	case Op::PMTHI: return "c->hi = c->r[" + num(rs) + "];";
	case Op::PMTLO: return "c->lo = c->r[" + num(rs) + "];";
	case Op::PADDW: case Op::PSUBW: case Op::PCGTW: case Op::PMAXW: case Op::PADDH: case Op::PSUBH:
	case Op::PCGTH: case Op::PMAXH: case Op::PADDB: case Op::PSUBB: case Op::PCGTB: case Op::PADDSW:
	case Op::PSUBSW: case Op::PEXTLW: case Op::PPACW: case Op::PADDSH: case Op::PSUBSH: case Op::PEXTLH:
	case Op::PPACH: case Op::PADDSB: case Op::PSUBSB: case Op::PEXTLB: case Op::PPACB: case Op::PEXT5:
	case Op::PPAC5: case Op::PABSW: case Op::PCEQW: case Op::PMINW: case Op::PADSBH: case Op::PABSH:
	case Op::PCEQH: case Op::PMINH: case Op::PCEQB: case Op::PADDUW: case Op::PSUBUW: case Op::PEXTUW:
	case Op::PADDUH: case Op::PSUBUH: case Op::PEXTUH: case Op::PADDUB: case Op::PSUBUB: case Op::PEXTUB:
	case Op::QFSRV: case Op::PSLLVW: case Op::PSRLVW: case Op::PINTH: case Op::PCPYLD:
	case Op::PAND: case Op::PXOR: case Op::PEXEH: case Op::PREVH: case Op::PEXEW: case Op::PROT3W:
	case Op::PSRAVW: case Op::PINTEH: case Op::PCPYUD: case Op::POR: case Op::PNOR: case Op::PEXCH:
	case Op::PCPYH: case Op::PEXCW:
		if (rd == 0) return ""; // pure register ops with no other side effect
		return mmi_name(i.op) + "(c, " + I + ");";
	case Op::PMADDW: case Op::PMSUBW: case Op::PMULTW: case Op::PDIVW: case Op::PMADDH: case Op::PHMADH:
	case Op::PMSUBH: case Op::PHMSBH: case Op::PMULTH: case Op::PDIVBW: case Op::PMADDUW: case Op::PMULTUW:
	case Op::PDIVUW:
		return mmi_name(i.op) + "(c, " + I + ");";

	// --- COP1 ---
	case Op::MFC1: return set_rt("SEXT32(c->f[" + num(rd) + "].u)");
	case Op::MTC1: return "c->f[" + num(rd) + "].u = " + u32(rt) + ";";
	case Op::CFC1:
		return set_rt(rd == 31 ? std::string("SEXT32(c->fcr31)") : rd == 0 ? std::string("SEXT32(c->fcr0)") : std::string("0ull"));
	case Op::CTC1: return rd == 31 ? "c->fcr31 = " + u32(rt) + ";" : std::string();
	case Op::ADD_S: return "ps2_fpu_add(c, " + num(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::SUB_S: return "ps2_fpu_sub(c, " + num(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::MUL_S: return "ps2_fpu_mul(c, " + num(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::DIV_S: return "ps2_fpu_div(c, " + num(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::MADD_S: return "ps2_fpu_madd(c, " + num(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::MSUB_S: return "ps2_fpu_msub(c, " + num(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::RSQRT_S: return "ps2_fpu_rsqrt(c, " + num(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::SQRT_S: return "ps2_fpu_sqrt(c, " + num(i.fd()) + ", " + num(i.ft()) + ");";
	case Op::ADDA_S: return "ps2_fpu_adda(c, " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::SUBA_S: return "ps2_fpu_suba(c, " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::MULA_S: return "ps2_fpu_mula(c, " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::MADDA_S: return "ps2_fpu_madda(c, " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::MSUBA_S: return "ps2_fpu_msuba(c, " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::ABS_S: return "c->f[" + num(i.fd()) + "].u = c->f[" + num(i.fs()) + "].u & 0x7FFFFFFFu;";
	case Op::NEG_S: return "c->f[" + num(i.fd()) + "].u = c->f[" + num(i.fs()) + "].u ^ 0x80000000u;";
	case Op::MOV_S: return "c->f[" + num(i.fd()) + "].u = c->f[" + num(i.fs()) + "].u;";
	case Op::CVT_W_S: return "ps2_fpu_cvt_w_s(c, " + num(i.fd()) + ", " + num(i.fs()) + ");";
	case Op::CVT_S_W: return "ps2_fpu_cvt_s_w(c, " + num(i.fd()) + ", " + num(i.fs()) + ");";
	case Op::MAX_S: return "c->f[" + num(i.fd()) + "].u = ps2_fmax_bits(c->f[" + num(i.fs()) + "].u, c->f[" + num(i.ft()) + "].u);";
	case Op::MIN_S: return "c->f[" + num(i.fd()) + "].u = ps2_fmin_bits(c->f[" + num(i.fs()) + "].u, c->f[" + num(i.ft()) + "].u);";
	case Op::C_F_S: return "c->fcr31 &= ~PS2_FCR31_C;";
	case Op::C_EQ_S: return "ps2_fpu_cmp(c, PS2_FV(" + num(i.fs()) + ") == PS2_FV(" + num(i.ft()) + "));";
	case Op::C_LT_S: return "ps2_fpu_cmp(c, PS2_FV(" + num(i.fs()) + ") < PS2_FV(" + num(i.ft()) + "));";
	case Op::C_LE_S: return "ps2_fpu_cmp(c, PS2_FV(" + num(i.fs()) + ") <= PS2_FV(" + num(i.ft()) + "));";

	// --- COP2 transfers ---
	case Op::QMFC2: {
		std::string w = i.interlock ? "ps2_vu0_wait(c); " : "";
		return w + (rt ? "c->r[" + num(rt) + "] = " + vf(rd) + ";" : std::string());
	}
	case Op::QMTC2: {
		std::string w = i.interlock ? "ps2_vu0_wait(c); " : "";
		return w + (rd ? vf(rd) + " = c->r[" + num(rt) + "];" : std::string());
	}
	case Op::CFC2: return std::string(i.interlock ? "ps2_vu0_wait(c); " : "") + "ps2_cop2_cfc2(c, " + num(rt) + ", " + num(rd) + ");";
	case Op::CTC2: return std::string(i.interlock ? "ps2_vu0_wait(c); " : "") + "ps2_cop2_ctc2(c, " + num(rt) + ", " + num(rd) + ");";

	// --- VU0 macro: FMAC ---
	case Op::VADDbc: case Op::VSUBbc: case Op::VMADDbc: case Op::VMSUBbc: case Op::VMULbc:
	case Op::VMULq: case Op::VMULi: case Op::VADDq: case Op::VMADDq: case Op::VADDi: case Op::VMADDi:
	case Op::VSUBq: case Op::VMSUBq: case Op::VSUBi: case Op::VMSUBi:
	case Op::VADD: case Op::VMADD: case Op::VMUL: case Op::VSUB: case Op::VMSUB:
	case Op::VADDAbc: case Op::VSUBAbc: case Op::VMADDAbc: case Op::VMSUBAbc: case Op::VMULAbc:
	case Op::VMULAq: case Op::VMULAi: case Op::VADDAq: case Op::VMADDAq: case Op::VADDAi: case Op::VMADDAi:
	case Op::VSUBAq: case Op::VMSUBAq: case Op::VSUBAi: case Op::VMSUBAi:
	case Op::VADDA: case Op::VMADDA: case Op::VMULA: case Op::VSUBA: case Op::VMSUBA: {
		std::string d = vu_acc_dest(i.op) ? std::string("&c->vacc") : vdst(i.fd());
		return std::string("ps2_vu_fmac(c, ") + vu_kind(i.op) + ", " + num(i.dest) + ", " + d + ", " + vf(i.fs()) + ", " + vu_t(i) + ");";
	}
	case Op::VMAXbc: case Op::VMAXi: case Op::VMAX:
		return "ps2_vu_minmax(c, " + num(i.fd()) + ", " + num(i.dest) + ", " + vf(i.fs()) + ", " + vu_t(i) + ", 1);";
	case Op::VMINIbc: case Op::VMINIi: case Op::VMINI:
		return "ps2_vu_minmax(c, " + num(i.fd()) + ", " + num(i.dest) + ", " + vf(i.fs()) + ", " + vu_t(i) + ", 0);";
	case Op::VOPMULA: return "ps2_vu_opm(c, &c->vacc, " + num(i.fs()) + ", " + num(i.ft()) + ", 0);";
	case Op::VOPMSUB: return "ps2_vu_opm(c, " + vdst(i.fd()) + ", " + num(i.fs()) + ", " + num(i.ft()) + ", 1);";

	// --- VU0 macro: integer ---
	case Op::VIADD: return "ps2_vi_set(c, " + num(i.fd()) + ", c->vi[" + num(i.fs()) + "] + c->vi[" + num(i.ft()) + "]);";
	case Op::VISUB: return "ps2_vi_set(c, " + num(i.fd()) + ", c->vi[" + num(i.fs()) + "] - c->vi[" + num(i.ft()) + "]);";
	case Op::VIAND: return "ps2_vi_set(c, " + num(i.fd()) + ", c->vi[" + num(i.fs()) + "] & c->vi[" + num(i.ft()) + "]);";
	case Op::VIOR: return "ps2_vi_set(c, " + num(i.fd()) + ", c->vi[" + num(i.fs()) + "] | c->vi[" + num(i.ft()) + "]);";
	case Op::VIADDI: {
		int imm5 = (sa & 0x10) ? sa - 32 : sa;
		return "ps2_vi_set(c, " + num(i.ft()) + ", c->vi[" + num(i.fs()) + "] + (uint32_t)(" + num(imm5) + "));";
	}

	// --- VU0 macro: micro-program call ---
	case Op::VCALLMS:
		return "c->pc = " + pc + "; ps2_vu0_callms(c, " + pc + ", " + hex32(((i.word >> 6) & 0x7FFF) * 8) + ");";
	case Op::VCALLMSR:
		return "c->pc = " + pc + "; ps2_vu0_callms(c, " + pc + ", (ps2_vu0_ctrl_read(c, PS2_VI_CMSAR0) & 0xFFFFu) * 8u);";

	// --- VU0 macro: special2 ---
	case Op::VITOF0: case Op::VITOF4: case Op::VITOF12: case Op::VITOF15: {
		static const int f[4] = {0, 4, 12, 15};
		int k = static_cast<int>(i.op) - static_cast<int>(Op::VITOF0);
		return "ps2_vu_itof_op(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ", " + num(f[k]) + ");";
	}
	case Op::VFTOI0: case Op::VFTOI4: case Op::VFTOI12: case Op::VFTOI15: {
		static const int f[4] = {0, 4, 12, 15};
		int k = static_cast<int>(i.op) - static_cast<int>(Op::VFTOI0);
		return "ps2_vu_ftoi_op(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ", " + num(f[k]) + ");";
	}
	case Op::VABS: return "ps2_vu_abs(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ");";
	case Op::VMOVE: return "ps2_vu_move(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ");";
	case Op::VMR32: return "ps2_vu_mr32(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ");";
	case Op::VCLIPw: return "ps2_vu_clip(c, " + num(i.fs()) + ", " + num(i.ft()) + ");";
	case Op::VNOP: case Op::VWAITQ: return "";
	case Op::VLQI: return "ps2_vu_lq(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ", 1);";
	case Op::VLQD: return "ps2_vu_lq(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ", -1);";
	case Op::VSQI: return "ps2_vu_sq(c, " + num(i.dest) + ", " + num(i.fs()) + ", " + num(i.ft()) + ", 1);";
	case Op::VSQD: return "ps2_vu_sq(c, " + num(i.dest) + ", " + num(i.fs()) + ", " + num(i.ft()) + ", -1);";
	case Op::VDIV: {
		int fsf = (i.word >> 21) & 3, ftf = (i.word >> 23) & 3;
		return "ps2_vu_div(c, " + num(i.fs()) + ", " + num(fsf) + ", " + num(i.ft()) + ", " + num(ftf) + ");";
	}
	case Op::VSQRT: return "ps2_vu_sqrt(c, " + num(i.ft()) + ", " + num((i.word >> 23) & 3) + ");";
	case Op::VRSQRT: {
		int fsf = (i.word >> 21) & 3, ftf = (i.word >> 23) & 3;
		return "ps2_vu_rsqrt(c, " + num(i.fs()) + ", " + num(fsf) + ", " + num(i.ft()) + ", " + num(ftf) + ");";
	}
	case Op::VMTIR: return "ps2_vi_set(c, " + num(i.ft()) + ", c->vf[" + num(i.fs()) + "].uw[" + num((i.word >> 21) & 3) + "]);";
	case Op::VMFIR: return "ps2_vu_mfir(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ");";
	case Op::VILWR: return "ps2_vu_ilwr(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ");";
	case Op::VISWR: return "ps2_vu_iswr(c, " + num(i.dest) + ", " + num(i.ft()) + ", " + num(i.fs()) + ");";
	case Op::VRNEXT: return "ps2_vu_rnext(c, " + num(i.dest) + ", " + num(i.ft()) + ");";
	case Op::VRGET: return "ps2_vu_rget(c, " + num(i.dest) + ", " + num(i.ft()) + ");";
	case Op::VRINIT:
		return "c->vi[PS2_VI_R] = 0x3F800000u | (c->vf[" + num(i.fs()) + "].uw[" + num((i.word >> 21) & 3) + "] & 0x7FFFFFu);";
	case Op::VRXOR:
		return "c->vi[PS2_VI_R] = 0x3F800000u | ((c->vi[PS2_VI_R] ^ c->vf[" + num(i.fs()) + "].uw[" + num((i.word >> 21) & 3) + "]) & 0x7FFFFFu);";

	default:
		// Control-transfer instructions are handled by the function emitter.
		return unimpl();
	}
}

} // namespace recomp
