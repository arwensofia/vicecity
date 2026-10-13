// SPDX-License-Identifier: GPL-3.0-or-later
// Stage 2 of the recompiler tests: runs the C emitted for each snippet in
// cases.inc against a fresh CPU context and checks register/memory effects.
#include "asm.h"
#include "test_env.h"
#include "tests_generated.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace a;

namespace {

int g_failures = 0;
const char *g_current = "";
std::vector<std::string> g_failed_tests;

void fail_eq(int line, const char *expr, uint64_t got, uint64_t want)
{
	g_failures++;
	std::printf("  FAIL %s (cases.inc:%d): %s = 0x%016llx, expected 0x%016llx\n", g_current, line, expr,
	            static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
}

uint32_t fb(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, 4);
	return u;
}

uint8_t *g_ram, *g_spr, *g_vu0;

void w32(uint32_t addr, uint32_t v) { std::memcpy(g_ram + addr, &v, 4); }
void w64(uint32_t addr, uint64_t v) { std::memcpy(g_ram + addr, &v, 8); }
uint32_t r32(uint32_t addr)
{
	uint32_t v;
	std::memcpy(&v, g_ram + addr, 4);
	return v;
}
uint64_t r64(uint32_t addr)
{
	uint64_t v;
	std::memcpy(&v, g_ram + addr, 8);
	return v;
}
void set128(ps2_u128 &r, uint64_t lo, uint64_t hi)
{
	r.ud[0] = lo;
	r.ud[1] = hi;
}
void setw(ps2_u128 &r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
	r.uw[0] = w0;
	r.uw[1] = w1;
	r.uw[2] = w2;
	r.uw[3] = w3;
}
void seth(ps2_u128 &r, int h0, int h1, int h2, int h3, int h4, int h5, int h6, int h7)
{
	const int v[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
	for (int k = 0; k < 8; k++) r.uh[k] = static_cast<uint16_t>(v[k]);
}
void setf(ps2_u128 &r, float x, float y, float z, float w)
{
	r.f[0] = x;
	r.f[1] = y;
	r.f[2] = z;
	r.f[3] = w;
}

} // namespace

#define CHECK_EQ(a, b)                                                                     \
	do {                                                                                   \
		uint64_t _a = static_cast<uint64_t>(a), _b = static_cast<uint64_t>(b);             \
		if (_a != _b) fail_eq(__LINE__, #a, _a, _b);                                       \
	} while (0)
#define CHECK(cond) CHECK_EQ(!!(cond), 1)
#define GPR(n) (c->r[n].ud[0])
#define GPRHI(n) (c->r[n].ud[1])
#define FPR(n) (c->f[n].u)
#define VF(n) (c->vf[n])
#define VI(n) (c->vi[n])

#define TESTCASE(name, words, ...)                     \
	static void run_##name(ps2_cpu *c)                 \
	{                                                  \
		auto RUN = [&]() { TFN_##name(c); };           \
		(void)RUN;                                     \
		__VA_ARGS__                                    \
	}
#define OFFS(...) {__VA_ARGS__}
#define TESTCASE_JT(name, addr, offs, words, ...)                          \
	static void run_##name(ps2_cpu *c)                                     \
	{                                                                      \
		static const uint32_t o[] = offs;                                  \
		for (size_t k = 0; k < sizeof o / sizeof o[0]; k++)                \
			w32((addr) + 4 * static_cast<uint32_t>(k), TADDR_##name + 4 * o[k]); \
		auto RUN = [&]() { TFN_##name(c); };                               \
		__VA_ARGS__                                                        \
	}
#include "cases.inc"
#undef TESTCASE
#undef TESTCASE_JT

struct TestEntry {
	const char *name;
	void (*fn)(ps2_cpu *);
};
#define TESTCASE(name, words, ...) {#name, run_##name},
#define TESTCASE_JT(name, addr, offs, words, ...) {#name, run_##name},
static const TestEntry kTests[] = {
#include "cases.inc"
};

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : nullptr;
	ps2_set_host_fpcr();
	g_ram = static_cast<uint8_t *>(std::aligned_alloc(16, PS2_RAM_SIZE));
	g_spr = static_cast<uint8_t *>(std::aligned_alloc(16, PS2_SPR_SIZE));
	g_vu0 = static_cast<uint8_t *>(std::aligned_alloc(16, PS2_VU0_MEM_SIZE));
	std::memset(g_ram, 0, PS2_RAM_SIZE);
	static ps2_cpu ctx;
	int run = 0;
	for (const TestEntry &t : kTests) {
		if (only && std::strcmp(only, t.name) != 0) continue;
		std::memset(g_ram, 0, 0x40000);
		std::memset(g_spr, 0, PS2_SPR_SIZE);
		std::memset(g_vu0, 0, PS2_VU0_MEM_SIZE);
		ctx.ram = g_ram;
		ctx.spr = g_spr;
		ctx.vu0_mem = g_vu0;
		ps2_cpu_reset_regs(&ctx);
		ctx.r[31].ud[0] = 0xDEAD0000u;
		g_log = HookLog();
		g_current = t.name;
		int before = g_failures;
		t.fn(&ctx);
		CHECK_EQ(ctx.r[0].ud[0] | ctx.r[0].ud[1], 0); // r0 never written
		run++;
		if (g_failures != before) g_failed_tests.push_back(t.name);
	}
	std::printf("recomp_tests: %d tests, %zu failed, %d failed checks\n", run, g_failed_tests.size(), g_failures);
	for (const std::string &n : g_failed_tests) std::printf("  failed: %s\n", n.c_str());
	return g_failures ? 1 : 0;
}
