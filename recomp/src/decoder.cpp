// SPDX-License-Identifier: GPL-3.0-or-later
// R5900 decoder. Opcode tables follow the EE Core Instruction Set Manual
// (cross-checked against PCSX2's R5900OpcodeTables.cpp).
#include "decoder.h"

#include <cstdio>

namespace recomp {

namespace {

struct OpInfo {
	const char *mnemonic;
	OpClass cls;
};

const OpInfo kOpInfo[] = {
	{"(invalid)", OpClass::INVALID},
#define X(e, m, c) {m, OpClass::c},
#include "ops.def"
#undef X
};

static_assert(sizeof(kOpInfo) / sizeof(kOpInfo[0]) == static_cast<size_t>(Op::COUNT));

constexpr Op kSpecial[64] = {
	Op::SLL,     Op::INVALID, Op::SRL,     Op::SRA,     Op::SLLV,    Op::INVALID, Op::SRLV,    Op::SRAV,
	Op::JR,      Op::JALR,    Op::MOVZ,    Op::MOVN,    Op::SYSCALL, Op::BREAK,   Op::INVALID, Op::SYNC,
	Op::MFHI,    Op::MTHI,    Op::MFLO,    Op::MTLO,    Op::DSLLV,   Op::INVALID, Op::DSRLV,   Op::DSRAV,
	Op::MULT,    Op::MULTU,   Op::DIV,     Op::DIVU,    Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::ADD,     Op::ADDU,    Op::SUB,     Op::SUBU,    Op::AND,     Op::OR,      Op::XOR,     Op::NOR,
	Op::MFSA,    Op::MTSA,    Op::SLT,     Op::SLTU,    Op::DADD,    Op::DADDU,   Op::DSUB,    Op::DSUBU,
	Op::TGE,     Op::TGEU,    Op::TLT,     Op::TLTU,    Op::TEQ,     Op::INVALID, Op::TNE,     Op::INVALID,
	Op::DSLL,    Op::INVALID, Op::DSRL,    Op::DSRA,    Op::DSLL32,  Op::INVALID, Op::DSRL32,  Op::DSRA32,
};

constexpr Op kRegimm[32] = {
	Op::BLTZ,   Op::BGEZ,   Op::BLTZL,   Op::BGEZL,   Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::TGEI,   Op::TGEIU,  Op::TLTI,    Op::TLTIU,   Op::TEQI,    Op::INVALID, Op::TNEI,    Op::INVALID,
	Op::BLTZAL, Op::BGEZAL, Op::BLTZALL, Op::BGEZALL, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::MTSAB,  Op::MTSAH,  Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
};

// MMI funct table; 0xFE/0xFD/0xFC/0xFB mark the MMI0..3 sub-tables.
constexpr Op kMmi[64] = {
	Op::MADD,    Op::MADDU,   Op::INVALID, Op::INVALID, Op::PLZCW,   Op::INVALID, Op::INVALID, Op::INVALID,
	Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::MFHI1,   Op::MTHI1,   Op::MFLO1,   Op::MTLO1,   Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::MULT1,   Op::MULTU1,  Op::DIV1,    Op::DIVU1,   Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::MADD1,   Op::MADDU1,  Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::PMFHL,   Op::PMTHL,   Op::INVALID, Op::INVALID, Op::PSLLH,   Op::INVALID, Op::PSRLH,   Op::PSRAH,
	Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::PSLLW,   Op::INVALID, Op::PSRLW,   Op::PSRAW,
};

constexpr Op kMmi0[32] = {
	Op::PADDW,   Op::PSUBW,   Op::PCGTW,   Op::PMAXW,   Op::PADDH,   Op::PSUBH,   Op::PCGTH,   Op::PMAXH,
	Op::PADDB,   Op::PSUBB,   Op::PCGTB,   Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::PADDSW,  Op::PSUBSW,  Op::PEXTLW,  Op::PPACW,   Op::PADDSH,  Op::PSUBSH,  Op::PEXTLH,  Op::PPACH,
	Op::PADDSB,  Op::PSUBSB,  Op::PEXTLB,  Op::PPACB,   Op::INVALID, Op::INVALID, Op::PEXT5,   Op::PPAC5,
};

constexpr Op kMmi1[32] = {
	Op::INVALID, Op::PABSW,   Op::PCEQW,   Op::PMINW,   Op::PADSBH,  Op::PABSH,   Op::PCEQH,   Op::PMINH,
	Op::INVALID, Op::INVALID, Op::PCEQB,   Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::PADDUW,  Op::PSUBUW,  Op::PEXTUW,  Op::INVALID, Op::PADDUH,  Op::PSUBUH,  Op::PEXTUH,  Op::INVALID,
	Op::PADDUB,  Op::PSUBUB,  Op::PEXTUB,  Op::QFSRV,   Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
};

constexpr Op kMmi2[32] = {
	Op::PMADDW,  Op::INVALID, Op::PSLLVW,  Op::PSRLVW,  Op::PMSUBW,  Op::INVALID, Op::INVALID, Op::INVALID,
	Op::PMFHI,   Op::PMFLO,   Op::PINTH,   Op::INVALID, Op::PMULTW,  Op::PDIVW,   Op::PCPYLD,  Op::INVALID,
	Op::PMADDH,  Op::PHMADH,  Op::PAND,    Op::PXOR,    Op::PMSUBH,  Op::PHMSBH,  Op::INVALID, Op::INVALID,
	Op::INVALID, Op::INVALID, Op::PEXEH,   Op::PREVH,   Op::PMULTH,  Op::PDIVBW,  Op::PEXEW,   Op::PROT3W,
};

constexpr Op kMmi3[32] = {
	Op::PMADDUW, Op::INVALID, Op::INVALID, Op::PSRAVW,  Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::PMTHI,   Op::PMTLO,   Op::PINTEH,  Op::INVALID, Op::PMULTUW, Op::PDIVUW,  Op::PCPYUD,  Op::INVALID,
	Op::INVALID, Op::INVALID, Op::POR,     Op::PNOR,    Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::INVALID, Op::INVALID, Op::PEXCH,   Op::PCPYH,   Op::INVALID, Op::INVALID, Op::PEXCW,   Op::INVALID,
};

constexpr Op kCop1S[64] = {
	Op::ADD_S,   Op::SUB_S,   Op::MUL_S,   Op::DIV_S,   Op::SQRT_S,  Op::ABS_S,   Op::MOV_S,   Op::NEG_S,
	Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::RSQRT_S, Op::INVALID,
	Op::ADDA_S,  Op::SUBA_S,  Op::MULA_S,  Op::INVALID, Op::MADD_S,  Op::MSUB_S,  Op::MADDA_S, Op::MSUBA_S,
	Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::CVT_W_S, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::MAX_S,   Op::MIN_S,   Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	Op::C_F_S,   Op::INVALID, Op::C_EQ_S,  Op::INVALID, Op::C_LT_S,  Op::INVALID, Op::C_LE_S,  Op::INVALID,
	Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
};

constexpr Op kVuSpecial1[64] = {
	Op::VADDbc,  Op::VADDbc,  Op::VADDbc,  Op::VADDbc,  Op::VSUBbc,  Op::VSUBbc,  Op::VSUBbc,  Op::VSUBbc,
	Op::VMADDbc, Op::VMADDbc, Op::VMADDbc, Op::VMADDbc, Op::VMSUBbc, Op::VMSUBbc, Op::VMSUBbc, Op::VMSUBbc,
	Op::VMAXbc,  Op::VMAXbc,  Op::VMAXbc,  Op::VMAXbc,  Op::VMINIbc, Op::VMINIbc, Op::VMINIbc, Op::VMINIbc,
	Op::VMULbc,  Op::VMULbc,  Op::VMULbc,  Op::VMULbc,  Op::VMULq,   Op::VMAXi,   Op::VMULi,   Op::VMINIi,
	Op::VADDq,   Op::VMADDq,  Op::VADDi,   Op::VMADDi,  Op::VSUBq,   Op::VMSUBq,  Op::VSUBi,   Op::VMSUBi,
	Op::VADD,    Op::VMADD,   Op::VMUL,    Op::VMAX,    Op::VSUB,    Op::VMSUB,   Op::VOPMSUB, Op::VMINI,
	Op::VIADD,   Op::VISUB,   Op::VIADDI,  Op::INVALID, Op::VIAND,   Op::VIOR,    Op::INVALID, Op::INVALID,
	Op::VCALLMS, Op::VCALLMSR, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
};

constexpr Op kVuSpecial2[128] = {
	Op::VADDAbc, Op::VADDAbc, Op::VADDAbc, Op::VADDAbc, Op::VSUBAbc, Op::VSUBAbc, Op::VSUBAbc, Op::VSUBAbc,
	Op::VMADDAbc, Op::VMADDAbc, Op::VMADDAbc, Op::VMADDAbc, Op::VMSUBAbc, Op::VMSUBAbc, Op::VMSUBAbc, Op::VMSUBAbc,
	Op::VITOF0,  Op::VITOF4,  Op::VITOF12, Op::VITOF15, Op::VFTOI0,  Op::VFTOI4,  Op::VFTOI12, Op::VFTOI15,
	Op::VMULAbc, Op::VMULAbc, Op::VMULAbc, Op::VMULAbc, Op::VMULAq,  Op::VABS,    Op::VMULAi,  Op::VCLIPw,
	Op::VADDAq,  Op::VMADDAq, Op::VADDAi,  Op::VMADDAi, Op::VSUBAq,  Op::VMSUBAq, Op::VSUBAi,  Op::VMSUBAi,
	Op::VADDA,   Op::VMADDA,  Op::VMULA,   Op::INVALID, Op::VSUBA,   Op::VMSUBA,  Op::VOPMULA, Op::VNOP,
	Op::VMOVE,   Op::VMR32,   Op::INVALID, Op::INVALID, Op::VLQI,    Op::VSQI,    Op::VLQD,    Op::VSQD,
	Op::VDIV,    Op::VSQRT,   Op::VRSQRT,  Op::VWAITQ,  Op::VMTIR,   Op::VMFIR,   Op::VILWR,   Op::VISWR,
	Op::VRNEXT,  Op::VRGET,   Op::VRINIT,  Op::VRXOR,   // 0x44.. invalid
};

} // namespace

const char *op_mnemonic(Op op) { return kOpInfo[static_cast<size_t>(op)].mnemonic; }
OpClass op_class(Op op) { return kOpInfo[static_cast<size_t>(op)].cls; }

const char *op_class_name(OpClass c)
{
	switch (c) {
	case OpClass::INVALID: return "invalid";
	case OpClass::CPU: return "cpu";
	case OpClass::BR: return "branch";
	case OpClass::MMI: return "mmi";
	case OpClass::FPU: return "fpu";
	case OpClass::VU0: return "vu0";
	case OpClass::COP0: return "cop0";
	case OpClass::SYS: return "sys";
	}
	return "?";
}

Insn decode(uint32_t pc, uint32_t w)
{
	Insn i;
	i.pc = pc;
	i.word = w;
	i.rs = (w >> 21) & 31;
	i.rt = (w >> 16) & 31;
	i.rd = (w >> 11) & 31;
	i.sa = (w >> 6) & 31;
	i.imm = static_cast<int16_t>(w & 0xFFFF);
	i.uimm = w & 0xFFFF;
	const uint32_t funct = w & 63;
	const uint32_t btarget = pc + 4 + (static_cast<uint32_t>(i.imm) << 2);

	auto branch = [&](Op op, bool likely) {
		i.op = op;
		i.flow = Flow::Branch;
		i.likely = likely;
		i.target = btarget;
	};

	switch (w >> 26) {
	case 0x00: // SPECIAL
		i.op = kSpecial[funct];
		if (i.op == Op::JR) i.flow = Flow::JumpReg;
		if (i.op == Op::JALR) i.flow = Flow::CallReg;
		break;
	case 0x01: // REGIMM
		i.op = kRegimm[i.rt];
		switch (i.op) {
		case Op::BLTZ: case Op::BGEZ: branch(i.op, false); break;
		case Op::BLTZL: case Op::BGEZL: branch(i.op, true); break;
		case Op::BLTZAL: case Op::BGEZAL:
			i.flow = Flow::Call; i.target = btarget; break;
		case Op::BLTZALL: case Op::BGEZALL:
			i.flow = Flow::Call; i.target = btarget; i.likely = true; break;
		default: break;
		}
		break;
	case 0x02: i.op = Op::J; i.flow = Flow::Jump; i.target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
	case 0x03: i.op = Op::JAL; i.flow = Flow::Call; i.target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
	case 0x04: branch(Op::BEQ, false); break;
	case 0x05: branch(Op::BNE, false); break;
	case 0x06: branch(Op::BLEZ, false); break;
	case 0x07: branch(Op::BGTZ, false); break;
	case 0x08: i.op = Op::ADDI; break;
	case 0x09: i.op = Op::ADDIU; break;
	case 0x0A: i.op = Op::SLTI; break;
	case 0x0B: i.op = Op::SLTIU; break;
	case 0x0C: i.op = Op::ANDI; break;
	case 0x0D: i.op = Op::ORI; break;
	case 0x0E: i.op = Op::XORI; break;
	case 0x0F: i.op = Op::LUI; break;
	case 0x10: // COP0
		switch (i.rs) {
		case 0x00: i.op = Op::MFC0; break;
		case 0x04: i.op = Op::MTC0; break;
		case 0x08:
			switch (i.rt) {
			case 0: branch(Op::BC0F, false); break;
			case 1: branch(Op::BC0T, false); break;
			case 2: branch(Op::BC0F, true); i.op = Op::BC0FL; break;
			case 3: branch(Op::BC0T, true); i.op = Op::BC0TL; break;
			default: break;
			}
			break;
		case 0x10:
			switch (funct) {
			case 0x01: i.op = Op::TLBR; break;
			case 0x02: i.op = Op::TLBWI; break;
			case 0x06: i.op = Op::TLBWR; break;
			case 0x08: i.op = Op::TLBP; break;
			case 0x18: i.op = Op::ERET; break;
			case 0x38: i.op = Op::EI; break;
			case 0x39: i.op = Op::DI; break;
			default: break;
			}
			break;
		default: break;
		}
		break;
	case 0x11: // COP1
		switch (i.rs) {
		case 0x00: i.op = Op::MFC1; break;
		case 0x02: i.op = Op::CFC1; break;
		case 0x04: i.op = Op::MTC1; break;
		case 0x06: i.op = Op::CTC1; break;
		case 0x08:
			switch (i.rt) {
			case 0: branch(Op::BC1F, false); break;
			case 1: branch(Op::BC1T, false); break;
			case 2: branch(Op::BC1FL, true); break;
			case 3: branch(Op::BC1TL, true); break;
			default: break;
			}
			break;
		case 0x10: i.op = kCop1S[funct]; break;
		case 0x14: if (funct == 0x20) i.op = Op::CVT_S_W; break;
		default: break;
		}
		break;
	case 0x12: // COP2
		i.dest = (w >> 21) & 15;
		if (i.rs & 0x10) {
			if ((funct & 0x3C) == 0x3C) {
				i.op = kVuSpecial2[(w & 3) | ((w >> 4) & 0x7C)];
			} else {
				i.op = kVuSpecial1[funct];
			}
			i.bc = w & 3;
		} else {
			switch (i.rs) {
			case 0x01: i.op = Op::QMFC2; i.interlock = w & 1; break;
			case 0x02: i.op = Op::CFC2; i.interlock = w & 1; break;
			case 0x05: i.op = Op::QMTC2; i.interlock = w & 1; break;
			case 0x06: i.op = Op::CTC2; i.interlock = w & 1; break;
			case 0x08:
				switch (i.rt) {
				case 0: branch(Op::BC2F, false); break;
				case 1: branch(Op::BC2T, false); break;
				case 2: branch(Op::BC2FL, true); break;
				case 3: branch(Op::BC2TL, true); break;
				default: break;
				}
				break;
			default: break;
			}
		}
		break;
	case 0x14: branch(Op::BEQL, true); break;
	case 0x15: branch(Op::BNEL, true); break;
	case 0x16: branch(Op::BLEZL, true); break;
	case 0x17: branch(Op::BGTZL, true); break;
	case 0x18: i.op = Op::DADDI; break;
	case 0x19: i.op = Op::DADDIU; break;
	case 0x1A: i.op = Op::LDL; break;
	case 0x1B: i.op = Op::LDR; break;
	case 0x1C: // MMI
		switch (funct) {
		case 0x08: i.op = kMmi0[i.sa]; break;
		case 0x09: i.op = kMmi2[i.sa]; break;
		case 0x28: i.op = kMmi1[i.sa]; break;
		case 0x29: i.op = kMmi3[i.sa]; break;
		default: i.op = kMmi[funct]; break;
		}
		break;
	case 0x1E: i.op = Op::LQ; break;
	case 0x1F: i.op = Op::SQ; break;
	case 0x20: i.op = Op::LB; break;
	case 0x21: i.op = Op::LH; break;
	case 0x22: i.op = Op::LWL; break;
	case 0x23: i.op = Op::LW; break;
	case 0x24: i.op = Op::LBU; break;
	case 0x25: i.op = Op::LHU; break;
	case 0x26: i.op = Op::LWR; break;
	case 0x27: i.op = Op::LWU; break;
	case 0x28: i.op = Op::SB; break;
	case 0x29: i.op = Op::SH; break;
	case 0x2A: i.op = Op::SWL; break;
	case 0x2B: i.op = Op::SW; break;
	case 0x2C: i.op = Op::SDL; break;
	case 0x2D: i.op = Op::SDR; break;
	case 0x2E: i.op = Op::SWR; break;
	case 0x2F: i.op = Op::CACHE; break;
	case 0x31: i.op = Op::LWC1; break;
	case 0x33: i.op = Op::PREF; break;
	case 0x36: i.op = Op::LQC2; break;
	case 0x37: i.op = Op::LD; break;
	case 0x39: i.op = Op::SWC1; break;
	case 0x3E: i.op = Op::SQC2; break;
	case 0x3F: i.op = Op::SD; break;
	default: break;
	}
	if (i.op == Op::INVALID) {
		i.flow = Flow::None;
		i.likely = false;
	}
	return i;
}

std::string disasm(const Insn &i)
{
	char buf[96];
	const char *m = op_mnemonic(i.op);
	switch (i.flow) {
	case Flow::Branch:
	case Flow::Jump:
	case Flow::Call:
		std::snprintf(buf, sizeof buf, "%s r%u, r%u, 0x%08x", m, i.rs, i.rt, i.target);
		break;
	case Flow::JumpReg:
	case Flow::CallReg:
		std::snprintf(buf, sizeof buf, "%s r%u (rd r%u)", m, i.rs, i.rd);
		break;
	default:
		std::snprintf(buf, sizeof buf, "%s rs=%u rt=%u rd=%u sa=%u imm=%d", m, i.rs, i.rt, i.rd, i.sa, i.imm);
		break;
	}
	return buf;
}

} // namespace recomp
