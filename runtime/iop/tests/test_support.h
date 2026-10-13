// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test harness for runtime/iop: fake EE memory and the KERNEL PROVIDES half of vc_iop.h.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include "ps2_abi.h"

namespace iop_test {

extern int g_failures;

// 32 MB EE RAM + scratchpad, and a ps2_cpu pointing at them.
uint8_t* Ram();
ps2_cpu* Cpu();
// Registers a host function that ps2_call_indirect runs for EE address `addr`.
void SetEeFunction(uint32_t addr, std::function<void(ps2_cpu*)> fn);
int CallCount(uint32_t addr);
// Creates an empty temporary directory and returns its path.
std::string TempDir(const char* tag);

} // namespace iop_test

#define CHECK(cond)                                                                         \
	do {                                                                                    \
		if (!(cond)) {                                                                      \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
			iop_test::g_failures++;                                                         \
		}                                                                                   \
	} while (0)

#define CHECK_EQ(a, b)                                                                      \
	do {                                                                                    \
		long long va_ = (long long)(a), vb_ = (long long)(b);                               \
		if (va_ != vb_) {                                                                   \
			std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n",       \
			             __FILE__, __LINE__, #a, #b, va_, vb_);                             \
			iop_test::g_failures++;                                                         \
		}                                                                                   \
	} while (0)
