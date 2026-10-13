// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loads the PT_LOAD segments of an EE ELF executable into EE RAM. The file is untrusted
// input: every header field is bounds-checked.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vc::app {

struct ElfImage
{
	uint32_t entry = 0;
	uint32_t lowest = 0xFFFFFFFFu;  // lowest loaded address
	uint32_t highest = 0;           // end of the highest segment (incl. .bss)
	int segments = 0;
};

// `ram`/`ram_size`: EE main RAM. Returns false with `error` set on malformed input.
bool LoadElf(const uint8_t* data, size_t size, uint8_t* ram, size_t ram_size, ElfImage* out, std::string* error);

} // namespace vc::app
