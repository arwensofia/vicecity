// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE threads as fibers, with the EE kernel's scheduling rules, and semaphores.

#include "fiber.h"
#include "kernel.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <unistd.h>

namespace vc::kernel {

namespace {

constexpr size_t kHostStackSize = size_t{8} << 20; // per EE thread; committed lazily
// COP0 Status of a user thread: CU0-2, IM2/IM3 (INT0/INT1), IM7 (timer), KSU = user, IE.
constexpr uint32_t kStatusBase = 0x70008C11u;
constexpr uint32_t kStatusEIE = 0x00010000u;
// The EE kernel keeps a thread's saved context at the top of its stack.
constexpr uint32_t kStackContextSize = 0x2A0u;

enum WaitType : int
{
	WAIT_NONE = 0,
	WAIT_SLEEP = 1,
	WAIT_SEMA = 2,
};

struct Thread
{
	alignas(16) ps2_cpu cpu;
	int id = 0;
	bool used = false;
	int status = THS_DORMANT;
	int prio = 0;
	int init_prio = 0;
	uint32_t entry = 0, stack = 0, stack_size = 0, gp = 0, attr = 0, option = 0;
	int wakeup_count = 0;
	int wait_type = WAIT_NONE;
	int wait_id = 0;
	int32_t wait_result = 0;
	uint32_t start_arg = 0;
	uint32_t cop0_status = kStatusBase | kStatusEIE;
	bool needs_start = false;
	vc_fiber_ctx ctx{};
	FiberStack host_stack;
};

struct Sema
{
	bool used = false;
	int count = 0;
	int max = 0;
	int init = 0;
	uint32_t attr = 0;
	uint32_t option = 0;
	std::deque<Thread*> waiters;
};

Thread* g_threads[kMaxThreads] = {};
Sema g_semas[kMaxSemas];
std::deque<Thread*> g_ready[128];
Thread* g_cur = nullptr;
uint32_t g_heap_start = 0;
uint32_t g_heap_end = 0;
uint64_t g_fpcr_ee = 0;
BootConfig g_cfg;

uint64_t ReadFpcr()
{
	uint64_t v;
	__asm__ volatile("mrs %0, fpcr" : "=r"(v));
	return v;
}

void ReadyPush(Thread* t, bool front)
{
	auto& q = g_ready[t->prio];
	if (front)
		q.push_front(t);
	else
		q.push_back(t);
	t->status = THS_READY;
}

void ReadyRemove(Thread* t)
{
	auto& q = g_ready[t->prio];
	auto it = std::find(q.begin(), q.end(), t);
	if (it != q.end())
		q.erase(it);
}

Thread* ReadyBest()
{
	for (auto& q : g_ready)
		if (!q.empty())
			return q.front();
	return nullptr;
}

void CopyCop2(ps2_cpu* dst, const ps2_cpu* src)
{
	std::memcpy(dst->vf, src->vf, sizeof dst->vf);
	dst->vacc = src->vacc;
	std::memcpy(dst->vi, src->vi, sizeof dst->vi);
}

void ThreadEntry(void* p);

void SwitchTo(Thread* next)
{
	next->status = THS_RUN;
	if (next == g_cur)
		return;
	Thread* prev = g_cur;
	// VU0 (COP2) registers are one piece of hardware shared by all threads.
	CopyCop2(&next->cpu, &prev->cpu);
	g_cur = next;
	if (next->needs_start) {
		next->needs_start = false;
		FiberInit(&next->ctx, next->host_stack.Top(), ThreadEntry, next, g_fpcr_ee);
	}
	vc_fiber_switch(&prev->ctx, &next->ctx);
	// Back on `prev`.
}

// Run the highest-priority ready thread. A running current thread keeps the CPU unless a
// ready thread has a strictly higher priority; `cur_front` says where a preempted current
// thread goes in its ready queue (head after an interrupt, tail after a voluntary change).
void Reschedule(bool cur_front = true)
{
	if (InIContext()) {
		RequestReschedule();
		return;
	}
	for (;;) {
		Thread* best = ReadyBest();
		if (g_cur->status == THS_RUN) {
			if (!best || best->prio >= g_cur->prio)
				return;
			ReadyPush(g_cur, cur_front);
		}
		if (!best) {
			IdleWait();
			continue;
		}
		ReadyRemove(best);
		NoteNotIdle();
		SwitchTo(best);
		return;
	}
}

Thread* GetThread(int32_t id)
{
	if (id == 0)
		return g_cur;
	if (id < 0 || id >= kMaxThreads)
		return nullptr;
	Thread* t = g_threads[id];
	return (t && t->used) ? t : nullptr;
}

Sema* GetSema(int32_t id)
{
	if (id < 0 || id >= kMaxSemas || !g_semas[id].used)
		return nullptr;
	return &g_semas[id];
}

uint64_t Sext(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

void ThreadEntry(void* p)
{
	Thread* t = static_cast<Thread*>(p);
	ps2_cpu* c = &t->cpu;
	std::memset(c->r, 0, sizeof c->r);
	c->hi = c->lo = ps2_u128{};
	c->sa = 0;
	c->r[4].ud[0] = Sext(t->start_arg);
	c->r[28].ud[0] = Sext(t->gp);
	c->r[29].ud[0] = Sext(t->stack + t->stack_size - kStackContextSize);
	c->r[31].ud[0] = 0;
	if (g_cfg.log_syscalls)
		Log("thread", "thread %d starts at %08x (arg %08x)", t->id, t->entry, t->start_arg);
	ps2_call_indirect(c, 0, t->entry);
	// Returning from the entry function ends the thread.
	ExitThread();
	Fatal("thread %d resumed after ExitThread", t->id);
}

void RemoveFromWaitQueues(Thread* t)
{
	if (t->wait_type == WAIT_SEMA) {
		if (Sema* s = GetSema(t->wait_id)) {
			auto it = std::find(s->waiters.begin(), s->waiters.end(), t);
			if (it != s->waiters.end())
				s->waiters.erase(it);
		}
	}
	t->wait_type = WAIT_NONE;
}

// Make a waiting thread runnable again (end of WaitSema/SleepThread).
void EndWait(Thread* t, int32_t result)
{
	t->wait_type = WAIT_NONE;
	t->wait_result = result;
	if (t->status == THS_WAITSUSPEND)
		t->status = THS_SUSPEND;
	else
		ReadyPush(t, false);
}

const char* StatusName(int s)
{
	switch (s) {
	case THS_RUN: return "RUN";
	case THS_READY: return "READY";
	case THS_WAIT: return "WAIT";
	case THS_SUSPEND: return "SUSPEND";
	case THS_WAITSUSPEND: return "WAITSUSPEND";
	case THS_DORMANT: return "DORMANT";
	default: return "?";
	}
}

} // namespace

// ---------------------------------------------------------------------------------------

ps2_cpu* CurrentCpu() { return &g_cur->cpu; }
int CurrentThreadId() { return g_cur ? g_cur->id : -1; }
bool ThreadInterruptsEnabled() { return (g_cur->cop0_status & kStatusEIE) != 0; }

void SetThreadInterruptsEnabled(bool on)
{
	if (on)
		g_cur->cop0_status |= kStatusEIE;
	else
		g_cur->cop0_status &= ~kStatusEIE;
}

uint32_t ThreadCop0Status() { return g_cur->cop0_status; }
void SetThreadCop0Status(uint32_t v) { g_cur->cop0_status = v; }

int Run(const BootConfig& cfg)
{
	g_cfg = cfg;
	ps2_set_host_fpcr();
	g_fpcr_ee = ReadFpcr();
	InitInterrupts();

	Thread* m = new Thread;
	m->id = 1;
	m->used = true;
	m->status = THS_RUN;
	m->prio = m->init_prio = 0;
	m->entry = cfg.entry;
	m->cop0_status = kStatusBase; // crt0 enables interrupts itself (ei after _InitSys)
	m->cpu.ram = g_ram;
	m->cpu.spr = g_spr;
	m->cpu.vu0_mem = g_vu0_mem;
	ps2_cpu_reset_regs(&m->cpu);
	m->cpu.user = m;
	m->cpu.r[29].ud[0] = PS2_RAM_SIZE - 0x100; // until SetupThread
	g_threads[1] = m;
	g_cur = m;

	ps2_func_t entry = LookupFunction(cfg.entry);
	if (!entry)
		Fatal("entry point %08x is not a recompiled function", cfg.entry);
	Log("kernel", "starting EE at %08x", cfg.entry);
	entry(&m->cpu);
	Log("kernel", "entry function returned (v0=%d)", m->cpu.r[2].sw[0]);
	return m->cpu.r[2].sw[0];
}

uint32_t SetupThread(ps2_cpu* c, uint32_t gp, uint32_t stack, uint32_t stack_size, uint32_t args,
	uint32_t root)
{
	(void)c;
	(void)root;
	Thread* m = g_cur;
	const uint32_t base = (stack == 0xFFFFFFFFu) ? PS2_RAM_SIZE - stack_size : stack;
	m->stack = base;
	m->stack_size = stack_size;
	m->gp = gp;

	if (args) {
		// struct { int argc; char *argv[16]; char text[256]; }
		const uint32_t argc = static_cast<uint32_t>(std::min<size_t>(g_cfg.argv.size(), 16));
		uint32_t text = args + 4 + 16 * 4;
		const uint32_t text_end = text + 256;
		Store32(args, argc);
		for (uint32_t i = 0; i < argc; i++) {
			const std::string& s = g_cfg.argv[i];
			if (text + s.size() + 1 > text_end)
				break;
			Store32(args + 4 + 4 * i, text);
			if (uint8_t* p = EePtr(text, static_cast<uint32_t>(s.size() + 1)))
				std::memcpy(p, s.c_str(), s.size() + 1);
			text += static_cast<uint32_t>(s.size() + 1);
		}
	}
	const uint32_t sp = base + stack_size - kStackContextSize;
	Log("kernel", "SetupThread gp=%08x stack=%08x+%x args=%08x -> sp=%08x", gp, base, stack_size, args, sp);
	return sp;
}

uint32_t SetupHeap(uint32_t start, uint32_t size)
{
	g_heap_start = start;
	g_heap_end = (size == 0xFFFFFFFFu) ? (g_cur->stack ? g_cur->stack : PS2_RAM_SIZE) : start + size;
	Log("kernel", "SetupHeap %08x-%08x", g_heap_start, g_heap_end);
	return start;
}

uint32_t EndOfHeap() { return g_heap_end; }

int32_t CreateThread(ps2_cpu* c, uint32_t param)
{
	(void)c;
	const int32_t prio = static_cast<int32_t>(Load32(param + 20));
	if (prio < 0 || prio >= 128)
		return -1;
	int id = 2;
	while (id < kMaxThreads && g_threads[id] && g_threads[id]->used)
		id++;
	if (id >= kMaxThreads)
		return -1;
	Thread* t = g_threads[id];
	if (!t)
		t = g_threads[id] = new Thread;
	t->id = id;
	t->used = true;
	t->status = THS_DORMANT;
	t->entry = Load32(param + 4);
	t->stack = Load32(param + 8);
	t->stack_size = Load32(param + 12);
	t->gp = Load32(param + 16);
	t->prio = t->init_prio = prio;
	t->attr = Load32(param + 28);
	t->option = Load32(param + 32);
	t->wakeup_count = 0;
	t->wait_type = WAIT_NONE;
	t->cpu.ram = g_ram;
	t->cpu.spr = g_spr;
	t->cpu.vu0_mem = g_vu0_mem;
	t->cpu.user = t;
	ps2_cpu_reset_regs(&t->cpu);
	if (!t->host_stack.Valid() && !t->host_stack.Allocate(kHostStackSize))
		Fatal("out of host memory for EE thread stacks");
	Log("thread", "CreateThread id=%d entry=%08x stack=%08x+%x gp=%08x prio=%d", id, t->entry, t->stack,
		t->stack_size, t->gp, prio);
	return id;
}

int32_t DeleteThread(int32_t id)
{
	Thread* t = GetThread(id);
	if (!t || t == g_cur || t->status != THS_DORMANT)
		return -1;
	t->used = false;
	return id;
}

int32_t StartThread(int32_t id, uint32_t arg)
{
	Thread* t = GetThread(id);
	if (!t || t == g_cur || t->status != THS_DORMANT)
		return -1;
	if (!t->host_stack.Valid() && !t->host_stack.Allocate(kHostStackSize))
		Fatal("out of host memory for EE thread stacks");
	t->start_arg = arg;
	t->needs_start = true;
	t->prio = t->init_prio;
	t->wakeup_count = 0;
	t->cop0_status = kStatusBase | kStatusEIE;
	ReadyPush(t, false);
	Reschedule();
	return id;
}

void ExitThread()
{
	if (InInterrupt()) {
		Log("thread", "ExitThread from an interrupt handler ignored");
		return;
	}
	Log("thread", "thread %d exits", g_cur->id);
	g_cur->status = THS_DORMANT;
	Reschedule();
	Fatal("ExitThread: thread %d was resumed", g_cur->id);
}

void ExitDeleteThread()
{
	if (InInterrupt())
		return;
	g_cur->used = false; // the slot (and its host stack) is reused only after we switch away
	ExitThread();
}

int32_t TerminateThread(int32_t id)
{
	Thread* t = GetThread(id);
	if (!t || t == g_cur)
		return -1;
	if (t->status == THS_READY)
		ReadyRemove(t);
	RemoveFromWaitQueues(t);
	t->status = THS_DORMANT;
	t->needs_start = false;
	return id;
}

int32_t ChangeThreadPriority(int32_t id, int32_t prio)
{
	Thread* t = GetThread(id);
	if (!t || prio < 0 || prio >= 128)
		return -1;
	const int old = t->prio;
	if (t->status == THS_READY) {
		ReadyRemove(t);
		t->prio = prio;
		ReadyPush(t, false);
	} else {
		t->prio = prio;
	}
	Reschedule(false);
	return old;
}

int32_t RotateThreadReadyQueue(int32_t prio)
{
	if (prio < 0 || prio >= 128)
		return -1;
	if (g_cur->status == THS_RUN && g_cur->prio == prio) {
		if (!g_ready[prio].empty()) {
			ReadyPush(g_cur, false);
			Reschedule(false);
		}
	} else if (g_ready[prio].size() > 1) {
		Thread* t = g_ready[prio].front();
		g_ready[prio].pop_front();
		g_ready[prio].push_back(t);
	}
	return prio;
}

int32_t ReleaseWaitThread(int32_t id)
{
	Thread* t = GetThread(id);
	if (!t || t == g_cur || (t->status != THS_WAIT && t->status != THS_WAITSUSPEND))
		return -1;
	RemoveFromWaitQueues(t);
	EndWait(t, -418 /* KE_RELEASE_WAIT */);
	Reschedule();
	return id;
}

int32_t ReferThreadStatus(int32_t id, uint32_t info)
{
	Thread* t = GetThread(id);
	if (!t)
		return -1;
	if (info) {
		Store32(info + 0, static_cast<uint32_t>(t->status));
		Store32(info + 4, t->entry);
		Store32(info + 8, t->stack);
		Store32(info + 12, t->stack_size);
		Store32(info + 16, t->gp);
		Store32(info + 20, static_cast<uint32_t>(t->init_prio));
		Store32(info + 24, static_cast<uint32_t>(t->prio));
		Store32(info + 28, t->attr);
		Store32(info + 32, t->option);
		Store32(info + 36, static_cast<uint32_t>(t->wait_type));
		Store32(info + 40, static_cast<uint32_t>(t->wait_id));
		Store32(info + 44, static_cast<uint32_t>(t->wakeup_count));
	}
	return t->status;
}

int32_t SleepThread()
{
	if (InInterrupt())
		return -1;
	if (g_cur->wakeup_count > 0) {
		g_cur->wakeup_count--;
		return g_cur->id;
	}
	g_cur->status = THS_WAIT;
	g_cur->wait_type = WAIT_SLEEP;
	g_cur->wait_id = 0;
	Reschedule();
	return g_cur->id;
}

int32_t WakeupThread(int32_t id)
{
	Thread* t = GetThread(id);
	if (!t || id == 0 || (t == g_cur && !InIContext()))
		return -1;
	if ((t->status == THS_WAIT || t->status == THS_WAITSUSPEND) && t->wait_type == WAIT_SLEEP) {
		EndWait(t, t->id);
		Reschedule();
	} else if (t->status != THS_DORMANT) {
		t->wakeup_count++;
	}
	return id;
}

int32_t CancelWakeupThread(int32_t id)
{
	Thread* t = GetThread(id);
	if (!t)
		return -1;
	const int n = t->wakeup_count;
	t->wakeup_count = 0;
	return n;
}

int32_t SuspendThread(int32_t id)
{
	Thread* t = GetThread(id);
	if (!t || t == g_cur)
		return -1;
	switch (t->status) {
	case THS_READY:
		ReadyRemove(t);
		t->status = THS_SUSPEND;
		break;
	case THS_WAIT:
		t->status = THS_WAITSUSPEND;
		break;
	default:
		return -1;
	}
	return id;
}

int32_t ResumeThread(int32_t id)
{
	Thread* t = GetThread(id);
	if (!t || t == g_cur)
		return -1;
	if (t->status == THS_SUSPEND) {
		ReadyPush(t, false);
		Reschedule();
	} else if (t->status == THS_WAITSUSPEND) {
		t->status = THS_WAIT;
	} else {
		return -1;
	}
	return id;
}

// ---------------------------------------------------------------------------------------
// Semaphores
// ---------------------------------------------------------------------------------------

int32_t CreateSema(uint32_t param)
{
	int id = 1;
	while (id < kMaxSemas && g_semas[id].used)
		id++;
	if (id >= kMaxSemas)
		return -1;
	Sema& s = g_semas[id];
	s.used = true;
	s.max = static_cast<int32_t>(Load32(param + 4));
	s.init = static_cast<int32_t>(Load32(param + 8));
	s.count = s.init;
	s.attr = Load32(param + 16);
	s.option = Load32(param + 20);
	s.waiters.clear();
	return id;
}

int32_t DeleteSema(int32_t id)
{
	Sema* s = GetSema(id);
	if (!s)
		return -1;
	while (!s->waiters.empty()) {
		Thread* t = s->waiters.front();
		s->waiters.pop_front();
		EndWait(t, -425 /* KE_WAIT_DELETE */);
	}
	s->used = false;
	Reschedule();
	return id;
}

int32_t SignalSema(int32_t id)
{
	Sema* s = GetSema(id);
	if (!s)
		return -1;
	if (!s->waiters.empty()) {
		Thread* t = s->waiters.front();
		s->waiters.pop_front();
		EndWait(t, id);
		Reschedule();
	} else {
		s->count++;
	}
	return id;
}

int32_t WaitSema(int32_t id)
{
	Sema* s = GetSema(id);
	if (!s)
		return -1;
	if (s->count > 0) {
		s->count--;
		return id;
	}
	if (InInterrupt()) {
		Log("sema", "WaitSema(%d) would block inside an interrupt handler", id);
		return -1;
	}
	s->waiters.push_back(g_cur);
	g_cur->status = THS_WAIT;
	g_cur->wait_type = WAIT_SEMA;
	g_cur->wait_id = id;
	g_cur->wait_result = id;
	Reschedule();
	return g_cur->wait_result;
}

int32_t PollSema(int32_t id)
{
	Sema* s = GetSema(id);
	if (!s)
		return -1;
	if (s->count > 0) {
		s->count--;
		return id;
	}
	return -1;
}

int32_t ReferSemaStatus(int32_t id, uint32_t info)
{
	Sema* s = GetSema(id);
	if (!s)
		return -1;
	if (info) {
		Store32(info + 0, static_cast<uint32_t>(s->count));
		Store32(info + 4, static_cast<uint32_t>(s->max));
		Store32(info + 8, static_cast<uint32_t>(s->init));
		Store32(info + 12, static_cast<uint32_t>(s->waiters.size()));
		Store32(info + 16, s->attr);
		Store32(info + 20, s->option);
	}
	return id;
}

void Preempt() { Reschedule(true); }

void DumpThreads()
{
	Log("kernel", "threads (current %d, vblank %llu):", g_cur ? g_cur->id : -1,
		static_cast<unsigned long long>(VBlankCount()));
	for (Thread* t : g_threads) {
		if (!t || !t->used)
			continue;
		Log("kernel", "  %3d %-11s prio %3d entry %08x wait %d/%d wakeups %d  pc %08x ra %08x sp %08x", t->id,
			StatusName(t->status), t->prio, t->entry, t->wait_type, t->wait_id, t->wakeup_count, t->cpu.pc,
			t->cpu.r[31].uw[0], t->cpu.r[29].uw[0]);
	}
	for (int i = 0; i < kMaxSemas; i++) {
		const Sema& s = g_semas[i];
		if (s.used && (!s.waiters.empty()))
			Log("kernel", "  sema %3d count %d max %d waiters %zu (first: thread %d)", i, s.count, s.max,
				s.waiters.size(), s.waiters.front()->id);
	}
}

[[noreturn]] void ExitProgram(int code)
{
	Log("kernel", "EE program exit (%d)", code);
	LogShutdown();
	std::fflush(stdout);
	std::fflush(stderr);
	_exit(code);
}

} // namespace vc::kernel
