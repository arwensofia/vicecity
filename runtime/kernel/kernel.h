// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE kernel HLE: threads (fibers), semaphores, interrupt handlers (INTC/DMAC), alarms,
// vblank, timers, and the syscall table. Internal to runtime/kernel and runtime/mem.
//
// Execution model: all EE threads run on one host thread (the "EE host thread"), each on its
// own fiber. Scheduling follows the EE kernel: strict priorities (0 = highest), FIFO within a
// priority, no time slicing. A switch happens only inside kernel entry points (syscalls) and
// at interrupt delivery points (Poll), so the host never switches inside PCSX2/vc_gs code.
// Interrupts are delivered at Poll(): syscalls, EE hardware register accesses, `ei`, and
// while every thread is blocked (idle).

#pragma once

#include "ps2_abi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vc::kernel {

// ---------------------------------------------------------------------------------------
// Boot (threads.cpp)
// ---------------------------------------------------------------------------------------

struct BootConfig
{
	uint32_t entry = 0;
	std::vector<std::string> argv; // argv[0] is the executable path as the PS2 sees it
	bool log_syscalls = false;
};

// Set up the EE memory pointers (from vc::gs::GetMemory()).
void InitMemory(uint8_t* ram, uint8_t* spr, uint8_t* vu0_mem, uint8_t* vu0_micro, uint8_t* vu1_mem,
	uint8_t* vu1_micro);
// Run the EE program on the calling host thread until it calls Exit. Returns the exit code.
int Run(const BootConfig& cfg);
[[noreturn]] void ExitProgram(int code);

// ---------------------------------------------------------------------------------------
// Memory helpers (runtime/mem/mem.cpp)
// ---------------------------------------------------------------------------------------

extern uint8_t* g_ram;
extern uint8_t* g_spr;
extern uint8_t* g_vu0_mem;
extern uint8_t* g_vu0_micro;
extern uint8_t* g_vu1_mem;
extern uint8_t* g_vu1_micro;

// Host pointer for `size` bytes at an EE address in RAM (any mirror) or scratchpad.
uint8_t* EePtr(uint32_t addr, uint32_t size = 1);
uint32_t Load32(uint32_t addr);
void Store32(uint32_t addr, uint32_t v);
void Store64(uint32_t addr, uint64_t v);
std::string LoadCString(uint32_t addr, size_t max = 256);

// ---------------------------------------------------------------------------------------
// Time (intr.cpp)
// ---------------------------------------------------------------------------------------

uint64_t NowNs(); // since Run()

// ---------------------------------------------------------------------------------------
// Threads and semaphores (threads.cpp). Return values follow the EE kernel (ids >= 0,
// negative on error). Functions are safe to call from interrupt handlers where the EE
// kernel has an i-variant: they then defer the reschedule to the end of the interrupt.
// ---------------------------------------------------------------------------------------

constexpr int kMaxThreads = 256;
constexpr int kMaxSemas = 256;

enum ThreadStatus : int
{
	THS_RUN = 0x01,
	THS_READY = 0x02,
	THS_WAIT = 0x04,
	THS_SUSPEND = 0x08,
	THS_WAITSUSPEND = 0x0c,
	THS_DORMANT = 0x10,
};

ps2_cpu* CurrentCpu();
int CurrentThreadId();
bool ThreadInterruptsEnabled();        // COP0 Status.EIE of the running thread
void SetThreadInterruptsEnabled(bool); // di / ei / mtc0 Status
uint32_t ThreadCop0Status();
void SetThreadCop0Status(uint32_t);

uint32_t SetupThread(ps2_cpu* c, uint32_t gp, uint32_t stack, uint32_t stack_size, uint32_t args,
	uint32_t root);
uint32_t SetupHeap(uint32_t start, uint32_t size);
uint32_t EndOfHeap();

int32_t CreateThread(ps2_cpu* c, uint32_t param);
int32_t DeleteThread(int32_t id);
int32_t StartThread(int32_t id, uint32_t arg);
void ExitThread();
void ExitDeleteThread();
int32_t TerminateThread(int32_t id);
int32_t ChangeThreadPriority(int32_t id, int32_t prio);
int32_t RotateThreadReadyQueue(int32_t prio);
int32_t ReleaseWaitThread(int32_t id);
int32_t ReferThreadStatus(int32_t id, uint32_t info);
int32_t SleepThread();
int32_t WakeupThread(int32_t id);
int32_t CancelWakeupThread(int32_t id);
int32_t SuspendThread(int32_t id);
int32_t ResumeThread(int32_t id);

int32_t CreateSema(uint32_t param);
int32_t DeleteSema(int32_t id);
int32_t SignalSema(int32_t id);
int32_t WaitSema(int32_t id);
int32_t PollSema(int32_t id);
int32_t ReferSemaStatus(int32_t id, uint32_t info);

// Give the CPU to a higher-priority ready thread if there is one (after interrupts).
void Preempt();
// Log the state of all threads and semaphores (hang diagnostics).
void DumpThreads();

// ---------------------------------------------------------------------------------------
// Interrupts, alarms, vblank, timers (intr.cpp)
// ---------------------------------------------------------------------------------------

void InitInterrupts();
bool InInterrupt();
// Interrupt-context semantics for the i* syscalls (negative numbers) when they are called from
// thread context, e.g. from an RPC end function run synchronously by runtime/iop: the
// reschedule is deferred to the next interrupt delivery point instead of switching inside.
void BeginIContext();
void EndIContext();
bool InIContext(); // InInterrupt() or inside an i* syscall
// Called by thread functions when a higher-priority thread may have become ready.
void RequestReschedule();
bool TakeRescheduleRequest();

// vc::gs callbacks
void OnInt0(void* user);
void OnInt1(void* user);
void OnDmaStart(void* user, int channel);

// Interrupt delivery point for the running thread.
void Poll();
// Nothing is ready to run: wait for the next event and dispatch interrupts.
void IdleWait();
// A thread got the CPU (resets the all-threads-blocked detector).
void NoteNotIdle();

int32_t AddIntcHandler(int32_t cause, uint32_t func, int32_t next, uint32_t arg, uint32_t gp);
int32_t RemoveIntcHandler(int32_t cause, int32_t id);
int32_t AddDmacHandler(int32_t channel, uint32_t func, int32_t next, uint32_t arg, uint32_t gp);
int32_t RemoveDmacHandler(int32_t channel, int32_t id);
int32_t EnableIntcHandler(int32_t cause, int32_t id, bool enable);
int32_t EnableDmacHandler(int32_t channel, int32_t id, bool enable);
int32_t EnableIntc(int32_t cause);
int32_t DisableIntc(int32_t cause);
int32_t EnableDmac(int32_t channel);
int32_t DisableDmac(int32_t channel);

int32_t SetAlarm(uint32_t hsyncs, uint32_t func, uint32_t arg, uint32_t gp);
int32_t ReleaseAlarm(int32_t id);
void SetVSyncFlag(uint32_t flag_addr, uint32_t csr_addr);

// Timer registers 0x10000000-0x10001FFF.
uint32_t TimerRead(uint32_t addr);
void TimerWrite(uint32_t addr, uint32_t value);

uint64_t VBlankCount();
// Write the displayed image to a PPM file at the given vblank (debugging aid).
void AddScreenshot(uint64_t vblank, const std::string& path);

// Scratchpad DMA channels 8/9 (spr_dma.cpp): started via OnDmaStart, run after the register
// access that started them.
void SprDmaStart(int channel);
void SprDmaRunPending();
bool SprDmaPending();

// ---------------------------------------------------------------------------------------
// Indirect calls (dispatch.cpp)
// ---------------------------------------------------------------------------------------

ps2_func_t LookupFunction(uint32_t addr);
// Call an EE function with up to 4 arguments on context `c` (registers are clobbered as an
// EE call would). Returns v0.
uint32_t CallEE(ps2_cpu* c, uint32_t func, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0,
	uint32_t a3 = 0);

// Install SIGSEGV/SIGBUS/... handlers that print the EE state and host backtrace.
void InstallCrashHandlers();
// Print every function entry (needs recompiled code built with PS2_TRACE=1).
void SetTracePrint(bool on);
void DumpCpu(const ps2_cpu* c);

} // namespace vc::kernel
