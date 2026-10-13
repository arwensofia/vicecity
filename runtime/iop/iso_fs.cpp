// SPDX-License-Identifier: GPL-3.0-or-later

#include "iso_fs.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vc::iop {

namespace {

constexpr uint32_t kPvdSector = 16;
constexpr uint32_t kMaxDescriptors = 64;      // volume descriptor set scan limit
constexpr uint32_t kMaxDirBytes = 16u << 20;  // refuse absurd directory extents
constexpr size_t kMaxPathDepth = 32;
constexpr size_t kMaxNameLen = 255;

uint32_t Le32(const uint8_t* p)
{
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

char Upper(char c)
{
	return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
}

bool EqualsNoCase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (Upper(a[i]) != Upper(b[i]))
			return false;
	return true;
}

std::string TrimDiscName(std::string_view n)
{
	size_t semi = n.find(';');
	if (semi != std::string_view::npos)
		n = n.substr(0, semi);
	// ISO9660 level 1 writes "NAME." for names without extension.
	if (!n.empty() && n.back() == '.')
		n.remove_suffix(1);
	return std::string(n);
}

} // namespace

IsoImage::~IsoImage()
{
	Close();
}

void IsoImage::Close()
{
	if (fd_ >= 0)
		::close(fd_);
	fd_ = -1;
	file_size_ = 0;
	sectors_ = 0;
	volume_id_.clear();
	std::lock_guard<std::mutex> lock(mutex_);
	dirs_.clear();
}

bool IsoImage::Open(const std::string& path, std::string* error)
{
	auto fail = [&](const std::string& msg) {
		if (error)
			*error = msg;
		Close();
		return false;
	};
	Close();
	fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd_ < 0)
		return fail("cannot open '" + path + "': " + std::strerror(errno));
	struct stat st;
	if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode))
		return fail("'" + path + "' is not a regular file");
	file_size_ = uint64_t(st.st_size);
	sectors_ = file_size_ / kSectorSize;
	if (sectors_ <= kPvdSector)
		return fail("image too small");

	// Scan the volume descriptor set for the primary volume descriptor.
	uint8_t vd[kSectorSize];
	bool found = false;
	for (uint32_t i = 0; i < kMaxDescriptors && kPvdSector + i < sectors_; i++)
	{
		if (!ReadSectors(kPvdSector + i, 1, vd))
			return fail("read error in volume descriptors");
		if (std::memcmp(vd + 1, "CD001", 5) != 0)
			return fail("no ISO9660 volume descriptor at sector 16");
		if (vd[0] == 255)
			break;
		if (vd[0] == 1)
		{
			found = true;
			break;
		}
	}
	if (!found)
		return fail("no ISO9660 primary volume descriptor");
	if (vd[6] != 1)
		return fail("unsupported volume descriptor version");
	uint16_t block_size = uint16_t(vd[128] | vd[129] << 8);
	if (block_size != kSectorSize)
		return fail("unsupported logical block size");

	std::string vid(reinterpret_cast<const char*>(vd + 40), 32);
	while (!vid.empty() && (vid.back() == ' ' || vid.back() == '\0'))
		vid.pop_back();
	volume_id_ = vid;

	bool self = false;
	if (!ParseRecord(vd + 156, 34, &root_, &self) || !root_.is_dir)
		return fail("malformed root directory record");
	root_.name.clear();
	if (!LoadDir(root_.lsn, root_.size))
		return fail("malformed root directory");
	return true;
}

bool IsoImage::ReadSectors(uint32_t lsn, uint32_t count, void* dst)
{
	if (fd_ < 0)
		return false;
	uint64_t end = uint64_t(lsn) + count;
	if (end > sectors_)
		return false;
	size_t len = size_t(count) * kSectorSize;
	uint64_t off = uint64_t(lsn) * kSectorSize;
	auto* p = static_cast<uint8_t*>(dst);
	while (len > 0)
	{
		ssize_t n = ::pread(fd_, p, len, off_t(off));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		p += n;
		off += uint64_t(n);
		len -= size_t(n);
	}
	return true;
}

int64_t IsoImage::ReadBytes(uint64_t offset, void* dst, size_t len)
{
	if (fd_ < 0)
		return -1;
	if (offset >= file_size_)
		return 0;
	len = size_t(std::min<uint64_t>(len, file_size_ - offset));
	auto* p = static_cast<uint8_t*>(dst);
	size_t done = 0;
	while (done < len)
	{
		ssize_t n = ::pread(fd_, p + done, len - done, off_t(offset + done));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (n == 0)
			break;
		done += size_t(n);
	}
	return int64_t(done);
}

int64_t IsoImage::ReadFile(const IsoEntry& file, uint64_t offset, void* dst, size_t len)
{
	if (offset >= file.size)
		return 0;
	len = size_t(std::min<uint64_t>(len, file.size - offset));
	return ReadBytes(uint64_t(file.lsn) * kSectorSize + offset, dst, len);
}

bool IsoImage::ParseRecord(const uint8_t* rec, size_t avail, IsoEntry* out, bool* is_self_or_parent)
{
	if (avail < 34)
		return false;
	uint8_t len = rec[0];
	uint8_t ext_attr = rec[1];
	uint8_t name_len = rec[32];
	if (len < 34 || len > avail || name_len == 0 || size_t(33) + name_len > len)
		return false;
	uint64_t lsn = uint64_t(Le32(rec + 2)) + ext_attr;
	uint32_t size = Le32(rec + 10);
	uint64_t sectors = (uint64_t(size) + kSectorSize - 1) / kSectorSize;
	if (lsn > UINT32_MAX || lsn + sectors > sectors_)
		return false;
	out->lsn = uint32_t(lsn);
	out->size = size;
	out->is_dir = (rec[25] & 0x02) != 0;
	std::memcpy(out->date, rec + 18, 7);
	const char* name = reinterpret_cast<const char*>(rec + 33);
	*is_self_or_parent = name_len == 1 && (name[0] == 0 || name[0] == 1);
	out->name = *is_self_or_parent ? std::string() : TrimDiscName(std::string_view(name, name_len));
	return true;
}

const IsoImage::Dir* IsoImage::LoadDir(uint32_t lsn, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = dirs_.find(lsn);
	if (it != dirs_.end())
		return &it->second;
	if (size == 0 || size > kMaxDirBytes)
		return nullptr;
	uint32_t count = (size + kSectorSize - 1) / kSectorSize;
	std::vector<uint8_t> buf(size_t(count) * kSectorSize);
	if (!ReadSectors(lsn, count, buf.data()))
		return nullptr;
	Dir dir;
	for (uint32_t s = 0; s < count; s++)
	{
		const uint8_t* sec = buf.data() + size_t(s) * kSectorSize;
		size_t sec_end = std::min<size_t>(kSectorSize, size - size_t(s) * kSectorSize);
		size_t pos = 0;
		while (pos < sec_end)
		{
			if (sec[pos] == 0)
				break; // rest of the sector is padding; records never cross sectors
			IsoEntry e;
			bool special = false;
			if (!ParseRecord(sec + pos, sec_end - pos, &e, &special))
				return nullptr;
			pos += sec[pos];
			if (special || e.name.empty())
				continue;
			// Multi-extent files (flag 0x80) only appear for files >= 4 GB, which PS2 discs
			// do not have; each extent would be listed as a separate record with the same
			// name. Keep the first.
			bool dup = false;
			for (const IsoEntry& o : dir.entries)
				dup = dup || EqualsNoCase(o.name, e.name);
			if (!dup)
				dir.entries.push_back(std::move(e));
		}
	}
	return &dirs_.emplace(lsn, std::move(dir)).first->second;
}

bool IsoImage::SplitPath(std::string_view path, std::vector<std::string>* parts)
{
	parts->clear();
	size_t colon = path.find(':');
	if (colon != std::string_view::npos)
		path = path.substr(colon + 1);
	size_t i = 0;
	while (i <= path.size())
	{
		size_t j = i;
		while (j < path.size() && path[j] != '\\' && path[j] != '/')
			j++;
		std::string_view comp = path.substr(i, j - i);
		i = j + 1;
		if (comp.empty() || comp == ".")
			continue;
		if (comp == "..")
		{
			if (!parts->empty())
				parts->pop_back();
			continue;
		}
		std::string name = TrimDiscName(comp);
		if (name.empty() || name.size() > kMaxNameLen)
			return false;
		for (char& ch : name)
		{
			if (ch == '\0')
				return false;
			ch = Upper(ch);
		}
		parts->push_back(std::move(name));
		if (parts->size() > kMaxPathDepth)
			return false;
	}
	return true;
}

bool IsoImage::Find(std::string_view path, IsoEntry* out)
{
	if (fd_ < 0)
		return false;
	std::vector<std::string> parts;
	if (!SplitPath(path, &parts))
		return false;
	IsoEntry cur = root_;
	for (const std::string& part : parts)
	{
		if (!cur.is_dir)
			return false;
		const Dir* dir = LoadDir(cur.lsn, cur.size);
		if (!dir)
			return false;
		const IsoEntry* next = nullptr;
		for (const IsoEntry& e : dir->entries)
		{
			if (EqualsNoCase(e.name, part))
			{
				next = &e;
				break;
			}
		}
		if (!next)
			return false;
		cur = *next;
	}
	*out = cur;
	return true;
}

bool IsoImage::ListDir(std::string_view path, std::vector<IsoEntry>* out)
{
	IsoEntry dir_entry;
	if (!Find(path, &dir_entry) || !dir_entry.is_dir)
		return false;
	const Dir* dir = LoadDir(dir_entry.lsn, dir_entry.size);
	if (!dir)
		return false;
	*out = dir->entries;
	return true;
}

} // namespace vc::iop
