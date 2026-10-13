// SPDX-License-Identifier: GPL-3.0-or-later
// Trace/debug build of the recompiler tests: the same generated snippets,
// compiled with -DPS2_TRACE=3 -DPS2_TRACK_PC=2 and linked with the support
// library (recomp/support/ps2_recomp_support.c). Checks the trace hooks, the
// ring buffer, c->pc tracking, symbolization and the unknown-target hook.
#include "ps2_recomp.h"
#include "test_env.h"
#include "tests_generated.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

int g_fail = 0;
#define T_CHECK(cond)                                                          \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::printf("  FAIL test_trace.cpp:%d: %s\n", __LINE__, #cond);    \
			g_fail++;                                                          \
		}                                                                      \
	} while (0)
#define T_CHECK_EQ(a, b)                                                                     \
	do {                                                                                     \
		unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b);       \
		if (_a != _b) {                                                                      \
			std::printf("  FAIL test_trace.cpp:%d: %s = 0x%llx, expected 0x%llx\n", __LINE__, \
			            #a, _a, _b);                                                         \
			g_fail++;                                                                        \
		}                                                                                    \
	} while (0)

std::vector<uint32_t> g_backedges;

std::vector<ps2_trace_entry> since(uint32_t head0)
{
	std::vector<ps2_trace_entry> v;
	for (uint32_t k = head0; k != ps2_trace_buffer.head; k++) v.push_back(ps2_trace_buffer.e[k & (PS2_TRACE_RING_SIZE - 1)]);
	return v;
}

} // namespace

// Strong override of the weak default: poll at every back edge.
extern "C" void ps2_backedge(ps2_cpu *, uint32_t target)
{
	g_backedges.push_back(target);
	ps2_backedge_budget = 0;
}

int main()
{
	ps2_set_host_fpcr();
	static ps2_cpu ctx;
	ctx.ram = static_cast<uint8_t *>(std::aligned_alloc(16, PS2_RAM_SIZE));
	ctx.spr = static_cast<uint8_t *>(std::aligned_alloc(16, PS2_SPR_SIZE));
	ctx.vu0_mem = static_cast<uint8_t *>(std::aligned_alloc(16, PS2_VU0_MEM_SIZE));
	std::memset(ctx.ram, 0, 0x40000);
	ps2_cpu_reset_regs(&ctx);
	ps2_cpu *c = &ctx;

	// loop_countdown: addiu a0,zero,10; L: addiu a0,a0,-1; bne a0,zero,L; addiu v0,v0,2; jr ra; nop
	c->r[29].ud[0] = 0x01F00000;
	uint32_t h0 = ps2_trace_buffer.head;
	ps2_backedge_budget = 0;
	TFN_loop_countdown(c);
	T_CHECK_EQ(g_backedges.size(), 9u); // bne taken 9 times
	T_CHECK(!g_backedges.empty() && g_backedges[0] == TADDR_loop_countdown + 4);
	std::vector<ps2_trace_entry> t = since(h0);
	T_CHECK_EQ(t.size(), 12u); // fn + 10 x loop block + block after the loop
	if (t.size() == 12) {
		T_CHECK_EQ(t[0].addr_kind, TADDR_loop_countdown | PS2_TRACE_KIND_FN);
		T_CHECK_EQ(t[0].sp, 0x01F00000u);
		for (int k = 1; k <= 10; k++) T_CHECK_EQ(t[k].addr_kind, (TADDR_loop_countdown + 4) | PS2_TRACE_KIND_BB);
		T_CHECK_EQ(t[11].addr_kind, (TADDR_loop_countdown + 16) | PS2_TRACE_KIND_BB);
	}
	T_CHECK_EQ(c->pc, TADDR_loop_countdown + 16); // PS2_TRACK_PC >= 1: last block

	// loads: PS2_TRACK_PC = 2 records the last load/store address.
	c->r[4].ud[0] = 0x1000;
	c->r[5].ud[0] = 0x1010;
	TFN_loads(c);
	T_CHECK_EQ(c->pc, TADDR_loads + 36); // lw zero, 0(a0) is the 10th instruction

	// Calls: jal_recompiled enters the callee at 0x00300100.
	h0 = ps2_trace_buffer.head;
	TFN_jal_recompiled(c);
	t = since(h0);
	T_CHECK(t.size() >= 3);
	if (t.size() >= 3) {
		T_CHECK_EQ(t[0].addr_kind, TADDR_jal_recompiled | PS2_TRACE_KIND_FN);
		T_CHECK_EQ(t[1].addr_kind, 0x00300100u | PS2_TRACE_KIND_FN);
		T_CHECK_EQ(t[2].addr_kind, (TADDR_jal_recompiled + 8) | PS2_TRACE_KIND_BB); // return point
	}

	// Symbolization.
	char buf[160];
	std::string s = ps2_recomp_format_addr(TADDR_loads + 8, buf, sizeof buf);
	T_CHECK(s == "test_loads+0x8 [tests]");
	s = ps2_recomp_format_addr(0x00300100u, buf, sizeof buf);
	T_CHECK(s == "ps2f_00300100 [tests]");
	s = ps2_recomp_format_addr(0x00000010u, buf, sizeof buf);
	T_CHECK(s == "0x00000010 (?)");
	T_CHECK(ps2_recomp_find_symbol(TADDR_loads + 0x300) == nullptr); // past the reached code
	T_CHECK(ps2_recomp_find_symbol(TADDR_loads) != nullptr);
	T_CHECK_EQ(ps2_recomp_host_to_ee(reinterpret_cast<const void *>(&TFN_loads)), TADDR_loads);

	// Unknown indirect target hook (default implementation logs + records MISS).
	h0 = ps2_trace_buffer.head;
	uint32_t before = ps2_dispatch_unknown_count;
	int saved = dup(2), devnull = open("/dev/null", O_WRONLY);
	dup2(devnull, 2); // keep the expected log line out of the test output
	ps2_dispatch_unknown(c, TADDR_loads + 4, 0x00DEAD00u, PS2_DISPATCH_JUMP);
	dup2(saved, 2);
	close(devnull);
	T_CHECK_EQ(ps2_dispatch_unknown_count, before + 1);
	t = since(h0);
	T_CHECK(!t.empty() && t.back().addr_kind == (0x00DEAD00u | PS2_TRACE_KIND_MISS));

	// Ring dump (async-signal-safe writer) contains symbolized entries.
	char path[] = "/tmp/ps2trace_XXXXXX";
	int fd = mkstemp(path);
	T_CHECK(fd >= 0);
	if (fd >= 0) {
		ps2_trace_dump(fd, 16);
		lseek(fd, 0, SEEK_SET);
		std::string out(8192, '\0');
		ssize_t n = read(fd, out.data(), out.size());
		out.resize(n > 0 ? static_cast<size_t>(n) : 0);
		close(fd);
		unlink(path);
		T_CHECK(out.find("--- ps2 trace: last 10 (hex)") != std::string::npos);
		T_CHECK(out.find("MISS 00dead00") != std::string::npos);
		T_CHECK(out.find("test_jal_recompiled") != std::string::npos);
	}

	std::printf("recomp_trace_tests: %d failed checks\n", g_fail);
	return g_fail ? 1 : 0;
}
