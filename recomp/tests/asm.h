// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal R5900 assembler for recompiler tests (encodings per the EE manual).
#pragma once

#include <cstdint>

namespace a {

enum : uint32_t {
	ZERO = 0, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7,
	S0, S1, S2, S3, S4, S5, S6, S7, T8, T9, K0, K1, GP, SP, FP, RA,
};

constexpr uint32_t R(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t fn)
{
	return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn;
}
constexpr uint32_t I(uint32_t op, uint32_t rs, uint32_t rt, int32_t imm)
{
	return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}
constexpr uint32_t MMI(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t fn) { return (0x1Cu << 26) | R(rs, rt, rd, sa, fn); }

// SPECIAL
constexpr uint32_t nop() { return 0; }
constexpr uint32_t sll(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x00); }
constexpr uint32_t srl(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x02); }
constexpr uint32_t sra(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x03); }
constexpr uint32_t sllv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(rs, rt, rd, 0, 0x04); }
constexpr uint32_t srlv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(rs, rt, rd, 0, 0x06); }
constexpr uint32_t srav(uint32_t rd, uint32_t rt, uint32_t rs) { return R(rs, rt, rd, 0, 0x07); }
constexpr uint32_t jr(uint32_t rs) { return R(rs, 0, 0, 0, 0x08); }
constexpr uint32_t jalr(uint32_t rd, uint32_t rs) { return R(rs, 0, rd, 0, 0x09); }
constexpr uint32_t movz(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x0A); }
constexpr uint32_t movn(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x0B); }
constexpr uint32_t syscall_(uint32_t code = 0) { return (code << 6) | 0x0C; }
constexpr uint32_t break_(uint32_t code = 0) { return (code << 6) | 0x0D; }
constexpr uint32_t sync_() { return 0x0F; }
constexpr uint32_t mfhi(uint32_t rd) { return R(0, 0, rd, 0, 0x10); }
constexpr uint32_t mthi(uint32_t rs) { return R(rs, 0, 0, 0, 0x11); }
constexpr uint32_t mflo(uint32_t rd) { return R(0, 0, rd, 0, 0x12); }
constexpr uint32_t mtlo(uint32_t rs) { return R(rs, 0, 0, 0, 0x13); }
constexpr uint32_t dsllv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(rs, rt, rd, 0, 0x14); }
constexpr uint32_t dsrlv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(rs, rt, rd, 0, 0x16); }
constexpr uint32_t dsrav(uint32_t rd, uint32_t rt, uint32_t rs) { return R(rs, rt, rd, 0, 0x17); }
constexpr uint32_t mult(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x18); }
constexpr uint32_t multu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x19); }
constexpr uint32_t div_(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x1A); }
constexpr uint32_t divu(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x1B); }
constexpr uint32_t add(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x20); }
constexpr uint32_t addu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x21); }
constexpr uint32_t sub(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x22); }
constexpr uint32_t subu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x23); }
constexpr uint32_t and_(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x24); }
constexpr uint32_t or_(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x25); }
constexpr uint32_t xor_(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x26); }
constexpr uint32_t nor(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x27); }
constexpr uint32_t mfsa(uint32_t rd) { return R(0, 0, rd, 0, 0x28); }
constexpr uint32_t mtsa(uint32_t rs) { return R(rs, 0, 0, 0, 0x29); }
constexpr uint32_t slt(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x2A); }
constexpr uint32_t sltu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x2B); }
constexpr uint32_t dadd(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x2C); }
constexpr uint32_t daddu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x2D); }
constexpr uint32_t dsub(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x2E); }
constexpr uint32_t dsubu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(rs, rt, rd, 0, 0x2F); }
constexpr uint32_t tge(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x30); }
constexpr uint32_t tgeu(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x31); }
constexpr uint32_t tlt(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x32); }
constexpr uint32_t tltu(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x33); }
constexpr uint32_t teq(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x34); }
constexpr uint32_t tne(uint32_t rs, uint32_t rt) { return R(rs, rt, 0, 0, 0x36); }
constexpr uint32_t dsll(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x38); }
constexpr uint32_t dsrl(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x3A); }
constexpr uint32_t dsra(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x3B); }
constexpr uint32_t dsll32(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x3C); }
constexpr uint32_t dsrl32(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x3E); }
constexpr uint32_t dsra32(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, rt, rd, sa, 0x3F); }

// REGIMM (offset in instructions relative to the delay slot)
constexpr uint32_t regimm(uint32_t rs, uint32_t sub, int32_t off) { return I(1, rs, sub, off); }
constexpr uint32_t bltz(uint32_t rs, int32_t off) { return regimm(rs, 0, off); }
constexpr uint32_t bgez(uint32_t rs, int32_t off) { return regimm(rs, 1, off); }
constexpr uint32_t bltzl(uint32_t rs, int32_t off) { return regimm(rs, 2, off); }
constexpr uint32_t bgezl(uint32_t rs, int32_t off) { return regimm(rs, 3, off); }
constexpr uint32_t tgei(uint32_t rs, int32_t imm) { return regimm(rs, 8, imm); }
constexpr uint32_t tgeiu(uint32_t rs, int32_t imm) { return regimm(rs, 9, imm); }
constexpr uint32_t tlti(uint32_t rs, int32_t imm) { return regimm(rs, 10, imm); }
constexpr uint32_t tltiu(uint32_t rs, int32_t imm) { return regimm(rs, 11, imm); }
constexpr uint32_t teqi(uint32_t rs, int32_t imm) { return regimm(rs, 12, imm); }
constexpr uint32_t tnei(uint32_t rs, int32_t imm) { return regimm(rs, 14, imm); }
constexpr uint32_t bltzal(uint32_t rs, int32_t off) { return regimm(rs, 16, off); }
constexpr uint32_t bgezal(uint32_t rs, int32_t off) { return regimm(rs, 17, off); }
constexpr uint32_t bltzall(uint32_t rs, int32_t off) { return regimm(rs, 18, off); }
constexpr uint32_t bgezall(uint32_t rs, int32_t off) { return regimm(rs, 19, off); }
constexpr uint32_t mtsab(uint32_t rs, int32_t imm) { return regimm(rs, 24, imm); }
constexpr uint32_t mtsah(uint32_t rs, int32_t imm) { return regimm(rs, 25, imm); }

// J-type (absolute target)
constexpr uint32_t j(uint32_t target) { return (2u << 26) | ((target >> 2) & 0x03FFFFFF); }
constexpr uint32_t jal(uint32_t target) { return (3u << 26) | ((target >> 2) & 0x03FFFFFF); }

// I-type
constexpr uint32_t beq(uint32_t rs, uint32_t rt, int32_t off) { return I(0x04, rs, rt, off); }
constexpr uint32_t bne(uint32_t rs, uint32_t rt, int32_t off) { return I(0x05, rs, rt, off); }
constexpr uint32_t blez(uint32_t rs, int32_t off) { return I(0x06, rs, 0, off); }
constexpr uint32_t bgtz(uint32_t rs, int32_t off) { return I(0x07, rs, 0, off); }
constexpr uint32_t addi(uint32_t rt, uint32_t rs, int32_t imm) { return I(0x08, rs, rt, imm); }
constexpr uint32_t addiu(uint32_t rt, uint32_t rs, int32_t imm) { return I(0x09, rs, rt, imm); }
constexpr uint32_t slti(uint32_t rt, uint32_t rs, int32_t imm) { return I(0x0A, rs, rt, imm); }
constexpr uint32_t sltiu(uint32_t rt, uint32_t rs, int32_t imm) { return I(0x0B, rs, rt, imm); }
constexpr uint32_t andi(uint32_t rt, uint32_t rs, uint32_t imm) { return I(0x0C, rs, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t ori(uint32_t rt, uint32_t rs, uint32_t imm) { return I(0x0D, rs, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t xori(uint32_t rt, uint32_t rs, uint32_t imm) { return I(0x0E, rs, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t lui(uint32_t rt, uint32_t imm) { return I(0x0F, 0, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t beql(uint32_t rs, uint32_t rt, int32_t off) { return I(0x14, rs, rt, off); }
constexpr uint32_t bnel(uint32_t rs, uint32_t rt, int32_t off) { return I(0x15, rs, rt, off); }
constexpr uint32_t blezl(uint32_t rs, int32_t off) { return I(0x16, rs, 0, off); }
constexpr uint32_t bgtzl(uint32_t rs, int32_t off) { return I(0x17, rs, 0, off); }
constexpr uint32_t daddi(uint32_t rt, uint32_t rs, int32_t imm) { return I(0x18, rs, rt, imm); }
constexpr uint32_t daddiu(uint32_t rt, uint32_t rs, int32_t imm) { return I(0x19, rs, rt, imm); }
constexpr uint32_t ldl(uint32_t rt, int32_t off, uint32_t base) { return I(0x1A, base, rt, off); }
constexpr uint32_t ldr(uint32_t rt, int32_t off, uint32_t base) { return I(0x1B, base, rt, off); }
constexpr uint32_t lq(uint32_t rt, int32_t off, uint32_t base) { return I(0x1E, base, rt, off); }
constexpr uint32_t sq(uint32_t rt, int32_t off, uint32_t base) { return I(0x1F, base, rt, off); }
constexpr uint32_t lb(uint32_t rt, int32_t off, uint32_t base) { return I(0x20, base, rt, off); }
constexpr uint32_t lh(uint32_t rt, int32_t off, uint32_t base) { return I(0x21, base, rt, off); }
constexpr uint32_t lwl(uint32_t rt, int32_t off, uint32_t base) { return I(0x22, base, rt, off); }
constexpr uint32_t lw(uint32_t rt, int32_t off, uint32_t base) { return I(0x23, base, rt, off); }
constexpr uint32_t lbu(uint32_t rt, int32_t off, uint32_t base) { return I(0x24, base, rt, off); }
constexpr uint32_t lhu(uint32_t rt, int32_t off, uint32_t base) { return I(0x25, base, rt, off); }
constexpr uint32_t lwr(uint32_t rt, int32_t off, uint32_t base) { return I(0x26, base, rt, off); }
constexpr uint32_t lwu(uint32_t rt, int32_t off, uint32_t base) { return I(0x27, base, rt, off); }
constexpr uint32_t sb(uint32_t rt, int32_t off, uint32_t base) { return I(0x28, base, rt, off); }
constexpr uint32_t sh(uint32_t rt, int32_t off, uint32_t base) { return I(0x29, base, rt, off); }
constexpr uint32_t swl(uint32_t rt, int32_t off, uint32_t base) { return I(0x2A, base, rt, off); }
constexpr uint32_t sw(uint32_t rt, int32_t off, uint32_t base) { return I(0x2B, base, rt, off); }
constexpr uint32_t sdl(uint32_t rt, int32_t off, uint32_t base) { return I(0x2C, base, rt, off); }
constexpr uint32_t sdr(uint32_t rt, int32_t off, uint32_t base) { return I(0x2D, base, rt, off); }
constexpr uint32_t swr(uint32_t rt, int32_t off, uint32_t base) { return I(0x2E, base, rt, off); }
constexpr uint32_t cache(uint32_t op, int32_t off, uint32_t base) { return I(0x2F, base, op, off); }
constexpr uint32_t lwc1(uint32_t ft, int32_t off, uint32_t base) { return I(0x31, base, ft, off); }
constexpr uint32_t pref(uint32_t hint, int32_t off, uint32_t base) { return I(0x33, base, hint, off); }
constexpr uint32_t lqc2(uint32_t ft, int32_t off, uint32_t base) { return I(0x36, base, ft, off); }
constexpr uint32_t ld(uint32_t rt, int32_t off, uint32_t base) { return I(0x37, base, rt, off); }
constexpr uint32_t swc1(uint32_t ft, int32_t off, uint32_t base) { return I(0x39, base, ft, off); }
constexpr uint32_t sqc2(uint32_t ft, int32_t off, uint32_t base) { return I(0x3E, base, ft, off); }
constexpr uint32_t sd(uint32_t rt, int32_t off, uint32_t base) { return I(0x3F, base, rt, off); }

// COP0
constexpr uint32_t mfc0(uint32_t rt, uint32_t rd, uint32_t sel = 0) { return (0x10u << 26) | (0u << 21) | (rt << 16) | (rd << 11) | sel; }
constexpr uint32_t mtc0(uint32_t rt, uint32_t rd, uint32_t sel = 0) { return (0x10u << 26) | (4u << 21) | (rt << 16) | (rd << 11) | sel; }
constexpr uint32_t bc0f(int32_t off) { return I(0x10, 8, 0, off); }
constexpr uint32_t bc0t(int32_t off) { return I(0x10, 8, 1, off); }
constexpr uint32_t bc0fl(int32_t off) { return I(0x10, 8, 2, off); }
constexpr uint32_t bc0tl(int32_t off) { return I(0x10, 8, 3, off); }
constexpr uint32_t c0op(uint32_t fn) { return (0x10u << 26) | (0x10u << 21) | fn; }
constexpr uint32_t ei() { return c0op(0x38); }
constexpr uint32_t di() { return c0op(0x39); }
constexpr uint32_t eret() { return c0op(0x18); }
constexpr uint32_t tlbwi() { return c0op(0x02); }

// MMI
constexpr uint32_t madd(uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, 0, 0x00); }
constexpr uint32_t maddu(uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, 0, 0x01); }
constexpr uint32_t plzcw(uint32_t rd, uint32_t rs) { return MMI(rs, 0, rd, 0, 0x04); }
constexpr uint32_t mfhi1(uint32_t rd) { return MMI(0, 0, rd, 0, 0x10); }
constexpr uint32_t mthi1(uint32_t rs) { return MMI(rs, 0, 0, 0, 0x11); }
constexpr uint32_t mflo1(uint32_t rd) { return MMI(0, 0, rd, 0, 0x12); }
constexpr uint32_t mtlo1(uint32_t rs) { return MMI(rs, 0, 0, 0, 0x13); }
constexpr uint32_t mult1(uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, 0, 0x18); }
constexpr uint32_t multu1(uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, 0, 0x19); }
constexpr uint32_t div1(uint32_t rs, uint32_t rt) { return MMI(rs, rt, 0, 0, 0x1A); }
constexpr uint32_t divu1(uint32_t rs, uint32_t rt) { return MMI(rs, rt, 0, 0, 0x1B); }
constexpr uint32_t madd1(uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, 0, 0x20); }
constexpr uint32_t maddu1(uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, 0, 0x21); }
constexpr uint32_t pmfhl(uint32_t rd, uint32_t fmt) { return MMI(0, 0, rd, fmt, 0x30); }
constexpr uint32_t pmthl(uint32_t rs, uint32_t fmt) { return MMI(rs, 0, 0, fmt, 0x31); }
constexpr uint32_t psllh(uint32_t rd, uint32_t rt, uint32_t sa) { return MMI(0, rt, rd, sa, 0x34); }
constexpr uint32_t psrlh(uint32_t rd, uint32_t rt, uint32_t sa) { return MMI(0, rt, rd, sa, 0x36); }
constexpr uint32_t psrah(uint32_t rd, uint32_t rt, uint32_t sa) { return MMI(0, rt, rd, sa, 0x37); }
constexpr uint32_t psllw(uint32_t rd, uint32_t rt, uint32_t sa) { return MMI(0, rt, rd, sa, 0x3C); }
constexpr uint32_t psrlw(uint32_t rd, uint32_t rt, uint32_t sa) { return MMI(0, rt, rd, sa, 0x3E); }
constexpr uint32_t psraw(uint32_t rd, uint32_t rt, uint32_t sa) { return MMI(0, rt, rd, sa, 0x3F); }
// MMI0..3 sub-ops: mmiN(sub, rd, rs, rt)
constexpr uint32_t mmi0(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, sub, 0x08); }
constexpr uint32_t mmi2(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, sub, 0x09); }
constexpr uint32_t mmi1(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, sub, 0x28); }
constexpr uint32_t mmi3(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return MMI(rs, rt, rd, sub, 0x29); }

// COP1
constexpr uint32_t cop1(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t fn) { return (0x11u << 26) | R(rs, rt, rd, sa, fn); }
constexpr uint32_t mfc1(uint32_t rt, uint32_t fs) { return cop1(0, rt, fs, 0, 0); }
constexpr uint32_t cfc1(uint32_t rt, uint32_t fs) { return cop1(2, rt, fs, 0, 0); }
constexpr uint32_t mtc1(uint32_t rt, uint32_t fs) { return cop1(4, rt, fs, 0, 0); }
constexpr uint32_t ctc1(uint32_t rt, uint32_t fs) { return cop1(6, rt, fs, 0, 0); }
constexpr uint32_t fs_(uint32_t fn, uint32_t fd, uint32_t fs, uint32_t ft) { return cop1(16, ft, fs, fd, fn); }
constexpr uint32_t cvt_s_w(uint32_t fd, uint32_t fs) { return cop1(20, 0, fs, fd, 0x20); }
constexpr uint32_t bc1f(int32_t off) { return I(0x11, 8, 0, off); }
constexpr uint32_t bc1t(int32_t off) { return I(0x11, 8, 1, off); }
constexpr uint32_t bc1fl(int32_t off) { return I(0x11, 8, 2, off); }
constexpr uint32_t bc1tl(int32_t off) { return I(0x11, 8, 3, off); }

// COP2
constexpr uint32_t cop2x(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t low) { return (0x12u << 26) | (rs << 21) | (rt << 16) | (rd << 11) | low; }
constexpr uint32_t qmfc2(uint32_t rt, uint32_t fs, bool i = false) { return cop2x(1, rt, fs, i); }
constexpr uint32_t cfc2(uint32_t rt, uint32_t id, bool i = false) { return cop2x(2, rt, id, i); }
constexpr uint32_t qmtc2(uint32_t rt, uint32_t fs, bool i = false) { return cop2x(5, rt, fs, i); }
constexpr uint32_t ctc2(uint32_t rt, uint32_t id, bool i = false) { return cop2x(6, rt, id, i); }
constexpr uint32_t bc2f(int32_t off) { return I(0x12, 8, 0, off); }
constexpr uint32_t bc2t(int32_t off) { return I(0x12, 8, 1, off); }
constexpr uint32_t bc2fl(int32_t off) { return I(0x12, 8, 2, off); }
constexpr uint32_t bc2tl(int32_t off) { return I(0x12, 8, 3, off); }
enum : uint32_t { X = 8, Y = 4, Z = 2, W = 1, XYZW = 15, XYZ = 14 };
// special1: v1(fn, dest, fd, fs, ft)
constexpr uint32_t v1(uint32_t fn, uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft)
{
	return (0x12u << 26) | (1u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn;
}
// special2: v2(index 0..127, dest, fs, ft)
constexpr uint32_t v2(uint32_t idx, uint32_t dest, uint32_t fs, uint32_t ft)
{
	return (0x12u << 26) | (1u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}
constexpr uint32_t vcallms(uint32_t imm15) { return (0x12u << 26) | (1u << 25) | (imm15 << 6) | 0x38; }
constexpr uint32_t vcallmsr() { return (0x12u << 26) | (1u << 25) | 0x39; }

} // namespace a
