// SPDX-License-Identifier: GPL-3.0-or-later
//
// Memory card filesystem semantics (McFs) on a temporary host directory.

#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "mc_fs.h"
#include "test_support.h"

using namespace vc::iop;

namespace {

std::vector<std::string> Names(McFs& mc, const char* path)
{
	std::vector<McDirEntry> ents;
	std::vector<std::string> names;
	int n = mc.GetDir(path, 0, 64, &ents);
	for (int i = 0; i < n; i++)
		names.emplace_back(ents[size_t(i)].name);
	return names;
}

} // namespace

int main()
{
	std::string base = iop_test::TempDir("mc");
	McFs mc(base + "/mc0");

	// Unformatted card.
	CHECK(!mc.Formatted());
	CHECK_EQ(mc.Open("/x", kMcCreate | kMcWrOnly), kMcNoFormat);
	CHECK_EQ(mc.Format(), kMcOk);
	CHECK(mc.Formatted());
	CHECK_EQ(mc.FreeClusters(), McFs::kTotalClusters);

	// Refuses to format a directory that is not a card directory.
	McFs notcard(base);
	CHECK_EQ(notcard.Format(), kMcDeniedPermit);

	// Directories.
	CHECK_EQ(mc.Mkdir("/BASLUS-20552GTA40000"), kMcOk);
	CHECK_EQ(mc.Mkdir("/BASLUS-20552GTA40000"), kMcNoEntry); // exists
	CHECK_EQ(mc.Mkdir("/MISSING/SUB"), kMcNoEntry);          // parent missing
	CHECK_EQ(mc.Mkdir("/.hidden"), kMcNoEntry);              // invalid name
	CHECK_EQ(mc.Mkdir(std::string("/") + std::string(32, 'A')), kMcNoEntry);

	// Files: create, write, read back, seek.
	int fd = mc.Open("/BASLUS-20552GTA40000/SAVE1", kMcCreate | kMcRdWr);
	CHECK(fd >= 0);
	std::vector<uint8_t> data(5000);
	for (size_t i = 0; i < data.size(); i++)
		data[i] = uint8_t(i * 13);
	CHECK_EQ(mc.Write(fd, data.data(), int(data.size())), 5000);
	CHECK_EQ(mc.Seek(fd, 0, 1), 5000);
	CHECK_EQ(mc.Seek(fd, 100, 0), 100);
	uint8_t buf[64];
	CHECK_EQ(mc.Read(fd, buf, 64), 64);
	CHECK(std::memcmp(buf, data.data() + 100, 64) == 0);
	CHECK_EQ(mc.Seek(fd, -10, 2), 4990);
	CHECK_EQ(mc.Read(fd, buf, 64), 10);
	CHECK_EQ(mc.Close(fd), kMcOk);
	CHECK_EQ(mc.Close(fd), kMcNoEntry);
	// 5 clusters of data, 2 for the directory ("." "..", SAVE1), 1 more for the root (3 entries).
	CHECK_EQ(mc.FreeClusters(), McFs::kTotalClusters - 5 - 2 - 1);

	// Open modes.
	CHECK_EQ(mc.Open("/BASLUS-20552GTA40000/NOPE", kMcRdOnly), kMcNoEntry);
	fd = mc.Open("/BASLUS-20552GTA40000/SAVE1", kMcRdOnly);
	CHECK(fd >= 0);
	CHECK_EQ(mc.Write(fd, buf, 4), kMcDeniedPermit);
	CHECK_EQ(mc.Close(fd), kMcOk);
	fd = mc.Open("/BASLUS-20552GTA40000/SAVE1", kMcWrOnly | kMcTrunc);
	CHECK(fd >= 0);
	CHECK_EQ(mc.Read(fd, buf, 4), kMcDeniedPermit);
	CHECK_EQ(mc.Close(fd), kMcOk);
	CHECK_EQ(mc.Open("/BASLUS-20552GTA40000", kMcRdOnly), kMcDeniedPermit); // directory

	// Handle limit.
	std::vector<int> fds;
	for (int i = 0; i < McFs::kMaxFiles; i++)
		fds.push_back(mc.Open("/BASLUS-20552GTA40000/SAVE1", kMcRdOnly));
	CHECK(fds.back() >= 0);
	CHECK_EQ(mc.Open("/BASLUS-20552GTA40000/SAVE1", kMcRdOnly), kMcUpLimitHandle);
	for (int f : fds)
		mc.Close(f);

	// Current directory and relative paths; ".." never leaves the card.
	std::string prev;
	CHECK_EQ(mc.Chdir("/BASLUS-20552GTA40000", &prev), kMcOk);
	CHECK(prev == "/");
	CHECK(mc.Cwd() == "/BASLUS-20552GTA40000");
	fd = mc.Open("ICON.SYS", kMcCreate | kMcWrOnly);
	CHECK(fd >= 0);
	CHECK_EQ(mc.Write(fd, "icon", 4), 4);
	mc.Close(fd);
	CHECK_EQ(mc.Chdir("../../..", &prev), kMcOk);
	CHECK(mc.Cwd() == "/" && prev == "/BASLUS-20552GTA40000");
	CHECK_EQ(mc.Chdir("/NOPE", &prev), kMcNoEntry);
	std::vector<std::string> parts;
	CHECK(mc.Resolve("../../../../etc", &parts) && parts.size() == 1 && parts[0] == "etc");
	CHECK(!mc.Resolve("/a\\b", &parts));
	struct stat st;
	CHECK(stat((base + "/mc0/BASLUS-20552GTA40000/ICON.SYS").c_str(), &st) == 0);

	// Directory listing: dot entries, wildcards, attributes, continuation.
	std::vector<std::string> n = Names(mc, "/*");
	CHECK(n.size() == 3 && n[0] == "." && n[1] == ".." && n[2] == "BASLUS-20552GTA40000");
	n = Names(mc, "/BASLUS-20552*");
	CHECK(n.size() == 1);
	n = Names(mc, "/BASLUS-20552GTA40000/*");
	CHECK(n.size() == 4);
	n = Names(mc, "/BASLUS-20552GTA40000/SAVE?");
	CHECK(n.size() == 1 && n[0] == "SAVE1");
	CHECK(Names(mc, "/XYZ*").empty());
	std::vector<McDirEntry> ents;
	CHECK_EQ(mc.GetDir("/BASLUS-20552GTA40000/*", 0, 3, &ents), 3);
	CHECK((ents[0].attr & kMcAttrSubdir) && ents[0].file_size == 4);
	CHECK_EQ(mc.GetDir("", 1, 3, &ents), 1); // continuation returns the rest
	CHECK_EQ(mc.GetDir("/BASLUS-20552GTA40000/ICON.SYS", 0, 8, &ents), 1);
	CHECK((ents[0].attr & kMcAttrFile) && ents[0].file_size == 4 && ents[0].modify.year >= 2024);
	CHECK_EQ(mc.GetDir("/NODIR/*", 0, 8, &ents), kMcNoEntry);

	// Entry space and rename / delete.
	CHECK_EQ(mc.GetEntSpace("/BASLUS-20552GTA40000"), 0); // 4 entries = 2 full clusters
	CHECK_EQ(mc.GetEntSpace("/NEWDIR"), 0);
	McDirEntry info{};
	std::strcpy(info.name, "SAVE2");
	CHECK_EQ(mc.SetFileInfo("/BASLUS-20552GTA40000/SAVE1", info, kMcInfoName), kMcOk);
	std::strcpy(info.name, "ICON.SYS");
	CHECK_EQ(mc.SetFileInfo("/BASLUS-20552GTA40000/SAVE2", info, kMcInfoName), kMcFailReplace);
	info.modify = McDateTime{0, 30, 15, 12, 1, 6, 2003};
	CHECK_EQ(mc.SetFileInfo("/BASLUS-20552GTA40000/SAVE2", info, kMcInfoModify), kMcOk);
	CHECK_EQ(mc.GetDir("/BASLUS-20552GTA40000/SAVE2", 0, 1, &ents), 1);
	CHECK(ents[0].modify.year == 2003 && ents[0].modify.hour == 12 && ents[0].modify.min == 15);
	CHECK_EQ(mc.Delete("/BASLUS-20552GTA40000"), kMcNotEmpty);
	CHECK_EQ(mc.Delete("/BASLUS-20552GTA40000/SAVE2"), kMcOk);
	CHECK_EQ(mc.Delete("/BASLUS-20552GTA40000/SAVE2"), kMcNoEntry);
	CHECK_EQ(mc.Delete("/BASLUS-20552GTA40000/ICON.SYS"), kMcOk);
	CHECK_EQ(mc.Delete("/BASLUS-20552GTA40000"), kMcOk);
	CHECK_EQ(mc.FreeClusters(), McFs::kTotalClusters);

	// Capacity: a write that does not fit fails with FullDevice.
	fd = mc.Open("/BIG", kMcCreate | kMcRdWr);
	std::vector<uint8_t> big(size_t(McFs::kTotalClusters + 1) * 1024);
	CHECK_EQ(mc.Write(fd, big.data(), int(big.size())), kMcFullDevice);
	mc.Close(fd);

	// Unformat removes everything.
	CHECK_EQ(mc.Unformat(), kMcOk);
	CHECK(!mc.Formatted());

	std::printf(iop_test::g_failures ? "FAILED\n" : "OK\n");
	return iop_test::g_failures ? 1 : 0;
}
