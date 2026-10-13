// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE interrupts as the kernel delivers them: INTC and DMAC handler chains, alarms (the
// libkernl Timer-3 patch, implemented natively), vblank timing, EE timers 0-3.

#include "kernel.h"
#include "log.h"
#include "vc_gs.h"
#include "vc_iop.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mach/mach_time.h>
#include <time.h>
#include <vector>

namespace vc::kernel {

namespace {

constexpr uint32_t INTC_STAT = 0x1000F000u;
constexpr uint32_t INTC_MASK = 0x1000F010u;
constexpr uint32_t D_STAT = 0x1000E010u;
constexpr uint32_t GS_CSR = 0x12001000u;

constexpr int INTC_VBLANK_S = 2;
constexpr int INTC_VBLANK_E = 3;
constexpr int INTC_TIM0 = 9;

constexpr uint64_t kVBlankPeriodNs = 16683350; // NTSC field, 59.94 Hz
constexpr uint64_t kVBlankLengthNs = 1430000;  // ~22.5 lines of vertical blank
constexpr double kHsyncHz = 15734.264;
constexpr double kBusHz = 147456000.0;
constexpr uint64_t kIopPollNs = 1000000;

// EE stack for interrupt handlers (kernel memory; the program starts at 0x100000).
constexpr uint32_t kIntrStackTop = 0x00050000u;

struct Handler
{
	int id;
	uint32_t func, arg, gp;
	bool enabled;
};

struct Alarm
{
	int id;
	uint64_t due;
	uint32_t func, arg, gp;
	uint16_t hsyncs;
};

struct Timer
{
	uint32_t mode = 0, comp = 0, hold = 0;
	uint64_t base_ns = 0;
	uint64_t base_count = 0;
	uint64_t last_ticks = 0; // ticks at the last interrupt check, unwrapped
};

std::vector<Handler> g_intc[16];
std::vector<Handler> g_dmac[16];
int g_next_handler_id = 1;
std::vector<Alarm> g_alarms;
int g_next_alarm_id = 0;
Timer g_timers[4];

bool g_in_interrupt = false;
bool g_need_resched = false;
bool g_hw_pending = false;
alignas(16) ps2_cpu g_icpu;

uint64_t g_start_abs = 0;
mach_timebase_info_data_t g_timebase;
uint64_t g_next_timed = 0;
uint64_t g_next_vblank = 0;
uint64_t g_vblank_end_due = 0;
uint64_t g_last_iop_poll = 0;
uint64_t g_vblanks = 0;
uint32_t g_vsync_flag = 0, g_vsync_csr = 0;
uint64_t g_last_idle_report = 0;
uint64_t g_idle_since = 0;
std::vector<std::pair<uint64_t, std::string>> g_screenshots;

void TakeScreenshot(const std::string& path)
{
	uint32_t w = 0, h = 0;
	std::vector<uint32_t> rgba;
	if (!vc::gs::CaptureDisplay(&w, &h, &rgba) || !w || !h) {
		Log("kernel", "screenshot %s: nothing displayed", path.c_str());
		return;
	}
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return;
	std::fprintf(f, "P6\n%u %u\n255\n", w, h);
	uint64_t sum = 0;
	for (uint32_t px : rgba) {
		const uint8_t rgb[3] = {static_cast<uint8_t>(px), static_cast<uint8_t>(px >> 8), static_cast<uint8_t>(px >> 16)};
		sum += rgb[0] + rgb[1] + rgb[2];
		std::fwrite(rgb, 1, 3, f);
	}
	std::fclose(f);
	Log("kernel", "screenshot %s: %ux%u, mean level %.1f", path.c_str(), w, h,
		static_cast<double>(sum) / (3.0 * static_cast<double>(rgba.size())));
}

void CopyCop2(ps2_cpu* dst, const ps2_cpu* src)
{
	std::memcpy(dst->vf, src->vf, sizeof dst->vf);
	dst->vacc = src->vacc;
	std::memcpy(dst->vi, src->vi, sizeof dst->vi);
}

uint64_t Sext(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

int32_t CallHandler(uint32_t func, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2)
{
	ps2_cpu* cur = CurrentCpu();
	ps2_cpu* ic = &g_icpu;
	CopyCop2(ic, cur);
	ic->r[4].ud[0] = Sext(a0);
	ic->r[5].ud[0] = Sext(a1);
	ic->r[6].ud[0] = Sext(a2);
	ic->r[28].ud[0] = Sext(gp);
	ic->r[29].ud[0] = kIntrStackTop - 0x10;
	ic->r[31].ud[0] = 0;
	ps2_call_indirect(ic, 0, func);
	CopyCop2(cur, ic);
	return ic->r[2].sw[0];
}

uint32_t HwRead(uint32_t addr)
{
	uint32_t v = 0;
	vc::gs::HwRead32(addr, &v);
	return v;
}

// --- timers ------------------------------------------------------------------------

double TimerRate(const Timer& t)
{
	switch (t.mode & 3) {
	case 0: return kBusHz;
	case 1: return kBusHz / 16;
	case 2: return kBusHz / 256;
	default: return kHsyncHz;
	}
}

uint64_t TimerTicks(const Timer& t, uint64_t now)
{
	if (!(t.mode & 0x80)) // CUE
		return t.base_count;
	const double dt = static_cast<double>(now - t.base_ns) * 1e-9;
	return t.base_count + static_cast<uint64_t>(dt * TimerRate(t));
}

uint32_t TimerCount(const Timer& t, uint64_t now)
{
	const uint64_t ticks = TimerTicks(t, now);
	if ((t.mode & 0x40) && t.comp) // ZRET
		return static_cast<uint32_t>(ticks % (static_cast<uint64_t>(t.comp) + 1));
	return static_cast<uint32_t>(ticks & 0xFFFF);
}

void TimerRebase(Timer& t, uint64_t now, uint32_t count)
{
	t.base_ns = now;
	t.base_count = count;
	t.last_ticks = count;
}

// Number of k with prev < k*period + offset <= cur.
uint64_t Crossings(uint64_t prev, uint64_t cur, uint64_t period, uint64_t offset)
{
	auto f = [&](uint64_t x) -> int64_t {
		return x < offset ? -1 : static_cast<int64_t>((x - offset) / period);
	};
	return static_cast<uint64_t>(std::max<int64_t>(0, f(cur) - f(prev)));
}

void UpdateTimers(uint64_t now)
{
	for (int i = 0; i < 4; i++) {
		Timer& t = g_timers[i];
		if (!(t.mode & 0x80) || !(t.mode & 0x300))
			continue;
		const uint64_t cur = TimerTicks(t, now);
		const uint64_t prev = t.last_ticks;
		if (cur == prev)
			continue;
		t.last_ticks = cur;
		const bool zret = (t.mode & 0x40) && t.comp;
		const uint64_t period = zret ? static_cast<uint64_t>(t.comp) + 1 : 0x10000;
		bool raise = false;
		if ((t.mode & 0x100) && Crossings(prev, cur, period, t.comp) && !(t.mode & 0x400)) {
			t.mode |= 0x400; // EQUF
			raise = true;
		}
		if ((t.mode & 0x200) && !zret && Crossings(prev, cur, 0x10000, 0) && !(t.mode & 0x800)) {
			t.mode |= 0x800; // OVFF
			raise = true;
		}
		if (raise)
			vc::gs::RaiseIntc(INTC_TIM0 + i);
	}
}

bool AnyTimerInterrupts()
{
	for (const Timer& t : g_timers)
		if ((t.mode & 0x80) && (t.mode & 0x300))
			return true;
	return false;
}

// --- vblank ------------------------------------------------------------------------

void VBlankStart()
{
	g_vblanks++;
	if (g_vsync_flag) {
		Store32(g_vsync_flag, 1);
		if (g_vsync_csr)
			Store64(g_vsync_csr, vc::gs::GsPrivRead64(GS_CSR));
	}
	vc::gs::VSync(); // presents, toggles FIELD, raises GS VSINT
	for (const auto& [when, path] : g_screenshots)
		if (when == g_vblanks)
			TakeScreenshot(path);
	vc::gs::RaiseIntc(INTC_VBLANK_S);
	vc_iop_vblank();
	if (g_vblanks <= 3 || (g_vblanks % 600) == 0)
		Log("kernel", "vblank %llu", static_cast<unsigned long long>(g_vblanks));
}

void ProcessTimed(uint64_t now)
{
	if (now >= g_next_vblank) {
		VBlankStart();
		g_next_vblank += kVBlankPeriodNs;
		if (now > g_next_vblank + 4 * kVBlankPeriodNs)
			g_next_vblank = now + kVBlankPeriodNs; // fell behind (debugger, long frame): resync
		g_vblank_end_due = now + kVBlankLengthNs;
	}
	if (g_vblank_end_due && now >= g_vblank_end_due) {
		g_vblank_end_due = 0;
		vc::gs::RaiseIntc(INTC_VBLANK_E);
	}
	UpdateTimers(now);
	if (now - g_last_iop_poll >= kIopPollNs) {
		g_last_iop_poll = now;
		vc_iop_poll();
	}

	uint64_t next = std::min(g_next_vblank, g_last_iop_poll + kIopPollNs);
	if (g_vblank_end_due)
		next = std::min(next, g_vblank_end_due);
	for (const Alarm& a : g_alarms)
		next = std::min(next, a.due);
	if (AnyTimerInterrupts())
		next = std::min(next, now + 250000);
	g_next_timed = next;
}

bool AlarmDue(uint64_t now)
{
	for (const Alarm& a : g_alarms)
		if (a.due <= now)
			return true;
	return false;
}

void CallChain(std::vector<Handler>& chain, int cause)
{
	// Copy: a handler may add/remove handlers.
	const std::vector<Handler> handlers = chain;
	for (const Handler& h : handlers)
		if (h.enabled)
			CallHandler(h.func, h.gp, static_cast<uint32_t>(cause), h.arg, 0);
}

// Run every pending interrupt source's handlers (with interrupts "disabled").
void Dispatch()
{
	if (g_in_interrupt)
		return;
	g_in_interrupt = true;
	g_hw_pending = false;
	for (int round = 0; round < 16; round++) {
		bool any = false;

		const uint64_t now = NowNs();
		for (size_t i = 0; i < g_alarms.size();) {
			if (g_alarms[i].due <= now) {
				const Alarm a = g_alarms[i];
				g_alarms.erase(g_alarms.begin() + static_cast<long>(i));
				const uint32_t hs = static_cast<uint32_t>(static_cast<double>(now) * 1e-9 * kHsyncHz) & 0xFFFF;
				CallHandler(a.func, a.gp, static_cast<uint32_t>(a.id), hs, a.arg);
				any = true;
				i = 0;
				continue;
			}
			i++;
		}

		const uint32_t intc = HwRead(INTC_STAT) & HwRead(INTC_MASK) & 0x7FFF;
		for (int bit = 0; bit < 15; bit++) {
			if (!(intc & (1u << bit)))
				continue;
			vc::gs::HwWrite32(INTC_STAT, 1u << bit); // acknowledge
			CallChain(g_intc[bit], bit);
			any = true;
		}

		const uint32_t d = HwRead(D_STAT);
		const uint32_t dmac = d & (d >> 16) & 0xE3FF;
		for (int ch = 0; ch < 16; ch++) {
			if (!(dmac & (1u << ch)))
				continue;
			vc::gs::HwWrite32(D_STAT, 1u << ch); // clear CIS
			CallChain(g_dmac[ch], ch);
			any = true;
		}

		if (!any)
			break;
	}
	g_in_interrupt = false;
}

int32_t AddHandler(std::vector<Handler>* table, int32_t index, uint32_t func, int32_t next, uint32_t arg,
	uint32_t gp)
{
	if (index < 0 || index >= 16)
		return -1;
	const Handler h{g_next_handler_id++, func, arg, gp, true};
	auto& chain = table[index];
	if (next == 0)
		chain.push_back(h);
	else
		chain.insert(chain.begin(), h);
	return h.id;
}

int32_t RemoveHandler(std::vector<Handler>* table, int32_t index, int32_t id)
{
	if (index < 0 || index >= 16)
		return -1;
	auto& chain = table[index];
	auto it = std::find_if(chain.begin(), chain.end(), [&](const Handler& h) { return h.id == id; });
	if (it == chain.end())
		return -1;
	chain.erase(it);
	return 0;
}

int32_t SetHandlerEnabled(std::vector<Handler>* table, int32_t index, int32_t id, bool enable)
{
	if (index < 0 || index >= 16)
		return -1;
	for (Handler& h : table[index])
		if (h.id == id) {
			h.enabled = enable;
			return 0;
		}
	return -1;
}

} // namespace

// ---------------------------------------------------------------------------------------

uint64_t NowNs()
{
	const uint64_t t = mach_absolute_time() - g_start_abs;
	return t * g_timebase.numer / g_timebase.denom;
}

void InitInterrupts()
{
	mach_timebase_info(&g_timebase);
	g_start_abs = mach_absolute_time();
	g_icpu.ram = g_ram;
	g_icpu.spr = g_spr;
	g_icpu.vu0_mem = g_vu0_mem;
	ps2_cpu_reset_regs(&g_icpu);
	g_next_vblank = kVBlankPeriodNs;
	g_next_timed = 0;
}

bool InInterrupt() { return g_in_interrupt; }

namespace {
int g_icontext = 0;
}
void BeginIContext() { g_icontext++; }
void EndIContext() { g_icontext--; }
bool InIContext() { return g_in_interrupt || g_icontext > 0; }
void RequestReschedule() { g_need_resched = true; }

bool TakeRescheduleRequest()
{
	const bool r = g_need_resched;
	g_need_resched = false;
	return r;
}

void OnInt0(void*) { g_hw_pending = true; }
void OnInt1(void*) { g_hw_pending = true; }

void OnDmaStart(void*, int channel)
{
	if (channel >= 5 && channel <= 7) {
		vc_iop_dma_start(channel);
		return;
	}
	if (channel == 8 || channel == 9) {
		SprDmaStart(channel);
		return;
	}
	static bool warned[10];
	if (channel >= 0 && channel < 10 && !warned[channel]) {
		warned[channel] = true;
		Log("dmac", "DMA channel %d started (not implemented)", channel);
	}
}

void Poll()
{
	if (g_in_interrupt)
		return;
	const uint64_t now = NowNs();
	if (now >= g_next_timed)
		ProcessTimed(now);
	if (!ThreadInterruptsEnabled())
		return;
	if (g_hw_pending || (g_alarms.size() && AlarmDue(now)))
		Dispatch();
	if (g_need_resched && g_icontext == 0) {
		g_need_resched = false;
		Preempt();
	}
}

void IdleWait()
{
	uint64_t now = NowNs();
	if (!g_idle_since)
		g_idle_since = now;
	if (!g_hw_pending && now < g_next_timed && !AlarmDue(now)) {
		const uint64_t wait = std::min<uint64_t>(g_next_timed - now, kIopPollNs);
		timespec ts{0, static_cast<long>(wait)};
		nanosleep(&ts, nullptr);
		now = NowNs();
	}
	ProcessTimed(now);
	vc_iop_poll();
	// The idle loop runs with interrupts enabled whatever the blocked thread's Status says.
	if (g_hw_pending || AlarmDue(now))
		Dispatch();
	g_need_resched = false; // the scheduler loop picks the best thread anyway

	if (now - g_idle_since > 5000000000ull && now - g_last_idle_report > 5000000000ull) {
		g_last_idle_report = now;
		Log("kernel", "all EE threads blocked for %.1f s", static_cast<double>(now - g_idle_since) * 1e-9);
		DumpThreads();
	}
}

// Called by the scheduler whenever a thread gets the CPU (resets the idle-hang detector).
void NoteNotIdle() { g_idle_since = 0; }

int32_t AddIntcHandler(int32_t cause, uint32_t func, int32_t next, uint32_t arg, uint32_t gp)
{
	const int32_t id = AddHandler(g_intc, cause, func, next, arg, gp);
	Log("intr", "AddIntcHandler(cause %d, %08x, arg %08x) -> %d", cause, func, arg, id);
	return id;
}

int32_t RemoveIntcHandler(int32_t cause, int32_t id) { return RemoveHandler(g_intc, cause, id); }

int32_t AddDmacHandler(int32_t channel, uint32_t func, int32_t next, uint32_t arg, uint32_t gp)
{
	const int32_t id = AddHandler(g_dmac, channel, func, next, arg, gp);
	Log("intr", "AddDmacHandler(channel %d, %08x, arg %08x) -> %d", channel, func, arg, id);
	return id;
}

int32_t RemoveDmacHandler(int32_t channel, int32_t id) { return RemoveHandler(g_dmac, channel, id); }

int32_t EnableIntcHandler(int32_t cause, int32_t id, bool enable)
{
	return SetHandlerEnabled(g_intc, cause, id, enable);
}

int32_t EnableDmacHandler(int32_t channel, int32_t id, bool enable)
{
	return SetHandlerEnabled(g_dmac, channel, id, enable);
}

int32_t EnableIntc(int32_t cause)
{
	if (cause < 0 || cause >= 15)
		return -1;
	if (HwRead(INTC_MASK) & (1u << cause))
		return 0;
	vc::gs::HwWrite32(INTC_MASK, 1u << cause); // INTC_MASK writes toggle
	return 1;
}

int32_t DisableIntc(int32_t cause)
{
	if (cause < 0 || cause >= 15)
		return -1;
	if (!(HwRead(INTC_MASK) & (1u << cause)))
		return 0;
	vc::gs::HwWrite32(INTC_MASK, 1u << cause);
	return 1;
}

int32_t EnableDmac(int32_t channel)
{
	if (channel < 0 || channel >= 16)
		return -1;
	if (HwRead(D_STAT) & (1u << (16 + channel)))
		return 0;
	vc::gs::HwWrite32(D_STAT, 1u << (16 + channel)); // CIM bits toggle
	return 1;
}

int32_t DisableDmac(int32_t channel)
{
	if (channel < 0 || channel >= 16)
		return -1;
	if (!(HwRead(D_STAT) & (1u << (16 + channel))))
		return 0;
	vc::gs::HwWrite32(D_STAT, 1u << (16 + channel));
	return 1;
}

int32_t SetAlarm(uint32_t hsyncs, uint32_t func, uint32_t arg, uint32_t gp)
{
	if (g_alarms.size() >= 64)
		return -1;
	Alarm a;
	a.id = g_next_alarm_id;
	g_next_alarm_id = (g_next_alarm_id + 1) & 0x7FFF;
	a.hsyncs = static_cast<uint16_t>(hsyncs);
	a.due = NowNs() + static_cast<uint64_t>(static_cast<double>(hsyncs & 0xFFFF) / kHsyncHz * 1e9);
	a.func = func;
	a.arg = arg;
	a.gp = gp;
	g_alarms.push_back(a);
	g_next_timed = std::min(g_next_timed, a.due);
	return a.id;
}

int32_t ReleaseAlarm(int32_t id)
{
	auto it = std::find_if(g_alarms.begin(), g_alarms.end(), [&](const Alarm& a) { return a.id == id; });
	if (it == g_alarms.end())
		return -1;
	g_alarms.erase(it);
	return 0;
}

void SetVSyncFlag(uint32_t flag_addr, uint32_t csr_addr)
{
	g_vsync_flag = flag_addr;
	g_vsync_csr = csr_addr;
}

uint64_t VBlankCount() { return g_vblanks; }

void AddScreenshot(uint64_t vblank, const std::string& path) { g_screenshots.emplace_back(vblank, path); }

uint32_t TimerRead(uint32_t addr)
{
	const int n = static_cast<int>((addr >> 11) & 3);
	Timer& t = g_timers[n];
	switch (addr & 0x7F0) {
	case 0x00: return TimerCount(t, NowNs());
	case 0x10: return t.mode;
	case 0x20: return t.comp;
	case 0x30: return t.hold;
	default: return 0;
	}
}

void TimerWrite(uint32_t addr, uint32_t value)
{
	const int n = static_cast<int>((addr >> 11) & 3);
	Timer& t = g_timers[n];
	const uint64_t now = NowNs();
	switch (addr & 0x7F0) {
	case 0x00:
		TimerRebase(t, now, value & 0xFFFF);
		break;
	case 0x10: {
		const uint32_t count = TimerCount(t, now);
		const uint32_t flags = t.mode & 0xC00 & ~(value & 0xC00); // write 1 to clear
		t.mode = (value & 0x3FF) | flags;
		TimerRebase(t, now, count);
		break;
	}
	case 0x20:
		t.comp = value & 0xFFFF;
		TimerRebase(t, now, TimerCount(t, now));
		break;
	case 0x30:
		t.hold = value & 0xFFFF;
		break;
	default:
		break;
	}
	g_next_timed = 0; // re-evaluate timer events at the next Poll
}

} // namespace vc::kernel
