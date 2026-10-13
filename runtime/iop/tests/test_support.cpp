// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_support.h"

#include <cstdarg>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "vc_iop.h"

namespace iop_test {

int g_failures = 0;

namespace {

std::map<uint32_t, std::function<void(ps2_cpu*)>>& Funcs()
{
	static std::map<uint32_t, std::function<void(ps2_cpu*)>> f;
	return f;
}

std::map<uint32_t, int>& Counts()
{
	static std::map<uint32_t, int> c;
	return c;
}

uint8_t* Spr()
{
	static std::vector<uint8_t> spr(PS2_SPR_SIZE);
	return spr.data();
}

} // namespace

uint8_t* Ram()
{
	static std::vector<uint8_t> ram(PS2_RAM_SIZE);
	return ram.data();
}

ps2_cpu* Cpu()
{
	static ps2_cpu c;
	static bool init = false;
	if (!init)
	{
		c.ram = Ram();
		c.spr = Spr();
		ps2_cpu_reset_regs(&c);
		c.r[29].ud[0] = 0x01F00000; // stack
		init = true;
	}
	return &c;
}

void SetEeFunction(uint32_t addr, std::function<void(ps2_cpu*)> fn)
{
	Funcs()[addr] = std::move(fn);
}

int CallCount(uint32_t addr)
{
	return Counts()[addr];
}

std::string TempDir(const char* tag)
{
	char tmpl[256];
	const char* base = getenv("TMPDIR");
	snprintf(tmpl, sizeof tmpl, "%s/vc_iop_%s_XXXXXX", base ? base : "/tmp", tag);
	char* d = mkdtemp(tmpl);
	return d ? std::string(d) : std::string();
}

} // namespace iop_test

extern "C" {

uint8_t* vc_ee_ptr(uint32_t addr)
{
	if (addr >= PS2_SPR_BASE && addr < PS2_SPR_BASE + PS2_SPR_SIZE)
		return iop_test::Spr() + (addr - PS2_SPR_BASE);
	uint32_t phys = addr & 0x1FFFFFFF;
	return phys < PS2_RAM_SIZE ? iop_test::Ram() + phys : nullptr;
}

void vc_ee_raise_dmac(int) {}
void vc_ee_raise_intc(int) {}
uint64_t vc_ee_time_us(void) { return 0; }

void vc_log(const char* tag, const char* fmt, ...)
{
	if (!getenv("VC_IOP_TEST_VERBOSE"))
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[%s] ", tag);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void ps2_call_indirect(ps2_cpu* c, uint32_t pc, uint32_t target)
{
	(void)pc;
	iop_test::Counts()[target]++;
	auto it = iop_test::Funcs().find(target);
	if (it != iop_test::Funcs().end())
		it->second(c);
	else
		fprintf(stderr, "ps2_call_indirect: unknown EE function %08x\n", target);
}

void ps2_jump_indirect(ps2_cpu* c, uint32_t pc, uint32_t target)
{
	ps2_call_indirect(c, pc, target);
}

} // extern "C"
