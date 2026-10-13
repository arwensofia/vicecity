// SPDX-License-Identifier: GPL-3.0-or-later

#include "elf_loader.h"

#include <algorithm>
#include <cstring>

namespace vc::app {

namespace {

template <typename T>
bool Read(const uint8_t* data, size_t size, size_t off, T* out)
{
	if (off > size || size - off < sizeof(T))
		return false;
	std::memcpy(out, data + off, sizeof(T));
	return true;
}

} // namespace

bool LoadElf(const uint8_t* data, size_t size, uint8_t* ram, size_t ram_size, ElfImage* out, std::string* error)
{
	auto fail = [&](const char* msg) {
		*error = msg;
		return false;
	};
	if (size < 52 || std::memcmp(data, "\x7f" "ELF", 4) != 0)
		return fail("not an ELF file");
	if (data[4] != 1 || data[5] != 1)
		return fail("not a 32-bit little-endian ELF");
	uint16_t machine = 0, phentsize = 0, phnum = 0;
	uint32_t entry = 0, phoff = 0;
	Read(data, size, 18, &machine);
	Read(data, size, 24, &entry);
	Read(data, size, 28, &phoff);
	Read(data, size, 42, &phentsize);
	Read(data, size, 44, &phnum);
	if (machine != 8)
		return fail("not a MIPS executable");
	if (phentsize < 32 || phnum == 0 || phnum > 64)
		return fail("bad program header table");

	ElfImage img;
	img.entry = entry;
	for (uint32_t i = 0; i < phnum; i++) {
		const size_t ph = static_cast<size_t>(phoff) + static_cast<size_t>(i) * phentsize;
		uint32_t type, offset, vaddr, filesz, memsz;
		if (!Read(data, size, ph + 0, &type) || !Read(data, size, ph + 4, &offset) ||
			!Read(data, size, ph + 8, &vaddr) || !Read(data, size, ph + 16, &filesz) ||
			!Read(data, size, ph + 20, &memsz))
			return fail("truncated program header");
		if (type != 1 /* PT_LOAD */ || memsz == 0)
			continue;
		const uint32_t phys = vaddr & 0x1FFFFFFFu;
		if (filesz > memsz || phys >= ram_size || memsz > ram_size - phys)
			return fail("segment outside EE RAM");
		if (offset > size || filesz > size - offset)
			return fail("segment outside the file");
		std::memcpy(ram + phys, data + offset, filesz);
		std::memset(ram + phys + filesz, 0, memsz - filesz); // .bss
		img.lowest = std::min(img.lowest, phys);
		img.highest = std::max(img.highest, phys + memsz);
		img.segments++;
	}
	if (!img.segments)
		return fail("no loadable segments");
	*out = img;
	return true;
}

} // namespace vc::app
