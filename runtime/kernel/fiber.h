// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fibers (cooperative host contexts) for EE threads. Recompiled code keeps its return
// linkage on the host stack, so each EE thread runs on its own host stack and the EE kernel's
// context switch is a fiber switch (ps2_abi.h, "Threads").

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
struct vc_fiber_ctx
{
	uint64_t x[12]; // x19..x30
	uint64_t sp;
	uint64_t d[8];  // d8..d15
	uint64_t fpcr;
};
static_assert(sizeof(vc_fiber_ctx) == 176, "layout used by fiber_arm64.S");

void vc_fiber_switch(vc_fiber_ctx* from, const vc_fiber_ctx* to);
void vc_fiber_trampoline();
}

namespace vc::kernel {

// A host stack with a guard page below it.
class FiberStack
{
public:
	FiberStack() = default;
	FiberStack(const FiberStack&) = delete;
	FiberStack& operator=(const FiberStack&) = delete;
	~FiberStack();

	bool Allocate(size_t size);
	void* Top() const { return top_; }
	bool Valid() const { return base_ != nullptr; }

private:
	void* base_ = nullptr; // mapping start (guard page)
	size_t mapped_ = 0;
	void* top_ = nullptr;
};

// Prepare `ctx` so that switching to it calls entry(arg) on the given stack. `fpcr` is the
// floating-point control register value the fiber starts with.
void FiberInit(vc_fiber_ctx* ctx, void* stack_top, void (*entry)(void*), void* arg, uint64_t fpcr);

} // namespace vc::kernel
