// SPDX-License-Identifier: GPL-3.0-or-later

#include "mc_fs.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace vc::iop {

namespace {

constexpr size_t kMaxDepth = 16;
constexpr int kClusterSize = 1024;
constexpr int kJstOffset = 9 * 3600;

McDateTime ToMcTime(time_t t)
{
	time_t jst = t + kJstOffset;
	struct tm tm;
	gmtime_r(&jst, &tm);
	McDateTime d{};
	d.sec = uint8_t(tm.tm_sec);
	d.min = uint8_t(tm.tm_min);
	d.hour = uint8_t(tm.tm_hour);
	d.day = uint8_t(tm.tm_mday);
	d.month = uint8_t(tm.tm_mon + 1);
	d.year = uint16_t(tm.tm_year + 1900);
	return d;
}

time_t FromMcTime(const McDateTime& d)
{
	struct tm tm{};
	tm.tm_sec = d.sec;
	tm.tm_min = d.min;
	tm.tm_hour = d.hour;
	tm.tm_mday = d.day;
	tm.tm_mon = d.month - 1;
	tm.tm_year = d.year - 1900;
	return timegm(&tm) - kJstOffset;
}

int Clusters(uint64_t bytes)
{
	return int((bytes + kClusterSize - 1) / kClusterSize);
}

// Host directory entries that belong to the card (skips host metadata such as .DS_Store).
std::vector<std::string> HostList(const std::string& dir)
{
	std::vector<std::string> names;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return names;
	while (struct dirent* e = readdir(d))
	{
		std::string n = e->d_name;
		if (!n.empty() && n[0] != '.' && McFs::ValidName(n))
			names.push_back(std::move(n));
	}
	closedir(d);
	return names;
}

bool IsDir(const std::string& p)
{
	struct stat st;
	return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool Exists(const std::string& p)
{
	struct stat st;
	return ::lstat(p.c_str(), &st) == 0;
}

std::string JoinCard(const std::vector<std::string>& parts)
{
	std::string s = "/";
	for (size_t i = 0; i < parts.size(); i++)
	{
		if (i)
			s += '/';
		s += parts[i];
	}
	return s;
}

} // namespace

McFs::McFs(std::string host_dir) : root_(std::move(host_dir))
{
	while (root_.size() > 1 && root_.back() == '/')
		root_.pop_back();
}

McFs::~McFs()
{
	CloseAll();
}

bool McFs::ValidName(std::string_view name)
{
	if (name.empty() || name.size() > kMaxName || name[0] == '.')
		return false;
	for (char c : name)
	{
		unsigned char u = static_cast<unsigned char>(c);
		if (u < 0x20 || u >= 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?')
			return false;
	}
	return true;
}

bool McFs::WildcardMatch(std::string_view p, std::string_view n)
{
	// Iterative glob with backtracking on the last '*'.
	size_t pi = 0, ni = 0, star = std::string_view::npos, mark = 0;
	while (ni < n.size())
	{
		if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni]))
		{
			pi++;
			ni++;
		}
		else if (pi < p.size() && p[pi] == '*')
		{
			star = pi++;
			mark = ni;
		}
		else if (star != std::string_view::npos)
		{
			pi = star + 1;
			ni = ++mark;
		}
		else
			return false;
	}
	while (pi < p.size() && p[pi] == '*')
		pi++;
	return pi == p.size();
}

bool McFs::Resolve(std::string_view path, std::vector<std::string>* parts) const
{
	parts->clear();
	if (path.empty() || path[0] != '/')
	{
		// Relative to the current directory (cwd_ is always a valid absolute path).
		std::string_view cwd = cwd_;
		size_t i = 1;
		while (i < cwd.size())
		{
			size_t j = cwd.find('/', i);
			if (j == std::string_view::npos)
				j = cwd.size();
			if (j > i)
				parts->emplace_back(cwd.substr(i, j - i));
			i = j + 1;
		}
	}
	size_t i = 0;
	while (i <= path.size())
	{
		size_t j = path.find('/', i);
		if (j == std::string_view::npos)
			j = path.size();
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
		if (!ValidName(comp))
			return false;
		parts->emplace_back(comp);
		if (parts->size() > kMaxDepth)
			return false;
	}
	return true;
}

std::string McFs::HostPath(const std::vector<std::string>& parts) const
{
	std::string p = root_;
	for (const std::string& s : parts)
	{
		p += '/';
		p += s;
	}
	return p;
}

bool McFs::Formatted() const
{
	return IsDir(root_);
}

bool McFs::SafeRoot() const
{
	// Format/Unformat delete the card directory recursively; refuse anything that does not
	// look like a card directory ("<somewhere>/mcN").
	size_t slash = root_.rfind('/');
	std::string base = slash == std::string::npos ? root_ : root_.substr(slash + 1);
	return slash != std::string::npos && slash > 0 && base.size() >= 3 && base.compare(0, 2, "mc") == 0;
}

int McFs::Format()
{
	if (!SafeRoot())
		return kMcDeniedPermit;
	CloseAll();
	std::error_code ec;
	std::filesystem::remove_all(root_, ec);
	std::filesystem::create_directories(root_, ec);
	cwd_ = "/";
	listing_.clear();
	return Formatted() ? kMcOk : kMcFullDevice;
}

int McFs::Unformat()
{
	if (!SafeRoot())
		return kMcDeniedPermit;
	CloseAll();
	std::error_code ec;
	std::filesystem::remove_all(root_, ec);
	cwd_ = "/";
	listing_.clear();
	return kMcOk;
}

void McFs::CloseAll()
{
	for (Handle& h : files_)
	{
		if (h.fd >= 0)
			::close(h.fd);
		h = Handle();
	}
}

int McFs::Open(std::string_view path, int flags)
{
	if (!Formatted())
		return kMcNoFormat;
	std::vector<std::string> parts;
	if (!Resolve(path, &parts) || parts.empty())
		return kMcNoEntry;
	std::vector<std::string> parent(parts.begin(), parts.end() - 1);
	if (!IsDir(HostPath(parent)))
		return kMcNoEntry;
	std::string host = HostPath(parts);

	if (flags & kMcCreateDir)
	{
		if (Exists(host))
			return kMcNoEntry;
		if (FreeClusters() < 1)
			return kMcFullDevice;
		if (::mkdir(host.c_str(), 0755) != 0)
			return kMcNoEntry;
		return kMcOk;
	}

	bool exists = Exists(host);
	if (exists && IsDir(host))
		return kMcDeniedPermit;
	if (!exists && !(flags & kMcCreate))
		return kMcNoEntry;
	if (!exists && FreeClusters() < 1)
		return kMcFullDevice;

	int slot = -1;
	for (int i = 0; i < kMaxFiles && slot < 0; i++)
		if (files_[i].fd < 0)
			slot = i;
	if (slot < 0)
		return kMcUpLimitHandle;

	int oflags = O_RDWR | O_CLOEXEC | O_NOFOLLOW;
	if (flags & kMcCreate)
		oflags |= O_CREAT;
	if (flags & kMcTrunc)
		oflags |= O_TRUNC;
	int fd = ::open(host.c_str(), oflags, 0644);
	if (fd < 0)
		return kMcNoEntry;
	Handle& h = files_[slot];
	h.fd = fd;
	h.readable = (flags & kMcRdOnly) != 0 || (flags & kMcRdWr) == 0;
	h.writable = (flags & kMcWrOnly) != 0;
	h.parts = std::move(parts);
	return slot;
}

int McFs::Close(int fd)
{
	if (fd < 0 || fd >= kMaxFiles || files_[fd].fd < 0)
		return kMcNoEntry;
	::close(files_[fd].fd);
	files_[fd] = Handle();
	return kMcOk;
}

int McFs::Read(int fd, void* buf, int size)
{
	if (fd < 0 || fd >= kMaxFiles || files_[fd].fd < 0)
		return kMcNoEntry;
	if (!files_[fd].readable)
		return kMcDeniedPermit;
	if (size <= 0)
		return 0;
	ssize_t n = ::read(files_[fd].fd, buf, size_t(size));
	return n < 0 ? kMcNoEntry : int(n);
}

int McFs::Write(int fd, const void* buf, int size)
{
	if (fd < 0 || fd >= kMaxFiles || files_[fd].fd < 0)
		return kMcNoEntry;
	Handle& h = files_[fd];
	if (!h.writable)
		return kMcDeniedPermit;
	if (size <= 0)
		return 0;
	struct stat st;
	if (::fstat(h.fd, &st) != 0)
		return kMcNoEntry;
	off_t pos = ::lseek(h.fd, 0, SEEK_CUR);
	uint64_t new_end = std::max<uint64_t>(uint64_t(st.st_size), uint64_t(pos) + uint64_t(size));
	int extra = Clusters(new_end) - Clusters(uint64_t(st.st_size));
	if (extra > 0 && extra > FreeClusters())
		return kMcFullDevice;
	ssize_t n = ::write(h.fd, buf, size_t(size));
	return n < 0 ? kMcFullDevice : int(n);
}

int McFs::Seek(int fd, int offset, int whence)
{
	if (fd < 0 || fd >= kMaxFiles || files_[fd].fd < 0)
		return kMcNoEntry;
	int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
	off_t r = ::lseek(files_[fd].fd, offset, w);
	if (r < 0 || r > INT32_MAX)
		return kMcNoEntry;
	return int(r);
}

int McFs::Flush(int fd)
{
	if (fd < 0 || fd >= kMaxFiles || files_[fd].fd < 0)
		return kMcNoEntry;
	return kMcOk;
}

int McFs::Delete(std::string_view path)
{
	if (!Formatted())
		return kMcNoFormat;
	std::vector<std::string> parts;
	if (!Resolve(path, &parts) || parts.empty())
		return kMcNoEntry;
	std::string host = HostPath(parts);
	if (!Exists(host))
		return kMcNoEntry;
	if (IsDir(host))
	{
		if (!HostList(host).empty())
			return kMcNotEmpty;
		// Drop host metadata files (.DS_Store) that would keep rmdir from succeeding.
		std::error_code ec;
		std::filesystem::remove_all(host, ec);
		return ec ? kMcDeniedPermit : kMcOk;
	}
	for (const Handle& h : files_)
		if (h.fd >= 0 && h.parts == parts)
			return kMcDeniedPermit;
	return ::unlink(host.c_str()) == 0 ? kMcOk : kMcDeniedPermit;
}

int McFs::Chdir(std::string_view path, std::string* prev)
{
	if (prev)
		*prev = cwd_;
	if (!Formatted())
		return kMcNoFormat;
	std::vector<std::string> parts;
	if (!Resolve(path, &parts) || !IsDir(HostPath(parts)))
		return kMcNoEntry;
	cwd_ = JoinCard(parts);
	return kMcOk;
}

int McFs::GetDir(std::string_view path, int cont, int maxent, std::vector<McDirEntry>* out)
{
	out->clear();
	if (!Formatted())
		return kMcNoFormat;
	if (!cont)
	{
		listing_.clear();
		listing_pos_ = 0;
		size_t slash = path.rfind('/');
		std::string_view dir_part, pattern;
		if (slash == std::string_view::npos)
		{
			dir_part = "";
			pattern = path;
		}
		else
		{
			dir_part = path.substr(0, slash + 1); // keep "/" so the root stays absolute
			pattern = path.substr(slash + 1);
		}
		if (pattern.empty())
			pattern = "*";
		if (pattern.size() > kMaxName)
			return kMcNoEntry;
		std::vector<std::string> parts;
		if (!Resolve(dir_part, &parts))
			return kMcNoEntry;
		std::string host = HostPath(parts);
		if (!IsDir(host))
			return kMcNoEntry;

		struct Item
		{
			std::string name;
			struct stat st;
		};
		std::vector<Item> items;
		for (std::string& n : HostList(host))
		{
			Item it{std::move(n), {}};
			if (::lstat((host + "/" + it.name).c_str(), &it.st) != 0)
				continue;
			if (!S_ISDIR(it.st.st_mode) && !S_ISREG(it.st.st_mode))
				continue;
			items.push_back(std::move(it));
		}
		std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
			if (a.st.st_birthtimespec.tv_sec != b.st.st_birthtimespec.tv_sec)
				return a.st.st_birthtimespec.tv_sec < b.st.st_birthtimespec.tv_sec;
			return a.name < b.name;
		});

		struct stat dst{};
		::stat(host.c_str(), &dst);
		auto make = [](std::string_view name, const struct stat& st, bool dir, uint32_t size) {
			McDirEntry e{};
			e.create = ToMcTime(st.st_birthtimespec.tv_sec);
			e.modify = ToMcTime(st.st_mtimespec.tv_sec);
			e.file_size = size;
			e.attr = uint16_t(kMcAttrExists | kMcAttrPS2 | kMcAttrReadable | kMcAttrWriteable |
			                  kMcAttrExecutable | (dir ? kMcAttrSubdir : (kMcAttrFile | kMcAttrClosed)));
			std::memcpy(e.name, name.data(), std::min(name.size(), sizeof(e.name) - 1));
			return e;
		};
		uint32_t self_count = uint32_t(items.size() + 2);
		if (WildcardMatch(pattern, "."))
			listing_.push_back(make(".", dst, true, self_count));
		if (WildcardMatch(pattern, ".."))
			listing_.push_back(make("..", dst, true, 0));
		for (const Item& it : items)
		{
			if (!WildcardMatch(pattern, it.name))
				continue;
			bool dir = S_ISDIR(it.st.st_mode);
			uint32_t size = dir ? uint32_t(HostList(host + "/" + it.name).size() + 2)
			                    : uint32_t(std::min<off_t>(it.st.st_size, INT32_MAX));
			listing_.push_back(make(it.name, it.st, dir, size));
		}
	}
	while (int(out->size()) < maxent && listing_pos_ < listing_.size())
		out->push_back(listing_[listing_pos_++]);
	return int(out->size());
}

int McFs::SetFileInfo(std::string_view path, const McDirEntry& info, int flags)
{
	if (!Formatted())
		return kMcNoFormat;
	std::vector<std::string> parts;
	if (!Resolve(path, &parts) || parts.empty())
		return kMcNoEntry;
	std::string host = HostPath(parts);
	if (!Exists(host))
		return kMcNoEntry;
	if (flags & kMcInfoModify)
	{
		struct timespec ts[2];
		ts[0].tv_sec = ts[1].tv_sec = FromMcTime(info.modify);
		ts[0].tv_nsec = ts[1].tv_nsec = 0;
		::utimensat(AT_FDCWD, host.c_str(), ts, AT_SYMLINK_NOFOLLOW);
	}
	// kMcInfoCreate and kMcInfoAttr are accepted; the host keeps its own creation time and
	// every entry reports the standard PS2 attributes.
	if (flags & kMcInfoName)
	{
		std::string name(info.name, strnlen(info.name, sizeof(info.name)));
		if (!ValidName(name))
			return kMcNoEntry;
		std::vector<std::string> to(parts.begin(), parts.end() - 1);
		to.push_back(name);
		std::string host_to = HostPath(to);
		if (host_to != host)
		{
			if (Exists(host_to))
				return kMcFailReplace;
			if (::rename(host.c_str(), host_to.c_str()) != 0)
				return kMcDeniedPermit;
		}
	}
	return kMcOk;
}

int McFs::GetEntSpace(std::string_view path)
{
	if (!Formatted())
		return kMcNoFormat;
	std::vector<std::string> parts;
	if (!Resolve(path, &parts))
		return kMcNoEntry;
	std::string host = HostPath(parts);
	if (!IsDir(host))
		return 0;
	// Two 512-byte entries per cluster; free slots in the directory's last cluster.
	int n = int(HostList(host).size()) + 2;
	return ((n + 1) / 2) * 2 - n;
}

int McFs::UsedClusters(const std::string& dir) const
{
	std::vector<std::string> names = HostList(dir);
	int used = (int(names.size()) + 2 + 1) / 2;
	for (const std::string& n : names)
	{
		std::string p = dir + "/" + n;
		struct stat st;
		if (::lstat(p.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			used += UsedClusters(p);
		else if (S_ISREG(st.st_mode))
			used += Clusters(uint64_t(st.st_size));
	}
	return used;
}

int McFs::FreeClusters() const
{
	if (!Formatted())
		return 0;
	// The root directory's own cluster is part of the formatted overhead.
	int used = UsedClusters(root_) - 1;
	return std::max(0, kTotalClusters - used);
}

} // namespace vc::iop
