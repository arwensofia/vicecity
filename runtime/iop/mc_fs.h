// SPDX-License-Identifier: GPL-3.0-or-later
//
// vc::iop::McFs — a PS2 memory card (8 MB, PS2 type) whose filesystem lives in a host
// directory, with the semantics mcman exposes through libmc: one root, nested directories,
// file handles, open flags, "." / ".." entries, wildcard directory listing, 1 KB clusters.
//
// Card paths come from the game and are treated as untrusted: they are resolved logically
// (".." never leaves the card root), every component is validated (length <= 31, no path
// separators or control characters, no leading '.'), and only the resulting component list
// is turned into a host path below the card directory.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vc::iop {

// libmc result codes.
enum McResult : int
{
	kMcOk = 0,
	kMcChangedCard = -1,
	kMcNoFormat = -2,
	kMcFullDevice = -3,
	kMcNoEntry = -4,
	kMcDeniedPermit = -5,
	kMcNotEmpty = -6,
	kMcUpLimitHandle = -7,
	kMcFailReplace = -8,
	kMcNoCard = -10,
};

// sceMcFileOpen flags.
enum : int
{
	kMcRdOnly = 0x0001,
	kMcWrOnly = 0x0002,
	kMcRdWr = 0x0003,
	kMcCreateDir = 0x0040,
	kMcCreate = 0x0200,
	kMcTrunc = 0x0400,
};

// sceMcTblGetDir attribute bits.
enum : uint16_t
{
	kMcAttrReadable = 0x0001,
	kMcAttrWriteable = 0x0002,
	kMcAttrExecutable = 0x0004,
	kMcAttrFile = 0x0010,
	kMcAttrSubdir = 0x0020,
	kMcAttrClosed = 0x0080,
	kMcAttrPS2 = 0x0400,
	kMcAttrExists = 0x8000,
};

// sceMcSetFileInfo flags.
enum : int
{
	kMcInfoCreate = 0x01,
	kMcInfoModify = 0x02,
	kMcInfoAttr = 0x04,
	kMcInfoName = 0x10,
};

#pragma pack(push, 1)
struct McDateTime // sceMcStDateTime (JST, like the PS2 RTC)
{
	uint8_t resv;
	uint8_t sec;
	uint8_t min;
	uint8_t hour;
	uint8_t day;
	uint8_t month;
	uint16_t year;
};

struct McDirEntry // sceMcTblGetDir, 64 bytes in EE memory
{
	McDateTime create;
	McDateTime modify;
	uint32_t file_size;
	uint16_t attr;
	uint16_t resv1;
	uint32_t resv2;
	uint32_t pda_apl_no;
	char name[32];
};
#pragma pack(pop)
static_assert(sizeof(McDirEntry) == 64, "sceMcTblGetDir layout");

class McFs
{
public:
	static constexpr int kMaxFiles = 16;        // simultaneously open handles
	static constexpr int kTotalClusters = 8000; // usable 1 KB clusters on a formatted 8 MB card
	static constexpr size_t kMaxName = 31;

	// `host_dir` is the card's root directory on the host, e.g.
	// "~/Library/Application Support/vicecity/mc0". It is created on Format().
	explicit McFs(std::string host_dir);
	~McFs();
	McFs(const McFs&) = delete;
	McFs& operator=(const McFs&) = delete;

	const std::string& HostDir() const { return root_; }
	bool Formatted() const;
	int Format();   // creates an empty card (removes existing contents)
	int Unformat(); // removes everything, card becomes unformatted

	// Returns a handle (>= 0) or a McResult.
	int Open(std::string_view path, int flags);
	int Close(int fd);
	int Read(int fd, void* buf, int size);
	int Write(int fd, const void* buf, int size);
	int Seek(int fd, int offset, int whence); // whence 0 = SET, 1 = CUR, 2 = END
	int Flush(int fd);
	void CloseAll();

	int Mkdir(std::string_view path) { return Open(path, kMcCreateDir); }
	int Delete(std::string_view path);
	// Changes the current directory; `prev` receives the previous one (absolute card path).
	int Chdir(std::string_view path, std::string* prev);
	const std::string& Cwd() const { return cwd_; }
	// Lists entries matching the last path component ('*' and '?' wildcards). `cont` != 0
	// continues the previous listing. Returns the number of entries stored (<= maxent).
	int GetDir(std::string_view path, int cont, int maxent, std::vector<McDirEntry>* out);
	int SetFileInfo(std::string_view path, const McDirEntry& info, int flags);
	int GetEntSpace(std::string_view path);
	int FreeClusters() const;

	// Resolves a card path against the current directory. Returns false for invalid names.
	bool Resolve(std::string_view path, std::vector<std::string>* parts) const;
	static bool ValidName(std::string_view name);
	static bool WildcardMatch(std::string_view pattern, std::string_view name);

private:
	struct Handle
	{
		int fd = -1; // host descriptor
		bool readable = false;
		bool writable = false;
		std::vector<std::string> parts;
	};

	std::string HostPath(const std::vector<std::string>& parts) const;
	bool SafeRoot() const;
	int UsedClusters(const std::string& host_dir) const;

	std::string root_;
	std::string cwd_ = "/";
	Handle files_[kMaxFiles];
	std::vector<McDirEntry> listing_; // for GetDir continuation
	size_t listing_pos_ = 0;
};

} // namespace vc::iop
