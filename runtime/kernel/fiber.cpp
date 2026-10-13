// SPDX-License-Identifier: GPL-3.0-or-later

#include "fiber.h"

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace vc::kernel {

FiberStack::~FiberStack()
{
	if (base_)
		munmap(base_, mapped_);
}

bool FiberStack::Allocate(size_t size)
{
	const size_t page = static_cast<size_t>(getpagesize());
	size = (size + page - 1) & ~(page - 1);
	const size_t total = size + page;
	void* p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED)
		return false;
	mprotect(p, page, PROT_NONE); // guard page: stack overflow faults instead of corrupting
	base_ = p;
	mapped_ = total;
	top_ = static_cast<uint8_t*>(p) + total;
	return true;
}

void FiberInit(vc_fiber_ctx* ctx, void* stack_top, void (*entry)(void*), void* arg, uint64_t fpcr)
{
	std::memset(ctx, 0, sizeof *ctx);
	ctx->x[0] = reinterpret_cast<uint64_t>(entry);                 // x19
	ctx->x[1] = reinterpret_cast<uint64_t>(arg);                   // x20
	ctx->x[11] = reinterpret_cast<uint64_t>(&vc_fiber_trampoline); // x30
	ctx->sp = reinterpret_cast<uint64_t>(stack_top) & ~uint64_t{15};
	ctx->fpcr = fpcr;
}

} // namespace vc::kernel
