// SPDX-License-Identifier: GPL-3.0-or-later
#include "image.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace recomp {

bool Image::read32(uint32_t addr, uint32_t &out) const
{
	if (addr & 3) return false;
	for (const Segment &s : segments) {
		if (addr >= s.vaddr && addr - s.vaddr + 4 <= s.data.size()) {
			const uint8_t *p = s.data.data() + (addr - s.vaddr);
			out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			      (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
			return true;
		}
	}
	return false;
}

bool Image::is_excluded(uint32_t addr) const
{
	for (const auto &[a, b] : excluded)
		if (addr >= a && addr < b) return true;
	return false;
}

bool Image::is_code(uint32_t addr) const
{
	if ((addr & 3) || is_excluded(addr)) return false;
	for (const Segment &s : segments)
		if ((s.flags & 1) && addr >= s.vaddr && addr - s.vaddr + 4 <= s.data.size()) return true;
	return false;
}

uint32_t Image::code_end(uint32_t addr) const
{
	for (const Segment &s : segments)
		if ((s.flags & 1) && addr >= s.vaddr && addr - s.vaddr < s.data.size())
			return s.vaddr + static_cast<uint32_t>(s.data.size());
	return addr;
}

namespace {
uint16_t rd16(const std::vector<uint8_t> &b, size_t o) { return static_cast<uint16_t>(b[o] | (b[o + 1] << 8)); }
uint32_t rd32(const std::vector<uint8_t> &b, size_t o)
{
	return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
	       (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}
} // namespace

bool Image::load_elf(const std::string &path, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open " + path;
		return false;
	}
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	constexpr size_t kMaxFile = 64u << 20;
	if (b.size() < 52 || b.size() > kMaxFile) {
		err = "bad ELF size";
		return false;
	}
	if (std::memcmp(b.data(), "\x7f" "ELF", 4) != 0 || b[4] != 1 /*ELFCLASS32*/ || b[5] != 1 /*LE*/) {
		err = "not a 32-bit little-endian ELF";
		return false;
	}
	if (rd16(b, 18) != 8 /*EM_MIPS*/) {
		err = "not a MIPS ELF";
		return false;
	}
	entry = rd32(b, 24);
	const uint32_t phoff = rd32(b, 28);
	const uint16_t phentsize = rd16(b, 42), phnum = rd16(b, 44);
	if (phentsize < 32 || phnum == 0 || phnum > 64 ||
	    static_cast<uint64_t>(phoff) + static_cast<uint64_t>(phentsize) * phnum > b.size()) {
		err = "bad program header table";
		return false;
	}
	segments.clear();
	for (unsigned i = 0; i < phnum; i++) {
		size_t o = phoff + static_cast<size_t>(i) * phentsize;
		uint32_t type = rd32(b, o), off = rd32(b, o + 4), vaddr = rd32(b, o + 8);
		uint32_t filesz = rd32(b, o + 16), memsz = rd32(b, o + 20), flags = rd32(b, o + 24);
		if (type != 1 /*PT_LOAD*/ || memsz == 0) continue;
		if (filesz > memsz || static_cast<uint64_t>(off) + filesz > b.size() ||
		    static_cast<uint64_t>(vaddr) + memsz > 0x100000000ull || memsz > (64u << 20)) {
			err = "bad PT_LOAD segment";
			return false;
		}
		Segment s;
		s.vaddr = vaddr;
		s.memsz = memsz;
		s.flags = flags;
		s.data.assign(b.begin() + off, b.begin() + off + filesz);
		segments.push_back(std::move(s));
	}
	if (segments.empty()) {
		err = "no loadable segments";
		return false;
	}
	// Executable sections (SHF_EXECINSTR), if a section table is present. The
	// game has a single RWX segment, so these are the only way to tell .text
	// from .data. Malformed tables are ignored (sections are optional).
	exec_sections.clear();
	const uint32_t shoff = rd32(b, 32);
	const uint16_t shentsize = rd16(b, 46), shnum = rd16(b, 48);
	if (shoff && shentsize >= 40 && shnum <= 1024 &&
	    static_cast<uint64_t>(shoff) + static_cast<uint64_t>(shentsize) * shnum <= b.size()) {
		for (unsigned i = 0; i < shnum; i++) {
			size_t o = shoff + static_cast<size_t>(i) * shentsize;
			uint32_t type = rd32(b, o + 4), flags = rd32(b, o + 8), addr = rd32(b, o + 12), size = rd32(b, o + 20);
			if (type == 1 /*PROGBITS*/ && (flags & 4) /*EXECINSTR*/ && size && static_cast<uint64_t>(addr) + size <= 0x100000000ull)
				exec_sections.emplace_back(addr, addr + size);
		}
	}
	return true;
}

bool Image::in_exec_section(uint32_t addr) const
{
	if (exec_sections.empty()) return is_code(addr);
	for (const auto &[a, e] : exec_sections)
		if (addr >= a && addr < e) return is_code(addr);
	return false;
}

bool read_csv(const std::string &path, std::vector<std::string> &header,
              std::vector<std::vector<std::string>> &rows, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open " + path;
		return false;
	}
	std::string line;
	bool first = true;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		std::vector<std::string> fields;
		std::string cur;
		bool quoted = false;
		for (size_t k = 0; k < line.size(); k++) {
			char ch = line[k];
			if (ch == '"') {
				if (quoted && k + 1 < line.size() && line[k + 1] == '"') {
					cur.push_back('"');
					k++;
				} else {
					quoted = !quoted;
				}
			} else if (ch == ',' && !quoted) {
				fields.push_back(cur);
				cur.clear();
			} else {
				cur.push_back(ch);
			}
		}
		fields.push_back(cur);
		if (first) {
			header = std::move(fields);
			first = false;
		} else {
			rows.push_back(std::move(fields));
		}
	}
	return true;
}

bool load_function_csv(const std::string &path, std::vector<FunctionDef> &out, std::string &err)
{
	std::ifstream f(path);
	if (!f) {
		err = "cannot open " + path;
		return false;
	}
	std::string line;
	bool header = true;
	int lineno = 0;
	while (std::getline(f, line)) {
		lineno++;
		if (header) {
			header = false;
			if (line.rfind("address", 0) == 0) continue;
		}
		if (line.empty()) continue;
		// address,size,namespace,name,"comment" -- only the first four fields matter.
		std::vector<std::string> fields;
		std::string cur;
		bool quoted = false;
		for (char ch : line) {
			if (ch == '"') quoted = !quoted;
			else if (ch == ',' && !quoted) {
				fields.push_back(cur);
				cur.clear();
			} else cur.push_back(ch);
		}
		fields.push_back(cur);
		if (fields.size() < 4) {
			err = "malformed line " + std::to_string(lineno);
			return false;
		}
		FunctionDef d;
		try {
			d.addr = static_cast<uint32_t>(std::stoul(fields[0], nullptr, 16));
			d.size = static_cast<uint32_t>(std::stoul(fields[1], nullptr, 10));
		} catch (...) {
			err = "bad number on line " + std::to_string(lineno);
			return false;
		}
		d.name = fields[2].empty() ? fields[3] : fields[2] + "::" + fields[3];
		out.push_back(std::move(d));
	}
	return true;
}

} // namespace recomp
