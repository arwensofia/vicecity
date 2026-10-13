/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ps2_ops.h -- R5900 instruction semantics used by recompiled code.
 *
 * Included by every file in recomp_out/. Everything here is static inline and
 * operates on the ps2_cpu context defined in runtime/include/ps2_abi.h.
 * Register-index parameters are compile-time constants at every call site, so
 * after inlining each helper collapses to straight-line code.
 *
 * Reference semantics: EE Core Instruction Set Manual; corner cases (division
 * by zero, saturation, LWR sign-extension, FPU/VU clamping) match PCSX2's
 * interpreter, except where noted.
 *
 * Build recompiled code with: -std=c11 -ffp-contract=off -fno-math-errno
 *   -fwrapv is not required (all arithmetic on GPRs is done unsigned).
 */
#ifndef PS2_OPS_H
#define PS2_OPS_H

#include "ps2_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

/* ------------------------------------------------------------------------ */
/* Register access shorthands used by emitted code ("c" is the context).     */
/* ------------------------------------------------------------------------ */
#define GPR_U64(n) (c->r[n].ud[0])
#define GPR_S64(n) (c->r[n].sd[0])
#define GPR_U32(n) (c->r[n].uw[0])
#define GPR_S32(n) (c->r[n].sw[0])
#define SEXT32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))
#define FPR_U(n) (c->f[n].u)

/* Every recompiled function. Weak so the runtime can override (see ps2_abi.h). */
#define PS2_FN(name) __attribute__((weak)) void name(ps2_cpu *c)

/* Tail call to another recompiled function (branch/jump/fall-through leaving the
 * current function). musttail guarantees no host stack growth for loops that
 * span function fragments. */
#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(musttail)
#define PS2_TAILCALL(fn) __attribute__((musttail)) return fn(c)
#endif
#endif
#ifndef PS2_TAILCALL
#define PS2_TAILCALL(fn) do { fn(c); return; } while (0)
#endif

static inline float ps2_bits_f(uint32_t u)
{
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static inline uint32_t ps2_f_bits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

/* ------------------------------------------------------------------------ */
/* Multiply / divide. pipe 0 = HI/LO, pipe 1 = HI1/LO1.                      */
/* ------------------------------------------------------------------------ */

static inline void ps2_op_mult(ps2_cpu *c, int rd, int rs, int rt, int pipe)
{
	int64_t p = (int64_t)GPR_S32(rs) * (int64_t)GPR_S32(rt);
	c->lo.ud[pipe] = SEXT32((uint64_t)p);
	c->hi.ud[pipe] = SEXT32((uint64_t)p >> 32);
	if (rd) GPR_U64(rd) = c->lo.ud[pipe];
}

static inline void ps2_op_multu(ps2_cpu *c, int rd, int rs, int rt, int pipe)
{
	uint64_t p = (uint64_t)GPR_U32(rs) * (uint64_t)GPR_U32(rt);
	c->lo.ud[pipe] = SEXT32(p);
	c->hi.ud[pipe] = SEXT32(p >> 32);
	if (rd) GPR_U64(rd) = c->lo.ud[pipe];
}

static inline void ps2_op_madd(ps2_cpu *c, int rd, int rs, int rt, int pipe)
{
	uint64_t acc = ((uint64_t)c->hi.uw[pipe * 2] << 32) | c->lo.uw[pipe * 2];
	uint64_t p = (uint64_t)((int64_t)GPR_S32(rs) * (int64_t)GPR_S32(rt));
	uint64_t r = acc + p;
	c->lo.ud[pipe] = SEXT32(r);
	c->hi.ud[pipe] = SEXT32(r >> 32);
	if (rd) GPR_U64(rd) = c->lo.ud[pipe];
}

static inline void ps2_op_maddu(ps2_cpu *c, int rd, int rs, int rt, int pipe)
{
	uint64_t acc = ((uint64_t)c->hi.uw[pipe * 2] << 32) | c->lo.uw[pipe * 2];
	uint64_t r = acc + (uint64_t)GPR_U32(rs) * (uint64_t)GPR_U32(rt);
	c->lo.ud[pipe] = SEXT32(r);
	c->hi.ud[pipe] = SEXT32(r >> 32);
	if (rd) GPR_U64(rd) = c->lo.ud[pipe];
}

/* 32-bit signed division with the EE's results for /0 and INT_MIN/-1. */
static inline void ps2_div32(int32_t n, int32_t d, uint64_t *lo, uint64_t *hi)
{
	if ((uint32_t)n == 0x80000000u && d == -1) {
		*lo = SEXT32(0x80000000u);
		*hi = 0;
	} else if (d != 0) {
		*lo = SEXT32(n / d);
		*hi = SEXT32(n % d);
	} else {
		*lo = n < 0 ? 1 : SEXT32(0xFFFFFFFFu);
		*hi = SEXT32(n);
	}
}

static inline void ps2_divu32(uint32_t n, uint32_t d, uint64_t *lo, uint64_t *hi)
{
	if (d != 0) {
		*lo = SEXT32(n / d);
		*hi = SEXT32(n % d);
	} else {
		*lo = SEXT32(0xFFFFFFFFu);
		*hi = SEXT32(n);
	}
}

static inline void ps2_op_div(ps2_cpu *c, int rs, int rt, int pipe)
{
	ps2_div32(GPR_S32(rs), GPR_S32(rt), &c->lo.ud[pipe], &c->hi.ud[pipe]);
}

static inline void ps2_op_divu(ps2_cpu *c, int rs, int rt, int pipe)
{
	ps2_divu32(GPR_U32(rs), GPR_U32(rt), &c->lo.ud[pipe], &c->hi.ud[pipe]);
}

/* ------------------------------------------------------------------------ */
/* Unaligned loads/stores (little-endian EE).                                */
/* ------------------------------------------------------------------------ */

static inline void ps2_op_lwl(ps2_cpu *c, int rt, uint32_t addr)
{
	static const uint32_t mask[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0};
	unsigned sh = addr & 3;
	uint32_t mem = ps2_read32(c, addr & ~3u);
	if (!rt) return;
	GPR_U64(rt) = SEXT32((GPR_U32(rt) & mask[sh]) | (mem << (24 - 8 * sh)));
}

static inline void ps2_op_lwr(ps2_cpu *c, int rt, uint32_t addr)
{
	static const uint32_t mask[4] = {0, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
	unsigned sh = addr & 3;
	uint32_t mem = ps2_read32(c, addr & ~3u);
	if (!rt) return;
	uint32_t v = (GPR_U32(rt) & mask[sh]) | (mem >> (8 * sh));
	if (sh == 0)
		GPR_U64(rt) = SEXT32(v);
	else
		GPR_U32(rt) = v; /* upper 32 bits unchanged (as PCSX2) */
}

static inline void ps2_op_ldl(ps2_cpu *c, int rt, uint32_t addr)
{
	unsigned sh = addr & 7;
	uint64_t mem = ps2_read64(c, addr & ~7u);
	if (!rt) return;
	uint64_t keep = sh == 7 ? 0 : (0x00FFFFFFFFFFFFFFull >> (8 * sh));
	GPR_U64(rt) = (GPR_U64(rt) & keep) | (mem << (56 - 8 * sh));
}

static inline void ps2_op_ldr(ps2_cpu *c, int rt, uint32_t addr)
{
	unsigned sh = addr & 7;
	uint64_t mem = ps2_read64(c, addr & ~7u);
	if (!rt) return;
	uint64_t keep = sh == 0 ? 0 : ~(~0ull >> (8 * sh));
	GPR_U64(rt) = (GPR_U64(rt) & keep) | (mem >> (8 * sh));
}

static inline void ps2_op_swl(ps2_cpu *c, int rt, uint32_t addr)
{
	static const uint32_t mask[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0};
	unsigned sh = addr & 3;
	uint32_t mem = ps2_read32(c, addr & ~3u);
	ps2_write32(c, addr & ~3u, (GPR_U32(rt) >> (24 - 8 * sh)) | (mem & mask[sh]));
}

static inline void ps2_op_swr(ps2_cpu *c, int rt, uint32_t addr)
{
	static const uint32_t mask[4] = {0, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu};
	unsigned sh = addr & 3;
	uint32_t mem = ps2_read32(c, addr & ~3u);
	ps2_write32(c, addr & ~3u, (GPR_U32(rt) << (8 * sh)) | (mem & mask[sh]));
}

static inline void ps2_op_sdl(ps2_cpu *c, int rt, uint32_t addr)
{
	unsigned sh = addr & 7;
	uint64_t mem = ps2_read64(c, addr & ~7u);
	uint64_t keep = sh == 7 ? 0 : (~0ull << (8 * (sh + 1)));
	ps2_write64(c, addr & ~7u, (GPR_U64(rt) >> (56 - 8 * sh)) | (mem & keep));
}

static inline void ps2_op_sdr(ps2_cpu *c, int rt, uint32_t addr)
{
	unsigned sh = addr & 7;
	uint64_t mem = ps2_read64(c, addr & ~7u);
	uint64_t keep = sh == 0 ? 0 : (~0ull >> (64 - 8 * sh));
	ps2_write64(c, addr & ~7u, (GPR_U64(rt) << (8 * sh)) | (mem & keep));
}

static inline void ps2_op_lq(ps2_cpu *c, int rt, uint32_t addr)
{
	ps2_u128 v;
	ps2_read128(c, addr & ~15u, &v);
	if (rt) c->r[rt] = v;
}

/* ------------------------------------------------------------------------ */
/* MMI (128-bit parallel). Sources are copied first, so rd may alias rs/rt.  */
/* ------------------------------------------------------------------------ */

#define PS2_MMI_BEGIN(name)                                                   \
	static inline void ps2_mmi_##name(ps2_cpu *c, int rd, int rs, int rt)     \
	{                                                                         \
		const ps2_u128 s = c->r[rs], t = c->r[rt];                            \
		ps2_u128 d;                                                           \
		(void)s;                                                              \
		(void)t;
#define PS2_MMI_END                                                           \
		if (rd) c->r[rd] = d;                                                 \
	}
#define PS2_MMI_LANES(name, field, n, expr)                                   \
	PS2_MMI_BEGIN(name)                                                       \
	for (int i = 0; i < (n); i++) d.field[i] = (expr);                        \
	PS2_MMI_END

static inline int32_t ps2_sat32(int64_t v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFF - 1 ? (int32_t)0x80000000u : (int32_t)v; }
static inline int16_t ps2_sat16(int32_t v) { return v > 0x7FFF ? 0x7FFF : v < -0x8000 ? -0x8000 : (int16_t)v; }
static inline int8_t ps2_sat8(int32_t v) { return v > 0x7F ? 0x7F : v < -0x80 ? -0x80 : (int8_t)v; }

/* MMI0 */
PS2_MMI_LANES(paddw, uw, 4, s.uw[i] + t.uw[i])
PS2_MMI_LANES(psubw, uw, 4, s.uw[i] - t.uw[i])
PS2_MMI_LANES(pcgtw, uw, 4, s.sw[i] > t.sw[i] ? 0xFFFFFFFFu : 0u)
PS2_MMI_LANES(pmaxw, sw, 4, s.sw[i] > t.sw[i] ? s.sw[i] : t.sw[i])
PS2_MMI_LANES(paddh, uh, 8, (uint16_t)(s.uh[i] + t.uh[i]))
PS2_MMI_LANES(psubh, uh, 8, (uint16_t)(s.uh[i] - t.uh[i]))
PS2_MMI_LANES(pcgth, uh, 8, s.sh[i] > t.sh[i] ? 0xFFFF : 0)
PS2_MMI_LANES(pmaxh, sh, 8, s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i])
PS2_MMI_LANES(paddb, ub, 16, (uint8_t)(s.ub[i] + t.ub[i]))
PS2_MMI_LANES(psubb, ub, 16, (uint8_t)(s.ub[i] - t.ub[i]))
PS2_MMI_LANES(pcgtb, ub, 16, s.sb[i] > t.sb[i] ? 0xFF : 0)
PS2_MMI_LANES(paddsw, sw, 4, ps2_sat32((int64_t)s.sw[i] + t.sw[i]))
PS2_MMI_LANES(psubsw, sw, 4, ps2_sat32((int64_t)s.sw[i] - t.sw[i]))
PS2_MMI_LANES(paddsh, sh, 8, ps2_sat16((int32_t)s.sh[i] + t.sh[i]))
PS2_MMI_LANES(psubsh, sh, 8, ps2_sat16((int32_t)s.sh[i] - t.sh[i]))
PS2_MMI_LANES(paddsb, sb, 16, ps2_sat8((int32_t)s.sb[i] + t.sb[i]))
PS2_MMI_LANES(psubsb, sb, 16, ps2_sat8((int32_t)s.sb[i] - t.sb[i]))
/* interleave lower halves: d = {t0, s0, t1, s1, ...} */
PS2_MMI_LANES(pextlw, uw, 4, (i & 1) ? s.uw[i >> 1] : t.uw[i >> 1])
PS2_MMI_LANES(pextlh, uh, 8, (i & 1) ? s.uh[i >> 1] : t.uh[i >> 1])
PS2_MMI_LANES(pextlb, ub, 16, (i & 1) ? s.ub[i >> 1] : t.ub[i >> 1])
/* pack even elements: low half from t, high half from s */
PS2_MMI_LANES(ppacw, uw, 4, i < 2 ? t.uw[i * 2] : s.uw[(i - 2) * 2])
PS2_MMI_LANES(ppach, uh, 8, i < 4 ? t.uh[i * 2] : s.uh[(i - 4) * 2])
PS2_MMI_LANES(ppacb, ub, 16, i < 8 ? t.ub[i * 2] : s.ub[(i - 8) * 2])
PS2_MMI_LANES(pext5, uw, 4,
              ((t.uw[i] & 0x001Fu) << 3) | ((t.uw[i] & 0x03E0u) << 6) | ((t.uw[i] & 0x7C00u) << 9) |
                  ((t.uw[i] & 0x8000u) << 16))
PS2_MMI_LANES(ppac5, uw, 4,
              ((t.uw[i] >> 3) & 0x001Fu) | ((t.uw[i] >> 6) & 0x03E0u) | ((t.uw[i] >> 9) & 0x7C00u) |
                  ((t.uw[i] >> 16) & 0x8000u))

/* MMI1 */
PS2_MMI_LANES(pabsw, uw, 4, t.uw[i] == 0x80000000u ? 0x7FFFFFFFu : (t.sw[i] < 0 ? 0u - t.uw[i] : t.uw[i]))
PS2_MMI_LANES(pabsh, uh, 8, t.uh[i] == 0x8000u ? 0x7FFF : (uint16_t)(t.sh[i] < 0 ? -t.sh[i] : t.sh[i]))
PS2_MMI_LANES(pceqw, uw, 4, s.uw[i] == t.uw[i] ? 0xFFFFFFFFu : 0u)
PS2_MMI_LANES(pceqh, uh, 8, s.uh[i] == t.uh[i] ? 0xFFFF : 0)
PS2_MMI_LANES(pceqb, ub, 16, s.ub[i] == t.ub[i] ? 0xFF : 0)
PS2_MMI_LANES(pminw, sw, 4, s.sw[i] < t.sw[i] ? s.sw[i] : t.sw[i])
PS2_MMI_LANES(pminh, sh, 8, s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i])
PS2_MMI_LANES(padsbh, uh, 8, (uint16_t)(i < 4 ? s.uh[i] - t.uh[i] : s.uh[i] + t.uh[i]))
PS2_MMI_LANES(padduw, uw, 4, (uint64_t)s.uw[i] + t.uw[i] > 0xFFFFFFFFull ? 0xFFFFFFFFu : s.uw[i] + t.uw[i])
PS2_MMI_LANES(psubuw, uw, 4, s.uw[i] > t.uw[i] ? s.uw[i] - t.uw[i] : 0u)
PS2_MMI_LANES(padduh, uh, 8, (uint32_t)s.uh[i] + t.uh[i] > 0xFFFFu ? 0xFFFF : (uint16_t)(s.uh[i] + t.uh[i]))
PS2_MMI_LANES(psubuh, uh, 8, s.uh[i] > t.uh[i] ? (uint16_t)(s.uh[i] - t.uh[i]) : 0)
PS2_MMI_LANES(paddub, ub, 16, (uint32_t)s.ub[i] + t.ub[i] > 0xFFu ? 0xFF : (uint8_t)(s.ub[i] + t.ub[i]))
PS2_MMI_LANES(psubub, ub, 16, s.ub[i] > t.ub[i] ? (uint8_t)(s.ub[i] - t.ub[i]) : 0)
/* interleave upper halves */
PS2_MMI_LANES(pextuw, uw, 4, (i & 1) ? s.uw[2 + (i >> 1)] : t.uw[2 + (i >> 1)])
PS2_MMI_LANES(pextuh, uh, 8, (i & 1) ? s.uh[4 + (i >> 1)] : t.uh[4 + (i >> 1)])
PS2_MMI_LANES(pextub, ub, 16, (i & 1) ? s.ub[8 + (i >> 1)] : t.ub[8 + (i >> 1)])

/* QFSRV: rd = low 128 bits of (rs:rt) >> (SA * 8). */
static inline void ps2_mmi_qfsrv(ps2_cpu *c, int rd, int rs, int rt)
{
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	ps2_u128 d;
	unsigned n = (c->sa & 15) * 8;
	if (n == 0) {
		d = t;
	} else if (n < 64) {
		d.ud[0] = (t.ud[0] >> n) | (t.ud[1] << (64 - n));
		d.ud[1] = (t.ud[1] >> n) | (s.ud[0] << (64 - n));
	} else if (n == 64) {
		d.ud[0] = t.ud[1];
		d.ud[1] = s.ud[0];
	} else {
		d.ud[0] = (t.ud[1] >> (n - 64)) | (s.ud[0] << (128 - n));
		d.ud[1] = (s.ud[0] >> (n - 64)) | (s.ud[1] << (128 - n));
	}
	if (rd) c->r[rd] = d;
}

/* MMI2 / MMI3: word multiply / divide using lanes 0 and 2. */
static inline void ps2_mmi_pmultw(ps2_cpu *c, int rd, int rs, int rt)
{
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	for (int k = 0; k < 2; k++) {
		int64_t p = (int64_t)s.sw[2 * k] * t.sw[2 * k];
		c->lo.ud[k] = SEXT32((uint64_t)p);
		c->hi.ud[k] = SEXT32((uint64_t)p >> 32);
		if (rd) c->r[rd].sd[k] = p;
	}
}

static inline void ps2_mmi_pmultuw(ps2_cpu *c, int rd, int rs, int rt)
{
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	for (int k = 0; k < 2; k++) {
		uint64_t p = (uint64_t)s.uw[2 * k] * t.uw[2 * k];
		c->lo.ud[k] = SEXT32(p);
		c->hi.ud[k] = SEXT32(p >> 32);
		if (rd) c->r[rd].ud[k] = p;
	}
}

/* pmaddw/pmsubw/pmadduw: HI:LO (lanes 0/2) +/- product. Implemented per the
 * manual (64-bit accumulate), not PCSX2's approximation. */
static inline void ps2_mmi_pmaddw_impl(ps2_cpu *c, int rd, int rs, int rt, int sub, int uns)
{
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	for (int k = 0; k < 2; k++) {
		uint64_t acc = ((uint64_t)c->hi.uw[2 * k] << 32) | c->lo.uw[2 * k];
		uint64_t p = uns ? (uint64_t)s.uw[2 * k] * t.uw[2 * k]
		                 : (uint64_t)((int64_t)s.sw[2 * k] * t.sw[2 * k]);
		uint64_t r = sub ? acc - p : acc + p;
		c->lo.ud[k] = SEXT32(r);
		c->hi.ud[k] = SEXT32(r >> 32);
		if (rd) c->r[rd].ud[k] = r;
	}
}
static inline void ps2_mmi_pmaddw(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_pmaddw_impl(c, rd, rs, rt, 0, 0); }
static inline void ps2_mmi_pmsubw(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_pmaddw_impl(c, rd, rs, rt, 1, 0); }
static inline void ps2_mmi_pmadduw(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_pmaddw_impl(c, rd, rs, rt, 0, 1); }

static inline void ps2_mmi_pdivw(ps2_cpu *c, int rd, int rs, int rt)
{
	(void)rd;
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	for (int k = 0; k < 2; k++) ps2_div32(s.sw[2 * k], t.sw[2 * k], &c->lo.ud[k], &c->hi.ud[k]);
}

static inline void ps2_mmi_pdivuw(ps2_cpu *c, int rd, int rs, int rt)
{
	(void)rd;
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	for (int k = 0; k < 2; k++) ps2_divu32(s.uw[2 * k], t.uw[2 * k], &c->lo.ud[k], &c->hi.ud[k]);
}

/* pdivbw: each word of rs / halfword 0 of rt; LO/HI words get quotient/remainder. */
static inline void ps2_mmi_pdivbw(ps2_cpu *c, int rd, int rs, int rt)
{
	(void)rd;
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	int32_t d = t.sh[0];
	for (int k = 0; k < 4; k++) {
		int32_t n = s.sw[k];
		if ((uint32_t)n == 0x80000000u && d == -1) {
			c->lo.sw[k] = (int32_t)0x80000000u;
			c->hi.sw[k] = 0;
		} else if (d != 0) {
			c->lo.sw[k] = n / d;
			c->hi.sw[k] = n % d;
		} else {
			c->lo.sw[k] = n < 0 ? 1 : -1;
			c->hi.sw[k] = n;
		}
	}
}

/* Halfword multiply family. Products go to LO.w0, LO.w1, HI.w0, HI.w1, LO.w2,
 * LO.w3, HI.w2, HI.w3 for halfwords 0..7; rd gets {LO.w0, HI.w0, LO.w2, HI.w2}. */
static inline uint32_t *ps2_hl_word(ps2_cpu *c, int i)
{
	static const unsigned char sel[8] = {0, 1, 0, 1, 2, 3, 2, 3};
	return ((i >> 1) & 1) ? &c->hi.uw[sel[i]] : &c->lo.uw[sel[i]];
}
static inline void ps2_hl_to_rd(ps2_cpu *c, int rd)
{
	if (!rd) return;
	ps2_u128 d;
	d.uw[0] = c->lo.uw[0];
	d.uw[1] = c->hi.uw[0];
	d.uw[2] = c->lo.uw[2];
	d.uw[3] = c->hi.uw[2];
	c->r[rd] = d;
}
static inline void ps2_mmi_pmulth_impl(ps2_cpu *c, int rd, int rs, int rt, int mode /*0 mul,1 madd,2 msub*/)
{
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	for (int i = 0; i < 8; i++) {
		uint32_t p = (uint32_t)((int32_t)s.sh[i] * (int32_t)t.sh[i]);
		uint32_t *w = ps2_hl_word(c, i);
		*w = mode == 0 ? p : mode == 1 ? *w + p : *w - p;
	}
	ps2_hl_to_rd(c, rd);
}
static inline void ps2_mmi_pmulth(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_pmulth_impl(c, rd, rs, rt, 0); }
static inline void ps2_mmi_pmaddh(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_pmulth_impl(c, rd, rs, rt, 1); }
static inline void ps2_mmi_pmsubh(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_pmulth_impl(c, rd, rs, rt, 2); }

/* phmadh/phmsbh: pairwise. Word k of {LO.w0,HI.w0,LO.w2,HI.w2} = s[2k+1]*t[2k+1] +/- s[2k]*t[2k];
 * the odd words receive the upper product (inverted for phmsbh, as PCSX2). */
static inline void ps2_mmi_phmadh_impl(ps2_cpu *c, int rd, int rs, int rt, int sub)
{
	const ps2_u128 s = c->r[rs], t = c->r[rt];
	for (int k = 0; k < 4; k++) {
		uint32_t hi = (uint32_t)((int32_t)s.sh[2 * k + 1] * (int32_t)t.sh[2 * k + 1]);
		uint32_t lo = (uint32_t)((int32_t)s.sh[2 * k] * (int32_t)t.sh[2 * k]);
		uint32_t *base = (k & 1) ? c->hi.uw : c->lo.uw;
		int w = (k >> 1) * 2;
		base[w] = sub ? hi - lo : hi + lo;
		base[w + 1] = sub ? ~hi : hi;
	}
	ps2_hl_to_rd(c, rd);
}
static inline void ps2_mmi_phmadh(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_phmadh_impl(c, rd, rs, rt, 0); }
static inline void ps2_mmi_phmsbh(ps2_cpu *c, int rd, int rs, int rt) { ps2_mmi_phmadh_impl(c, rd, rs, rt, 1); }

PS2_MMI_BEGIN(psllvw)
	d.ud[0] = SEXT32(t.uw[0] << (s.uw[0] & 31));
	d.ud[1] = SEXT32(t.uw[2] << (s.uw[2] & 31));
PS2_MMI_END
PS2_MMI_BEGIN(psrlvw)
	d.ud[0] = SEXT32(t.uw[0] >> (s.uw[0] & 31));
	d.ud[1] = SEXT32(t.uw[2] >> (s.uw[2] & 31));
PS2_MMI_END
PS2_MMI_BEGIN(psravw)
	d.ud[0] = SEXT32((uint32_t)(t.sw[0] >> (s.uw[0] & 31)));
	d.ud[1] = SEXT32((uint32_t)(t.sw[2] >> (s.uw[2] & 31)));
PS2_MMI_END
PS2_MMI_BEGIN(pinth)
	for (int i = 0; i < 8; i++) d.uh[i] = (i & 1) ? s.uh[4 + (i >> 1)] : t.uh[i >> 1];
PS2_MMI_END
PS2_MMI_BEGIN(pinteh)
	for (int i = 0; i < 8; i++) d.uh[i] = (i & 1) ? s.uh[i - 1] : t.uh[i];
PS2_MMI_END
PS2_MMI_BEGIN(pcpyld)
	d.ud[0] = t.ud[0];
	d.ud[1] = s.ud[0];
PS2_MMI_END
PS2_MMI_BEGIN(pcpyud)
	d.ud[0] = s.ud[1];
	d.ud[1] = t.ud[1];
PS2_MMI_END
PS2_MMI_LANES(pand, ud, 2, s.ud[i] & t.ud[i])
PS2_MMI_LANES(por, ud, 2, s.ud[i] | t.ud[i])
PS2_MMI_LANES(pxor, ud, 2, s.ud[i] ^ t.ud[i])
PS2_MMI_LANES(pnor, ud, 2, ~(s.ud[i] | t.ud[i]))
/* element shuffles of rt */
PS2_MMI_BEGIN(pexeh)
	static const unsigned char m[8] = {2, 1, 0, 3, 6, 5, 4, 7};
	for (int i = 0; i < 8; i++) d.uh[i] = t.uh[m[i]];
PS2_MMI_END
PS2_MMI_BEGIN(prevh)
	static const unsigned char m[8] = {3, 2, 1, 0, 7, 6, 5, 4};
	for (int i = 0; i < 8; i++) d.uh[i] = t.uh[m[i]];
PS2_MMI_END
PS2_MMI_BEGIN(pexch)
	static const unsigned char m[8] = {0, 2, 1, 3, 4, 6, 5, 7};
	for (int i = 0; i < 8; i++) d.uh[i] = t.uh[m[i]];
PS2_MMI_END
PS2_MMI_BEGIN(pcpyh)
	for (int i = 0; i < 8; i++) d.uh[i] = t.uh[i & 4];
PS2_MMI_END
PS2_MMI_BEGIN(pexew)
	d.uw[0] = t.uw[2]; d.uw[1] = t.uw[1]; d.uw[2] = t.uw[0]; d.uw[3] = t.uw[3];
PS2_MMI_END
PS2_MMI_BEGIN(prot3w)
	d.uw[0] = t.uw[1]; d.uw[1] = t.uw[2]; d.uw[2] = t.uw[0]; d.uw[3] = t.uw[3];
PS2_MMI_END
PS2_MMI_BEGIN(pexcw)
	d.uw[0] = t.uw[0]; d.uw[1] = t.uw[2]; d.uw[2] = t.uw[1]; d.uw[3] = t.uw[3];
PS2_MMI_END

/* shifts by immediate sa (rt only) */
static inline void ps2_mmi_psllh(ps2_cpu *c, int rd, int rt, int sa) { ps2_u128 t = c->r[rt], d; for (int i = 0; i < 8; i++) d.uh[i] = (uint16_t)(t.uh[i] << (sa & 15)); if (rd) c->r[rd] = d; }
static inline void ps2_mmi_psrlh(ps2_cpu *c, int rd, int rt, int sa) { ps2_u128 t = c->r[rt], d; for (int i = 0; i < 8; i++) d.uh[i] = (uint16_t)(t.uh[i] >> (sa & 15)); if (rd) c->r[rd] = d; }
static inline void ps2_mmi_psrah(ps2_cpu *c, int rd, int rt, int sa) { ps2_u128 t = c->r[rt], d; for (int i = 0; i < 8; i++) d.sh[i] = (int16_t)(t.sh[i] >> (sa & 15)); if (rd) c->r[rd] = d; }
static inline void ps2_mmi_psllw(ps2_cpu *c, int rd, int rt, int sa) { ps2_u128 t = c->r[rt], d; for (int i = 0; i < 4; i++) d.uw[i] = t.uw[i] << sa; if (rd) c->r[rd] = d; }
static inline void ps2_mmi_psrlw(ps2_cpu *c, int rd, int rt, int sa) { ps2_u128 t = c->r[rt], d; for (int i = 0; i < 4; i++) d.uw[i] = t.uw[i] >> sa; if (rd) c->r[rd] = d; }
static inline void ps2_mmi_psraw(ps2_cpu *c, int rd, int rt, int sa) { ps2_u128 t = c->r[rt], d; for (int i = 0; i < 4; i++) d.sw[i] = t.sw[i] >> sa; if (rd) c->r[rd] = d; }

/* count leading sign bits - 1, per word of rs (words 0 and 1) */
static inline uint32_t ps2_clsb(int32_t v)
{
	uint32_t u = (uint32_t)(v < 0 ? ~v : v);
	return (u == 0 ? 32u : (uint32_t)__builtin_clz(u)) - 1u;
}
static inline void ps2_mmi_plzcw(ps2_cpu *c, int rd, int rs)
{
	if (!rd) return;
	int32_t a = c->r[rs].sw[0], b = c->r[rs].sw[1];
	c->r[rd].uw[0] = ps2_clsb(a);
	c->r[rd].uw[1] = ps2_clsb(b);
}

static inline uint16_t ps2_clamp_h(int32_t v) { return v > 0x7FFF ? 0x7FFF : v < -0x8000 ? 0x8000 : (uint16_t)v; }
static inline void ps2_mmi_pmfhl(ps2_cpu *c, int rd, int fmt)
{
	const ps2_u128 lo = c->lo, hi = c->hi;
	ps2_u128 d = rd ? c->r[rd] : lo;
	switch (fmt) {
	case 0: /* .lw */
		d.uw[0] = lo.uw[0]; d.uw[1] = hi.uw[0]; d.uw[2] = lo.uw[2]; d.uw[3] = hi.uw[2];
		break;
	case 1: /* .uw */
		d.uw[0] = lo.uw[1]; d.uw[1] = hi.uw[1]; d.uw[2] = lo.uw[3]; d.uw[3] = hi.uw[3];
		break;
	case 2: /* .slw */
		for (int k = 0; k < 2; k++) {
			int64_t v = (int64_t)(((uint64_t)hi.uw[2 * k] << 32) | lo.uw[2 * k]);
			d.ud[k] = v >= 0x7FFFFFFFll ? 0x7FFFFFFFull : v <= -0x80000000ll ? 0xFFFFFFFF80000000ull : SEXT32(lo.uw[2 * k]);
		}
		break;
	case 3: /* .lh */
		d.uh[0] = lo.uh[0]; d.uh[1] = lo.uh[2]; d.uh[2] = hi.uh[0]; d.uh[3] = hi.uh[2];
		d.uh[4] = lo.uh[4]; d.uh[5] = lo.uh[6]; d.uh[6] = hi.uh[4]; d.uh[7] = hi.uh[6];
		break;
	case 4: /* .sh */
		d.uh[0] = ps2_clamp_h(lo.sw[0]); d.uh[1] = ps2_clamp_h(lo.sw[1]);
		d.uh[2] = ps2_clamp_h(hi.sw[0]); d.uh[3] = ps2_clamp_h(hi.sw[1]);
		d.uh[4] = ps2_clamp_h(lo.sw[2]); d.uh[5] = ps2_clamp_h(lo.sw[3]);
		d.uh[6] = ps2_clamp_h(hi.sw[2]); d.uh[7] = ps2_clamp_h(hi.sw[3]);
		break;
	default:
		return;
	}
	if (rd) c->r[rd] = d;
}

static inline void ps2_mmi_pmthl(ps2_cpu *c, int rs, int fmt)
{
	if (fmt != 0) return;
	const ps2_u128 s = c->r[rs];
	c->lo.uw[0] = s.uw[0];
	c->hi.uw[0] = s.uw[1];
	c->lo.uw[2] = s.uw[2];
	c->hi.uw[2] = s.uw[3];
}

/* ------------------------------------------------------------------------ */
/* COP1 (FPU). Inputs: denormal -> signed 0, exponent 255 -> +/-FLT_MAX.      */
/* Results: overflow -> +/-FLT_MAX, denormal -> signed 0. Rounding comes from */
/* the host FPCR (round toward zero, see ps2_abi.h).                         */
/* Not modelled: O/U/SO/SU flag bits in FCR31.                               */
/* ------------------------------------------------------------------------ */

#define PS2_FMAX 0x7F7FFFFFu

static inline float ps2_fpu_val(uint32_t u)
{
	uint32_t e = u & 0x7F800000u;
	if (e == 0)
		u &= 0x80000000u;
	else if (e == 0x7F800000u)
		u = (u & 0x80000000u) | PS2_FMAX;
	return ps2_bits_f(u);
}

static inline uint32_t ps2_fpu_res(float f)
{
	uint32_t u = ps2_f_bits(f);
	uint32_t e = u & 0x7F800000u;
	if (e == 0x7F800000u)
		u = (u & 0x80000000u) | PS2_FMAX;
	else if (e == 0)
		u &= 0x80000000u;
	return u;
}

#define PS2_FV(n) ps2_fpu_val(c->f[n].u)

static inline void ps2_fpu_add(ps2_cpu *c, int fd, int fs, int ft) { c->f[fd].u = ps2_fpu_res(PS2_FV(fs) + PS2_FV(ft)); }
static inline void ps2_fpu_sub(ps2_cpu *c, int fd, int fs, int ft) { c->f[fd].u = ps2_fpu_res(PS2_FV(fs) - PS2_FV(ft)); }
static inline void ps2_fpu_mul(ps2_cpu *c, int fd, int fs, int ft) { c->f[fd].u = ps2_fpu_res(PS2_FV(fs) * PS2_FV(ft)); }
static inline void ps2_fpu_adda(ps2_cpu *c, int fs, int ft) { c->facc.u = ps2_fpu_res(PS2_FV(fs) + PS2_FV(ft)); }
static inline void ps2_fpu_suba(ps2_cpu *c, int fs, int ft) { c->facc.u = ps2_fpu_res(PS2_FV(fs) - PS2_FV(ft)); }
static inline void ps2_fpu_mula(ps2_cpu *c, int fs, int ft) { c->facc.u = ps2_fpu_res(PS2_FV(fs) * PS2_FV(ft)); }
static inline float ps2_fpu_prod(ps2_cpu *c, int fs, int ft) { return ps2_fpu_val(ps2_f_bits(PS2_FV(fs) * PS2_FV(ft))); }
static inline void ps2_fpu_madd(ps2_cpu *c, int fd, int fs, int ft) { c->f[fd].u = ps2_fpu_res(ps2_fpu_val(c->facc.u) + ps2_fpu_prod(c, fs, ft)); }
static inline void ps2_fpu_msub(ps2_cpu *c, int fd, int fs, int ft) { c->f[fd].u = ps2_fpu_res(ps2_fpu_val(c->facc.u) - ps2_fpu_prod(c, fs, ft)); }
static inline void ps2_fpu_madda(ps2_cpu *c, int fs, int ft) { c->facc.u = ps2_fpu_res(ps2_fpu_val(c->facc.u) + ps2_fpu_prod(c, fs, ft)); }
static inline void ps2_fpu_msuba(ps2_cpu *c, int fs, int ft) { c->facc.u = ps2_fpu_res(ps2_fpu_val(c->facc.u) - ps2_fpu_prod(c, fs, ft)); }

static inline void ps2_fpu_div(ps2_cpu *c, int fd, int fs, int ft)
{
	uint32_t s = c->f[fs].u, t = c->f[ft].u;
	if ((t & 0x7F800000u) == 0) {
		c->fcr31 |= (s & 0x7F800000u) == 0 ? (PS2_FCR31_I | PS2_FCR31_SI) : (PS2_FCR31_D | PS2_FCR31_SD);
		c->f[fd].u = ((s ^ t) & 0x80000000u) | PS2_FMAX;
		return;
	}
	c->f[fd].u = ps2_fpu_res(ps2_fpu_val(s) / ps2_fpu_val(t));
}

static inline void ps2_fpu_sqrt(ps2_cpu *c, int fd, int ft)
{
	uint32_t t = c->f[ft].u;
	c->fcr31 &= ~(PS2_FCR31_I | PS2_FCR31_D);
	if ((t & 0x7F800000u) == 0) {
		c->f[fd].u = t & 0x80000000u;
	} else {
		if (t & 0x80000000u) c->fcr31 |= PS2_FCR31_I | PS2_FCR31_SI;
		c->f[fd].u = ps2_fpu_res(__builtin_sqrtf(ps2_fpu_val(t & 0x7FFFFFFFu)));
	}
}

static inline void ps2_fpu_rsqrt(ps2_cpu *c, int fd, int fs, int ft)
{
	uint32_t s = c->f[fs].u, t = c->f[ft].u;
	c->fcr31 &= ~(PS2_FCR31_I | PS2_FCR31_D);
	if ((t & 0x7F800000u) == 0) {
		c->fcr31 |= PS2_FCR31_D | PS2_FCR31_SD;
		c->f[fd].u = ((s ^ t) & 0x80000000u) | PS2_FMAX;
		return;
	}
	if (t & 0x80000000u) c->fcr31 |= PS2_FCR31_I | PS2_FCR31_SI;
	float r = ps2_fpu_val(ps2_fpu_res(__builtin_sqrtf(ps2_fpu_val(t & 0x7FFFFFFFu))));
	c->f[fd].u = ps2_fpu_res(ps2_fpu_val(s) / r);
}

/* Ordering-preserving integer key for EE min/max (no NaNs on the EE). */
static inline uint32_t ps2_fmax_bits(uint32_t a, uint32_t b)
{
	int32_t x = (int32_t)a, y = (int32_t)b;
	if (x < 0 && y < 0) return (uint32_t)(x < y ? x : y);
	return (uint32_t)(x > y ? x : y);
}
static inline uint32_t ps2_fmin_bits(uint32_t a, uint32_t b)
{
	int32_t x = (int32_t)a, y = (int32_t)b;
	if (x < 0 && y < 0) return (uint32_t)(x > y ? x : y);
	return (uint32_t)(x < y ? x : y);
}

static inline void ps2_fpu_cvt_w_s(ps2_cpu *c, int fd, int fs)
{
	uint32_t s = c->f[fs].u;
	if ((s & 0x7F800000u) <= 0x4E800000u)
		c->f[fd].s = (int32_t)ps2_bits_f(s); /* truncation */
	else
		c->f[fd].u = (s & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
}

static inline void ps2_fpu_cvt_s_w(ps2_cpu *c, int fd, int fs) { c->f[fd].u = ps2_f_bits((float)c->f[fs].s); }

static inline void ps2_fpu_cmp(ps2_cpu *c, int cond)
{
	if (cond)
		c->fcr31 |= PS2_FCR31_C;
	else
		c->fcr31 &= ~PS2_FCR31_C;
}
#define PS2_FPU_C (c->fcr31 & PS2_FCR31_C)

/* ------------------------------------------------------------------------ */
/* COP2 / VU0 macro mode                                                     */
/* Field order x,y,z,w = element 0..3; dest mask bit 8 = x ... bit 1 = w.    */
/* MAC flag bit layout per field (x shift 3 .. w shift 0):                   */
/*   Z 0x0001, S 0x0010, U 0x0100, O 0x1000.                                 */
/* Status: Z S U O (bits 0-3), I D (4,5), sticky ZS SS US OS (6-9), IS DS.   */
/* ------------------------------------------------------------------------ */

#define ps2_vu_val ps2_fpu_val

static inline ps2_u128 ps2_vsplat(uint32_t bits)
{
	ps2_u128 v;
	v.uw[0] = v.uw[1] = v.uw[2] = v.uw[3] = bits;
	return v;
}

/* Clamp a VU result and update MAC bits for one field.
 * `exact` classifies the infinitely precise result when the host result is
 * ambiguous (host rounding is toward zero, so overflow yields FMAX instead of
 * Inf, and FZ turns underflow into an exact-looking zero):
 *   0 = no information, 1 = overflow, 2 = underflow. */
static inline uint32_t ps2_vu_mac(uint32_t *mac, int field, float f, int exact)
{
	const int sh = 3 - field;
	uint32_t v = ps2_f_bits(f);
	uint32_t s = v & 0x80000000u;
	uint32_t m = *mac & ~(0x1111u << sh);
	if (s) m |= 0x0010u << sh;
	uint32_t e = v & 0x7F800000u;
	if (exact == 1 || e == 0x7F800000u) {
		m |= 0x1000u << sh;
		v = s | PS2_FMAX;
	} else if (exact == 2 || (e == 0 && (v & 0x7FFFFFFFu) != 0)) {
		m |= 0x0101u << sh;
		v = s;
	} else if ((v & 0x7FFFFFFFu) == 0) {
		m |= 0x0001u << sh;
	}
	*mac = m;
	return v;
}

/* Exact (double precision) check for results that are +/-FMAX or +/-0. */
static inline int ps2_vu_exact(float r, double exact)
{
	uint32_t rb = ps2_f_bits(r) & 0x7FFFFFFFu;
	if (rb == PS2_FMAX) return __builtin_fabs(exact) > 3.4028234663852886e38 ? 1 : 0;
	if (rb == 0) return exact != 0.0 ? 2 : 0;
	return 0;
}

static inline void ps2_vu_status_update(ps2_cpu *c, uint32_t mac)
{
	uint32_t n = 0;
	if (mac & 0x000Fu) n |= 1;
	if (mac & 0x00F0u) n |= 2;
	if (mac & 0x0F00u) n |= 4;
	if (mac & 0xF000u) n |= 8;
	c->vi[PS2_VI_MAC] = mac;
	c->vi[PS2_VI_STATUS] = (c->vi[PS2_VI_STATUS] & ~0xFu) | n | (n << 6);
}

enum { PS2_VU_ADD, PS2_VU_SUB, PS2_VU_MUL, PS2_VU_MADD, PS2_VU_MSUB };

/* Generic FMAC op: d = s (op) t, or ACC (+/-) s*t for MADD/MSUB.
 * d is &c->vf[fd], &c->vacc, or NULL (fd == vf0: flags only). */
static inline void ps2_vu_fmac(ps2_cpu *c, int kind, unsigned dest, ps2_u128 *d, ps2_u128 s, ps2_u128 t)
{
	const ps2_u128 acc = c->vacc;
	ps2_u128 out;
	uint32_t mac = c->vi[PS2_VI_MAC];
	if (d) out = *d;
	for (int i = 0; i < 4; i++) {
		if (!(dest & (8u >> i))) {
			mac &= ~(0x1111u << (3 - i));
			continue;
		}
		float a = ps2_vu_val(s.uw[i]), b = ps2_vu_val(t.uw[i]), r;
		switch (kind) {
		case PS2_VU_ADD: r = a + b; break;
		case PS2_VU_SUB: r = a - b; break;
		case PS2_VU_MUL: r = a * b; break;
		case PS2_VU_MADD: r = ps2_vu_val(acc.uw[i]) + a * b; break;
		default: r = ps2_vu_val(acc.uw[i]) - a * b; break;
		}
		int ex = 0;
		uint32_t rb = ps2_f_bits(r) & 0x7FFFFFFFu;
		if (PS2_UNLIKELY(rb == PS2_FMAX || rb == 0)) {
			double da = a, db = b, dc = ps2_vu_val(acc.uw[i]), d;
			switch (kind) {
			case PS2_VU_ADD: d = da + db; break;
			case PS2_VU_SUB: d = da - db; break;
			case PS2_VU_MUL: d = da * db; break;
			case PS2_VU_MADD: d = dc + (double)(a * b); break;
			default: d = dc - (double)(a * b); break;
			}
			ex = ps2_vu_exact(r, d);
		}
		out.uw[i] = ps2_vu_mac(&mac, i, r, ex);
	}
	ps2_vu_status_update(c, mac);
	if (d) *d = out;
}

#define PS2_VF_DST(fd) ((fd) ? &c->vf[fd] : (ps2_u128 *)0)

/* vopmula: ACC.xyz = fs.yzx * ft.zxy ; vopmsub: fd.xyz = ACC.xyz - fs.yzx * ft.zxy */
static inline void ps2_vu_opm(ps2_cpu *c, ps2_u128 *d, int fs, int ft, int msub)
{
	const ps2_u128 s = c->vf[fs], t = c->vf[ft], acc = c->vacc;
	static const unsigned char a[3] = {1, 2, 0}, b[3] = {2, 0, 1};
	ps2_u128 out;
	uint32_t mac = c->vi[PS2_VI_MAC] & ~0x1111u; /* w cleared */
	if (d) out = *d;
	for (int i = 0; i < 3; i++) {
		float x = ps2_vu_val(s.uw[a[i]]), y = ps2_vu_val(t.uw[b[i]]);
		float p = x * y;
		float r = msub ? ps2_vu_val(acc.uw[i]) - p : p;
		double d = msub ? (double)ps2_vu_val(acc.uw[i]) - (double)p : (double)x * (double)y;
		out.uw[i] = ps2_vu_mac(&mac, i, r, ps2_vu_exact(r, d));
	}
	ps2_vu_status_update(c, mac);
	if (d) *d = out;
}

static inline void ps2_vu_minmax(ps2_cpu *c, int fd, unsigned dest, ps2_u128 s, ps2_u128 t, int is_max)
{
	if (!fd) return;
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i))
			c->vf[fd].uw[i] = is_max ? ps2_fmax_bits(s.uw[i], t.uw[i]) : ps2_fmin_bits(s.uw[i], t.uw[i]);
}

static inline uint32_t ps2_vu_ftoi(uint32_t u, int frac)
{
	float f = ps2_bits_f(u);
	if (frac) f *= (float)(1u << frac);
	u = ps2_f_bits(f);
	if ((u & 0x7F800000u) >= 0x4F000000u) return (u & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
	return (uint32_t)(int32_t)f;
}

static inline uint32_t ps2_vu_itof(uint32_t u, int frac)
{
	float f = (float)(int32_t)u;
	if (frac) f *= 1.0f / (float)(1u << frac);
	return ps2_f_bits(f);
}

static inline void ps2_vu_ftoi_op(ps2_cpu *c, unsigned dest, int ft, int fs, int frac)
{
	if (!ft) return;
	const ps2_u128 s = c->vf[fs];
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = ps2_vu_ftoi(s.uw[i], frac);
}

static inline void ps2_vu_itof_op(ps2_cpu *c, unsigned dest, int ft, int fs, int frac)
{
	if (!ft) return;
	const ps2_u128 s = c->vf[fs];
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = ps2_vu_itof(s.uw[i], frac);
}

static inline void ps2_vu_abs(ps2_cpu *c, unsigned dest, int ft, int fs)
{
	if (!ft) return;
	const ps2_u128 s = c->vf[fs];
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = s.uw[i] & 0x7FFFFFFFu;
}

static inline void ps2_vu_move(ps2_cpu *c, unsigned dest, int ft, int fs)
{
	if (!ft) return;
	const ps2_u128 s = c->vf[fs];
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = s.uw[i];
}

static inline void ps2_vu_mr32(ps2_cpu *c, unsigned dest, int ft, int fs)
{
	if (!ft) return;
	const ps2_u128 s = c->vf[fs];
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = s.uw[(i + 1) & 3];
}

static inline void ps2_vu_clip(ps2_cpu *c, int fs, int ft)
{
	const ps2_u128 s = c->vf[fs];
	int32_t w = (int32_t)c->vf[ft].uw[3];
	w = (w & 0x7F800000) ? (w & 0x7FFFFFFF) : 0x007FFFFF;
	uint32_t cf = c->vi[PS2_VI_CLIP] << 6;
	for (int i = 0; i < 3; i++) {
		if ((int32_t)(s.uw[i]) > w) cf |= 1u << (2 * i);
		if ((int32_t)(s.uw[i] ^ 0x80000000u) > w) cf |= 2u << (2 * i);
	}
	c->vi[PS2_VI_CLIP] = cf & 0xFFFFFFu;
}

/* Q-register ops. Status I = bit 4, D = bit 5; sticky IS = bit 10, DS = bit 11. */
static inline void ps2_vu_div(ps2_cpu *c, int fs, int fsf, int ft, int ftf)
{
	uint32_t sb = c->vf[fs].uw[fsf], tb = c->vf[ft].uw[ftf];
	float s = ps2_vu_val(sb), t = ps2_vu_val(tb);
	uint32_t st = c->vi[PS2_VI_STATUS] & ~0x30u;
	if (t == 0.0f) {
		st |= (s == 0.0f) ? 0x410u : 0x820u;
		c->vi[PS2_VI_Q] = ((sb ^ tb) & 0x80000000u) | PS2_FMAX;
	} else {
		c->vi[PS2_VI_Q] = ps2_fpu_res(s / t);
	}
	c->vi[PS2_VI_STATUS] = st;
}

static inline void ps2_vu_sqrt(ps2_cpu *c, int ft, int ftf)
{
	float t = ps2_vu_val(c->vf[ft].uw[ftf]);
	uint32_t st = c->vi[PS2_VI_STATUS] & ~0x30u;
	if (t < 0.0f) st |= 0x410u;
	c->vi[PS2_VI_Q] = ps2_fpu_res(__builtin_sqrtf(__builtin_fabsf(t)));
	c->vi[PS2_VI_STATUS] = st;
}

static inline void ps2_vu_rsqrt(ps2_cpu *c, int fs, int fsf, int ft, int ftf)
{
	uint32_t sb = c->vf[fs].uw[fsf], tb = c->vf[ft].uw[ftf];
	float s = ps2_vu_val(sb), t = ps2_vu_val(tb);
	uint32_t st = c->vi[PS2_VI_STATUS] & ~0x30u;
	if (t == 0.0f) {
		st |= 0x820u;
		if (s != 0.0f) {
			c->vi[PS2_VI_Q] = ((sb ^ tb) & 0x80000000u) | PS2_FMAX;
		} else {
			c->vi[PS2_VI_Q] = (sb ^ tb) & 0x80000000u;
			st |= 0x410u;
		}
	} else {
		if (t < 0.0f) st |= 0x410u;
		c->vi[PS2_VI_Q] = ps2_fpu_res(s / __builtin_sqrtf(__builtin_fabsf(t)));
	}
	c->vi[PS2_VI_STATUS] = st;
}

/* Integer ops (16-bit VI registers). */
static inline void ps2_vi_set(ps2_cpu *c, int r, uint32_t v)
{
	if (r) c->vi[r] = v & 0xFFFFu;
}

static inline uint32_t ps2_vu_mem_addr(ps2_cpu *c, int vi) { return (c->vi[vi] * 16u) & (PS2_VU0_MEM_SIZE - 1); }

static inline void ps2_vu_ilwr(ps2_cpu *c, unsigned dest, int it, int is)
{
	const uint8_t *p = c->vu0_mem + ps2_vu_mem_addr(c, is);
	uint16_t v = 0;
	int any = 0;
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) {
			memcpy(&v, p + 4 * i, 2);
			any = 1;
		}
	if (any) ps2_vi_set(c, it, v);
}

static inline void ps2_vu_iswr(ps2_cpu *c, unsigned dest, int it, int is)
{
	uint8_t *p = c->vu0_mem + ps2_vu_mem_addr(c, is);
	uint32_t v = c->vi[it] & 0xFFFFu;
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) memcpy(p + 4 * i, &v, 4);
}

/* vlqi/vlqd: vf[ft] <- VU mem[vi[is]] (post-increment / pre-decrement).
 * vsqi/vsqd: VU mem[vi[it]] <- vf[fs]. */
static inline void ps2_vu_lq(ps2_cpu *c, unsigned dest, int ft, int is, int inc)
{
	if (inc < 0) ps2_vi_set(c, is, c->vi[is] - 1);
	const uint8_t *p = c->vu0_mem + ps2_vu_mem_addr(c, is);
	if (ft)
		for (int i = 0; i < 4; i++)
			if (dest & (8u >> i)) memcpy(&c->vf[ft].uw[i], p + 4 * i, 4);
	if (inc > 0) ps2_vi_set(c, is, c->vi[is] + 1);
}

static inline void ps2_vu_sq(ps2_cpu *c, unsigned dest, int fs, int it, int inc)
{
	if (inc < 0) ps2_vi_set(c, it, c->vi[it] - 1);
	uint8_t *p = c->vu0_mem + ps2_vu_mem_addr(c, it);
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) memcpy(p + 4 * i, &c->vf[fs].uw[i], 4);
	if (inc > 0) ps2_vi_set(c, it, c->vi[it] + 1);
}

static inline void ps2_vu_mfir(ps2_cpu *c, unsigned dest, int ft, int is)
{
	if (!ft) return;
	uint32_t v = (uint32_t)(int32_t)(int16_t)c->vi[is];
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = v;
}

/* Random number unit (R register). */
static inline void ps2_vu_rnext(ps2_cpu *c, unsigned dest, int ft)
{
	uint32_t r = c->vi[PS2_VI_R];
	uint32_t x = (r >> 4) & 1, y = (r >> 22) & 1;
	r = (r << 1) ^ x ^ y;
	r = (r & 0x7FFFFFu) | 0x3F800000u;
	c->vi[PS2_VI_R] = r;
	if (!ft) return;
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = r;
}

static inline void ps2_vu_rget(ps2_cpu *c, unsigned dest, int ft)
{
	if (!ft) return;
	for (int i = 0; i < 4; i++)
		if (dest & (8u >> i)) c->vf[ft].uw[i] = c->vi[PS2_VI_R];
}

/* COP2 transfers. */
static inline void ps2_cop2_cfc2(ps2_cpu *c, int rt, int id)
{
	uint32_t v;
	if (id >= 26 && id != 30)
		v = ps2_vu0_ctrl_read(c, (uint32_t)id);
	else if (id == PS2_VI_R)
		v = c->vi[id] & 0x7FFFFFu;
	else
		v = c->vi[id];
	if (rt) GPR_U64(rt) = SEXT32(v);
}

static inline void ps2_cop2_ctc2(ps2_cpu *c, int rt, int id)
{
	uint32_t v = GPR_U32(rt);
	if (id == 0) return;
	if (id < 16) {
		c->vi[id] = v & 0xFFFFu;
	} else if (id >= 26 && id != 30) {
		ps2_vu0_ctrl_write(c, (uint32_t)id, v);
	} else if (id == PS2_VI_MAC) {
		/* read-only */
	} else if (id == PS2_VI_R) {
		c->vi[id] = (v & 0x7FFFFFu) | 0x3F800000u;
	} else {
		c->vi[id] = v;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* PS2_OPS_H */
