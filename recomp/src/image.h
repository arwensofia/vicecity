// SPDX-License-Identifier: GPL-3.0-or-later
// Memory image of the EE executable (ELF loader) and function list input.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace recomp {

struct Segment {
	uint32_t vaddr = 0;
	uint32_t memsz = 0;
	uint32_t flags = 0; // PF_X = 1, PF_W = 2, PF_R = 4
	std::vector<uint8_t> data; // file-backed part (filesz bytes)
};

class Image {
public:
	std::vector<Segment> segments;
	uint32_t entry = 0;
	std::vector<std::pair<uint32_t, uint32_t>> excluded; // [start, end) never code
	std::vector<std::pair<uint32_t, uint32_t>> exec_sections; // [start, end) of SHF_EXECINSTR sections

	bool is_excluded(uint32_t addr) const;

	// Read a little-endian word from file-backed data. False if unmapped.
	bool read32(uint32_t addr, uint32_t &out) const;
	// True if addr lies in the file-backed part of an executable segment.
	bool is_code(uint32_t addr) const;
	uint32_t code_end(uint32_t addr) const;
	// is_code() and inside an executable ELF section (when the ELF has sections).
	bool in_exec_section(uint32_t addr) const; // end of the executable segment containing addr

	// Loads an ELF32 little-endian MIPS executable. The file is untrusted:
	// every offset/size is bounds-checked. Returns false with a message.
	bool load_elf(const std::string &path, std::string &err);
};

struct FunctionDef {
	uint32_t addr = 0;
	uint32_t size = 0;
	std::string name;
};

// Generic CSV reader (double-quoted fields may contain commas; "" is a quote).
// The first line is treated as a header and returned in `header`.
bool read_csv(const std::string &path, std::vector<std::string> &header,
              std::vector<std::vector<std::string>> &rows, std::string &err);

// Parses out/functions.csv (address,size,namespace,name,comment).
bool load_function_csv(const std::string &path, std::vector<FunctionDef> &out, std::string &err);

} // namespace recomp
