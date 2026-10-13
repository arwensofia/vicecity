// SPDX-License-Identifier: GPL-3.0-or-later
//
// vc::iop::IsoImage — read-only access to the user's PS2 DVD image through its ISO9660 view
// (PS2 discs are UDF 1.02 bridge images; the IOP's cdvdman only uses ISO9660). The image is
// untrusted input: every directory record, extent and read is bounds-checked against the
// image size, and malformed structures are rejected instead of trusted.
//
// Paths are PS2-style and matched case-insensitively: "cdrom0:\DATA\DEFAULT.DAT;1",
// "\DATA\DEFAULT.DAT", "data/default.dat" all name the same file. The device prefix (up to
// the first ':') and the ";N" version suffix are optional.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vc::iop {

constexpr uint32_t kSectorSize = 2048;

struct IsoEntry
{
	std::string name;    // as on disc, without ";1" (uppercase on PS2 discs)
	uint32_t lsn = 0;    // first sector
	uint32_t size = 0;   // bytes
	bool is_dir = false;
	// ISO9660 recording time: years since 1900, month, day, hour, minute, second, GMT offset
	// in 15-minute units.
	uint8_t date[7] = {};
};

class IsoImage
{
public:
	IsoImage() = default;
	~IsoImage();
	IsoImage(const IsoImage&) = delete;
	IsoImage& operator=(const IsoImage&) = delete;

	// Opens the image read-only and validates the primary volume descriptor.
	bool Open(const std::string& path, std::string* error = nullptr);
	void Close();
	bool IsOpen() const { return fd_ >= 0; }

	uint64_t SectorCount() const { return sectors_; }
	const std::string& VolumeId() const { return volume_id_; }

	// Looks up a file or directory. Returns false if it does not exist or the path is malformed.
	bool Find(std::string_view path, IsoEntry* out);
	// Lists a directory (without "." and ".."). Returns false if `path` is not a directory.
	bool ListDir(std::string_view path, std::vector<IsoEntry>* out);

	// Reads `count` 2048-byte sectors starting at `lsn`. Fails (and reads nothing) if any part
	// of the range lies outside the image.
	bool ReadSectors(uint32_t lsn, uint32_t count, void* dst);
	// Reads up to `len` bytes at byte offset `offset` of the image. Returns bytes read (short
	// at the end of the image), or -1 on error.
	int64_t ReadBytes(uint64_t offset, void* dst, size_t len);
	// Reads up to `len` bytes of a file at `offset` (clamped to the file size).
	int64_t ReadFile(const IsoEntry& file, uint64_t offset, void* dst, size_t len);

	// Splits a PS2 path into upper-cased components without device prefix and version
	// suffix. Returns false for malformed paths (empty components are skipped; "." is
	// dropped; ".." pops a component, but never above the root).
	static bool SplitPath(std::string_view path, std::vector<std::string>* parts);

private:
	struct Dir
	{
		std::vector<IsoEntry> entries;
	};

	const Dir* LoadDir(uint32_t lsn, uint32_t size); // cached; nullptr on malformed data
	bool ParseRecord(const uint8_t* rec, size_t avail, IsoEntry* out, bool* is_self_or_parent);

	int fd_ = -1;
	uint64_t file_size_ = 0;
	uint64_t sectors_ = 0;
	std::string volume_id_;
	IsoEntry root_;
	std::mutex mutex_; // guards dirs_
	std::unordered_map<uint32_t, Dir> dirs_;
};

} // namespace vc::iop
