// SPDX-License-Identifier: GPL-3.0-or-later
//
// ISO9660 reader: a synthetic image built here (always), and the user's disc when VC_ISO is
// set (only structural facts every PS2 disc has are checked: SYSTEM.CNF with BOOT2).

#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "iso_fs.h"
#include "test_support.h"
#include "vc_iop.h"

using vc::iop::IsoEntry;
using vc::iop::IsoImage;
using vc::iop::kSectorSize;

namespace {

void Put32Both(uint8_t* p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
	{
		p[i] = uint8_t(v >> (8 * i));
		p[7 - i] = uint8_t(v >> (8 * i));
	}
}

size_t Record(uint8_t* p, uint32_t lsn, uint32_t size, bool dir, const std::string& name)
{
	size_t len = 33 + name.size();
	len += len & 1;
	std::memset(p, 0, len);
	p[0] = uint8_t(len);
	Put32Both(p + 2, lsn);
	Put32Both(p + 10, size);
	p[18] = 102; // 2002
	p[19] = 7;
	p[20] = 4;
	p[25] = dir ? 2 : 0;
	p[32] = uint8_t(name.size());
	std::memcpy(p + 33, name.data(), name.size());
	return len;
}

// Sectors: 16 PVD, 17 terminator, 18 root dir, 19 SUB dir, 20 HELLO.TXT, 21 DATA.BIN,
// optionally a record pointing beyond the image.
std::string BuildImage(bool corrupt)
{
	const uint32_t kSectors = 24;
	std::vector<uint8_t> img(kSectors * kSectorSize);
	uint8_t* pvd = &img[16 * kSectorSize];
	pvd[0] = 1;
	std::memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	std::memcpy(pvd + 40, "TESTVOL                         ", 32);
	pvd[128] = 0x00;
	pvd[129] = 0x08;
	Record(pvd + 156, 18, kSectorSize, true, std::string(1, '\0'));
	uint8_t* term = &img[17 * kSectorSize];
	term[0] = 255;
	std::memcpy(term + 1, "CD001", 5);

	uint8_t* root = &img[18 * kSectorSize];
	size_t pos = 0;
	pos += Record(root + pos, 18, kSectorSize, true, std::string(1, '\0'));
	pos += Record(root + pos, 18, kSectorSize, true, std::string(1, '\1'));
	pos += Record(root + pos, 20, 12, false, "HELLO.TXT;1");
	pos += Record(root + pos, 19, kSectorSize, true, "SUB");
	if (corrupt)
		pos += Record(root + pos, 1000, 4096, false, "BAD.BIN;1");
	uint8_t* sub = &img[19 * kSectorSize];
	pos = 0;
	pos += Record(sub + pos, 19, kSectorSize, true, std::string(1, '\0'));
	pos += Record(sub + pos, 18, kSectorSize, true, std::string(1, '\1'));
	pos += Record(sub + pos, 21, 3000, false, "DATA.BIN;1");
	std::memcpy(&img[20 * kSectorSize], "hello, disc\n", 12);
	for (uint32_t i = 0; i < 3000; i++)
		img[21 * kSectorSize + i] = uint8_t(i * 7);

	std::string path = iop_test::TempDir("iso") + "/test.iso";
	int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(write(fd, img.data(), img.size()) == ssize_t(img.size()));
	close(fd);
	return path;
}

void TestSplitPath()
{
	std::vector<std::string> p;
	CHECK(IsoImage::SplitPath("cdrom0:\\DATA\\default.dat;1", &p));
	CHECK(p.size() == 2 && p[0] == "DATA" && p[1] == "DEFAULT.DAT");
	CHECK(IsoImage::SplitPath("/a/./b/../c", &p));
	CHECK(p.size() == 2 && p[0] == "A" && p[1] == "C");
	CHECK(IsoImage::SplitPath("..\\..\\x", &p));
	CHECK(p.size() == 1 && p[0] == "X");
	CHECK(IsoImage::SplitPath("cdrom0:", &p) && p.empty());
	CHECK(!IsoImage::SplitPath(std::string("a/") + std::string(300, 'x'), &p));
}

void TestSynthetic()
{
	IsoImage iso;
	std::string err;
	CHECK(iso.Open(BuildImage(false), &err));
	CHECK(iso.VolumeId() == "TESTVOL");
	IsoEntry e;
	CHECK(iso.Find("cdrom0:\\hello.txt;1", &e));
	CHECK(!e.is_dir && e.lsn == 20 && e.size == 12);
	char buf[32] = {};
	CHECK_EQ(iso.ReadFile(e, 0, buf, sizeof buf), 12);
	CHECK(std::memcmp(buf, "hello, disc\n", 12) == 0);
	CHECK_EQ(iso.ReadFile(e, 7, buf, 3), 3);
	CHECK(std::memcmp(buf, "dis", 3) == 0);
	CHECK(iso.Find("\\SUB\\DATA.BIN;1", &e) && e.size == 3000);
	CHECK(iso.Find("sub/data.bin", &e));
	std::vector<uint8_t> sec(2 * kSectorSize);
	CHECK(iso.ReadSectors(21, 2, sec.data()));
	CHECK(sec[7] == 49 && sec[2999] == uint8_t(2999 * 7));
	CHECK(!iso.ReadSectors(23, 2, sec.data())); // past the end
	CHECK(!iso.Find("\\NOPE.TXT", &e));
	CHECK(!iso.Find("\\HELLO.TXT\\X", &e)); // file used as directory
	std::vector<IsoEntry> list;
	CHECK(iso.ListDir("cdrom0:\\", &list) && list.size() == 2);
	CHECK(!iso.ListDir("\\HELLO.TXT", &list));

	// A directory record pointing outside the image makes the directory malformed.
	IsoImage bad;
	CHECK(!bad.Open(BuildImage(true), &err));

	// Not an image at all.
	IsoImage none;
	CHECK(!none.Open("/dev/null", &err));
}

int TestDisc(const char* path)
{
	IsoImage iso;
	std::string err;
	if (!iso.Open(path, &err))
	{
		std::fprintf(stderr, "cannot open %s: %s\n", path, err.c_str());
		return 1;
	}
	std::printf("disc: volume '%s', %llu sectors\n", iso.VolumeId().c_str(),
	            (unsigned long long)iso.SectorCount());
	IsoEntry cnf;
	CHECK(iso.Find("cdrom0:\\SYSTEM.CNF;1", &cnf));
	std::string text(cnf.size, '\0');
	CHECK_EQ(iso.ReadFile(cnf, 0, text.data(), text.size()), int64_t(cnf.size));
	CHECK(text.find("BOOT2") != std::string::npos);
	IsoEntry lower;
	CHECK(iso.Find("system.cnf", &lower) && lower.lsn == cnf.lsn);

	// A file below DATA: first entry of the directory listing, read two ways.
	std::vector<IsoEntry> data;
	CHECK(iso.ListDir("\\DATA", &data) && !data.empty());
	for (const IsoEntry& e : data)
	{
		if (e.is_dir || e.size == 0)
			continue;
		IsoEntry f;
		CHECK(iso.Find("cdrom0:\\DATA\\" + e.name + ";1", &f) && f.lsn == e.lsn);
		std::vector<uint8_t> a(kSectorSize), b(kSectorSize);
		size_t n = std::min<size_t>(kSectorSize, f.size);
		CHECK_EQ(iso.ReadFile(f, 0, a.data(), n), int64_t(n));
		CHECK(iso.ReadSectors(f.lsn, 1, b.data()));
		CHECK(std::memcmp(a.data(), b.data(), n) == 0);
		std::printf("disc: DATA\\%s lsn %u size %u\n", f.name.c_str(), f.lsn, f.size);
		break;
	}
	std::vector<uint8_t> past(kSectorSize);
	CHECK(!iso.ReadSectors(uint32_t(iso.SectorCount()), 1, past.data()));

	// Through the C entry point used by the app to load the executable.
	vc_iop_config cfg{};
	cfg.iso_path = path;
	cfg.data_dir = nullptr;
	cfg.ee_ram = iop_test::Ram();
	std::string dir = iop_test::TempDir("isodata");
	cfg.data_dir = dir.c_str();
	CHECK_EQ(vc_iop_init(&cfg), 0);
	void* buf = nullptr;
	size_t size = 0;
	CHECK_EQ(vc_iop_read_disc_file("cdrom0:\\SYSTEM.CNF;1", &buf, &size), 0);
	CHECK(size == cnf.size && buf && std::memcmp(buf, text.data(), size) == 0);
	free(buf);
	CHECK_EQ(vc_iop_read_disc_file("cdrom0:\\NO_SUCH.FILE;1", &buf, &size), -1);
	vc_iop_shutdown();
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	bool disc_mode = argc > 1 && std::strcmp(argv[1], "--disc") == 0;
	if (!disc_mode)
	{
		TestSplitPath();
		TestSynthetic();
		std::printf(iop_test::g_failures ? "FAILED\n" : "OK\n");
		return iop_test::g_failures ? 1 : 0;
	}
	const char* disc = getenv("VC_ISO");
	if (!disc || !disc[0])
	{
		std::printf("VC_ISO not set; disc tests skipped\n");
		return 77;
	}
	int rc = TestDisc(disc);
	if (iop_test::g_failures || rc)
	{
		std::fprintf(stderr, "%d failure(s)\n", iop_test::g_failures);
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
