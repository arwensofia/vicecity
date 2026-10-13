// SPDX-License-Identifier: GPL-3.0-or-later
//
// SIF RPC dispatch through the ps2f_ overrides with synthetic EE calls: bind, call (blocking
// and nowait with end functions), and each service's buffer protocol.

#include <cstring>
#include <string>

#include "test_support.h"
#include "vc_iop.h"
#include "vc_pad.h"

extern "C" {
void ps2f_001164e0(ps2_cpu* c); // sceSifBindRpc
void ps2f_001166b0(ps2_cpu* c); // sceSifCallRpc
void ps2f_001168a0(ps2_cpu* c); // sceSifCheckStatRpc
void ps2f_001176c8(ps2_cpu* c); // sceOpen
void ps2f_00117950(ps2_cpu* c); // sceClose
void ps2f_00117ad0(ps2_cpu* c); // sceLseek
void ps2f_00117d08(ps2_cpu* c); // sceRead
}

namespace {

using iop_test::Cpu;
using iop_test::Ram;

uint32_t R32(uint32_t a)
{
	uint32_t v;
	std::memcpy(&v, Ram() + (a & 0x01FFFFFF), 4);
	return v;
}

void W32(uint32_t a, uint32_t v)
{
	std::memcpy(Ram() + (a & 0x01FFFFFF), &v, 4);
}

void Set(int reg, uint32_t v)
{
	Cpu()->r[reg].sd[0] = int32_t(v);
}

int32_t V0()
{
	return Cpu()->r[2].sw[0];
}

int32_t Bind(uint32_t client, uint32_t sid)
{
	Set(4, client);
	Set(5, sid);
	Set(6, 0);
	ps2f_001164e0(Cpu());
	return V0();
}

int32_t Call(uint32_t client, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
             uint32_t rsize, uint32_t end_func = 0, uint32_t end_param = 0)
{
	ps2_cpu* c = Cpu();
	Set(4, client);
	Set(5, fno);
	Set(6, mode);
	Set(7, send);
	Set(8, ssize);
	Set(9, recv);
	Set(10, rsize);
	Set(11, end_func);
	W32(c->r[29].uw[0], end_param);
	ps2f_001166b0(c);
	return V0();
}

constexpr uint32_t kClient = 0x00100000; // client blocks, 0x40 apart
constexpr uint32_t kBuf = 0x00200000;    // send/recv buffers
constexpr uint32_t kOut = 0x00300000;    // other EE buffers

void TestBindAndUnknown()
{
	CHECK_EQ(Bind(kClient, 0x80000400), 0);
	CHECK(R32(kClient + 0x24) != 0); // serve
	CHECK_EQ(Bind(kClient + 0x40, 0x12345678), 0);
	CHECK(R32(kClient + 0x40 + 0x24) != 0);
	W32(kBuf, 0xDEADBEEF);
	CHECK_EQ(Call(kClient + 0x40, 3, 0, kBuf, 16, kBuf, 16), 0);
	CHECK_EQ(R32(kBuf), 0xDEADBEEF); // unknown server: recv untouched
	// Unbound client.
	W32(kClient + 0x80 + 0x24, 0);
	CHECK_EQ(Call(kClient + 0x80, 0, 0, kBuf, 4, kBuf, 4), -1);
	Set(4, kClient);
	ps2f_001168a0(Cpu());
	CHECK_EQ(V0(), 0);
}

void TestLoadfileAndHeap()
{
	uint32_t cl = kClient + 0xc0;
	CHECK_EQ(Bind(cl, 0x80000006), 0);
	CHECK_EQ(Call(cl, 0xff, 0, 0, 0, kBuf, 4), 0);
	CHECK(std::memcmp(Ram() + kBuf, "2410", 4) == 0);
	std::memset(Ram() + kBuf, 0, 0x200);
	std::strcpy(reinterpret_cast<char*>(Ram() + kBuf + 8), "cdrom0:\\SYSTEM\\PADMAN.IRX;1");
	CHECK_EQ(Call(cl, 0, 0, kBuf, 0x200, kBuf, 8), 0);
	CHECK(int32_t(R32(kBuf)) > 0);
	CHECK_EQ(R32(kBuf + 4), 0);

	uint32_t hc = kClient + 0x100;
	CHECK_EQ(Bind(hc, 0x80000003), 0);
	W32(kBuf, 0x1000);
	CHECK_EQ(Call(hc, 1, 0, kBuf, 4, kBuf + 0x40, 4), 0);
	uint32_t a = R32(kBuf + 0x40);
	CHECK(a >= 0x100000 && a < 0x200000);
	W32(kBuf, 0x1000);
	CHECK_EQ(Call(hc, 1, 0, kBuf, 4, kBuf + 0x40, 4), 0);
	CHECK(R32(kBuf + 0x40) >= a + 0x1000);
	W32(kBuf, a);
	CHECK_EQ(Call(hc, 2, 0, kBuf, 4, kBuf + 0x40, 4), 0);
	CHECK_EQ(R32(kBuf + 0x40), 0);
	CHECK_EQ(Call(hc, 2, 0, kBuf, 4, kBuf + 0x40, 4), 0);
	CHECK_EQ(int32_t(R32(kBuf + 0x40)), -1); // double free
}

void TestMcserv()
{
	const uint32_t cl = kClient;
	const uint32_t desc = kBuf, name = kBuf + 0x100, res = kBuf + 0x800, param = kBuf + 0x900;
	// init
	CHECK_EQ(Call(cl, 0xfe, 0, desc, 0x30, res, 0xc), 0);
	CHECK(R32(res + 4) >= 0x20a && R32(res + 8) >= 0x20e);

	// getinfo (nowait + end function, like sceMcGetInfo)
	int ends = 0;
	uint32_t seen_param = 0;
	iop_test::SetEeFunction(0x00400000, [&](ps2_cpu* c) {
		ends++;
		seen_param = c->r[4].uw[0];
		c->r[16].ud[0] = 0x5555; // clobber s0: must be restored by the caller side
	});
	Cpu()->r[16].ud[0] = 0x1234;
	std::memset(Ram() + desc, 0, 0x30);
	W32(desc + 4, 0);
	W32(desc + 0x1c, param);
	CHECK_EQ(Call(cl, 0x01, 1, desc, 0x30, res, 4, 0x00400000, param), 0);
	CHECK_EQ(ends, 1);
	CHECK_EQ(seen_param, param);
	CHECK_EQ(Cpu()->r[16].ud[0], 0x1234);
	CHECK_EQ(int32_t(R32(res)), -1); // first access: new formatted card
	CHECK_EQ(R32(param), 2);         // PS2 card
	CHECK(R32(param + 4) > 7000);    // free clusters
	CHECK_EQ(R32(param + 0x90), 1);  // formatted
	CHECK_EQ(Call(cl, 0x01, 1, desc, 0x30, res, 4, 0x00400000, param), 0);
	CHECK_EQ(int32_t(R32(res)), 0); // same card
	W32(desc + 4, 1);
	CHECK_EQ(Call(cl, 0x01, 1, desc, 0x30, res, 4, 0x00400000, param), 0);
	CHECK_EQ(int32_t(R32(res)), -10); // slot 2 empty
	// libmc's own end function 0x33a558 runs natively: copies type/free/format into the
	// pointers sceMcGetInfo stored at 0x6ba2a8..
	W32(desc + 4, 0);
	W32(0x006ba2a8, kOut + 0x10);
	W32(0x006ba2ac, kOut + 0x14);
	W32(0x006ba2b0, kOut + 0x18);
	CHECK_EQ(Call(cl, 0x01, 1, desc, 0x30, res, 4, 0x0033a558, param), 0);
	CHECK_EQ(R32(kOut + 0x10), 2);
	CHECK(R32(kOut + 0x14) > 7000);
	CHECK_EQ(R32(kOut + 0x18), 1);
	CHECK_EQ(iop_test::CallCount(0x0033a558), 0); // not dispatched to recompiled code

	// mkdir + open/write/read/close through the name block and descriptor protocols
	std::memset(Ram() + name, 0, 0x414);
	W32(name + 0, 0);
	W32(name + 8, 0x40);
	std::strcpy(reinterpret_cast<char*>(Ram() + name + 0x14), "/BASLUS-20552GTA40000");
	CHECK_EQ(Call(cl, 0x02, 1, name, 0x414, res, 4), 0);
	CHECK_EQ(int32_t(R32(res)), 0);
	W32(name + 8, 0x200 | 0x3);
	std::strcpy(reinterpret_cast<char*>(Ram() + name + 0x14), "/BASLUS-20552GTA40000/GTAVCSF1");
	CHECK_EQ(Call(cl, 0x02, 1, name, 0x414, res, 4), 0);
	int32_t fd = int32_t(R32(res));
	CHECK(fd >= 0);
	// write 100 bytes: 3 unaligned head bytes travel in the descriptor, 97 come from EE memory
	uint32_t src = kOut + 0x13;
	for (int i = 0; i < 100; i++)
		Ram()[src + i] = uint8_t(i + 1);
	std::memset(Ram() + desc, 0, 0x30);
	W32(desc, uint32_t(fd));
	W32(desc + 0x14, 3);
	std::memcpy(Ram() + desc + 0x20, Ram() + src, 3);
	W32(desc + 0x18, src + 3);
	W32(desc + 0x0c, 97);
	CHECK_EQ(Call(cl, 0x06, 1, desc, 0x30, res, 4), 0);
	CHECK_EQ(int32_t(R32(res)), 100);
	W32(desc + 0x10, 0);
	W32(desc + 0x14, 0);
	CHECK_EQ(Call(cl, 0x04, 1, desc, 0x30, res, 4), 0); // seek 0
	CHECK_EQ(int32_t(R32(res)), 0);
	std::memset(Ram() + kOut + 0x1000, 0, 128);
	W32(param, 0x77);
	W32(desc + 0x0c, 100);
	W32(desc + 0x18, kOut + 0x1001);
	W32(desc + 0x1c, param);
	CHECK_EQ(Call(cl, 0x05, 1, desc, 0x30, res, 4, 0x00400000, param), 0);
	CHECK_EQ(int32_t(R32(res)), 100);
	CHECK(std::memcmp(Ram() + kOut + 0x1001, Ram() + src, 100) == 0);
	CHECK_EQ(R32(param), 0); // no unaligned fragments for the end function
	CHECK_EQ(Call(cl, 0x03, 1, desc, 0x30, res, 4), 0);
	CHECK_EQ(int32_t(R32(res)), 0);

	// getdir into an EE table
	W32(name + 8, 0);
	W32(name + 0xc, 8);
	W32(name + 0x10, kOut + 0x2000);
	std::strcpy(reinterpret_cast<char*>(Ram() + name + 0x14), "/BASLUS-20552GTA40000/*");
	CHECK_EQ(Call(cl, 0x0d, 1, name, 0x414, res, 4), 0);
	CHECK_EQ(int32_t(R32(res)), 3);
	CHECK(std::strcmp(reinterpret_cast<char*>(Ram() + kOut + 0x2000 + 2 * 64 + 0x20), "GTAVCSF1") == 0);
	CHECK_EQ(R32(kOut + 0x2000 + 2 * 64 + 0x10), 100);

	// chdir reports the previous directory
	W32(name + 0x10, kOut + 0x3000);
	std::strcpy(reinterpret_cast<char*>(Ram() + name + 0x14), "/BASLUS-20552GTA40000");
	CHECK_EQ(Call(cl, 0x0c, 1, name, 0x414, res, 4), 0);
	CHECK_EQ(int32_t(R32(res)), 0);
	CHECK(std::strcmp(reinterpret_cast<char*>(Ram() + kOut + 0x3000), "/") == 0);

	// entry space for a directory that does not exist yet
	std::strcpy(reinterpret_cast<char*>(Ram() + name + 0x14), "/BASLUS-20552GTA40001");
	CHECK_EQ(Call(cl, 0x12, 1, name, 0x414, res, 4), 0);
	CHECK(int32_t(R32(res)) >= 0);
}

void TestPad()
{
	const uint32_t cl = kClient + 0x140, buf = kBuf + 0x1000, area = kOut + 0x4000;
	CHECK_EQ(Bind(cl, 0x80000100), 0);
	std::memset(Ram() + buf, 0, 0x80);
	W32(buf, 0x12);
	CHECK_EQ(Call(cl, 1, 0, buf, 0x80, buf, 0x80), 0);
	CHECK_EQ(R32(buf + 0xc) >> 8, 4); // libpad wants padman 4.x
	std::memset(Ram() + area, 0, 256);
	W32(buf, 0x01);
	W32(buf + 4, 0);
	W32(buf + 8, 0);
	W32(buf + 0x10, area);
	CHECK_EQ(Call(cl, 1, 0, buf, 0x80, buf, 0x80), 0);
	CHECK_EQ(R32(buf + 0xc), 1);
	CHECK(R32(buf + 0x14) != 0); // IOP actuator buffer

	auto newest = [&]() { return R32(area + 0x58) >= R32(area + 0x80 + 0x58) ? area : area + 0x80; };
	uint32_t f = newest();
	CHECK_EQ(Ram()[f + 0x70], 6);    // stable
	CHECK_EQ(Ram()[f + 1], 0x73);    // DualShock analog mode
	CHECK_EQ(Ram()[f + 2], 0xFF);    // no buttons (active low)
	CHECK_EQ(Ram()[f + 3], 0xFF);
	CHECK_EQ(Ram()[f + 6], 0x80);    // left stick centred
	CHECK_EQ(R32(f + 0x60), 32);

	vc_pad_enable_gamecontroller(0);
	vc_pad_key(49, 1);  // space -> Cross
	vc_pad_key(13, 1);  // W -> left stick up
	uint32_t before = R32(f + 0x58);
	vc_iop_vblank();
	f = newest();
	CHECK(R32(f + 0x58) > before);
	CHECK_EQ(Ram()[f + 3], uint8_t(~0x40)); // Cross is bit 6 of the second byte
	CHECK_EQ(Ram()[f + 7], 0x00);           // ly up
	CHECK_EQ(Ram()[f + 8 + 6], 0xFF);       // Cross pressure
	vc_pad_key(49, 0);
	vc_pad_key(13, 0);
	vc_pad_state s{};
	s.buttons = VC_PAD_START;
	s.lx = s.ly = s.rx = s.ry = 0x80;
	vc_pad_set_state(0, &s, 1);
	vc_iop_vblank();
	f = newest();
	CHECK_EQ(Ram()[f + 2], uint8_t(~0x08));
	CHECK_EQ(Ram()[f + 3], 0xFF);
	vc_pad_set_state(0, nullptr, 0);

	// digital mode via set main mode
	W32(buf, 0x06);
	W32(buf + 0xc, 0);
	W32(buf + 0x10, 0);
	CHECK_EQ(Call(cl, 1, 0, buf, 0x80, buf, 0x80), 0);
	CHECK_EQ(R32(buf + 0x14), 1);
	vc_iop_vblank();
	CHECK_EQ(Ram()[newest() + 1], 0x41);
}

void TestCdvd()
{
	const uint32_t cl = kClient + 0x180, buf = kBuf + 0x2000, res = kBuf + 0x2200;
	CHECK_EQ(Bind(cl, 0x80000592), 0);
	W32(buf, 0);
	CHECK_EQ(Call(cl, 0, 0, buf, 4, res, 0x10), 0);
	CHECK_EQ(R32(res), 1);
	CHECK_EQ(Bind(cl + 0x40, 0x80000593), 0);
	CHECK_EQ(Call(cl + 0x40, 0x01, 0, 0, 0, res, 0x10), 0); // ReadClock
	CHECK_EQ(R32(res), 1);
	uint8_t month = Ram()[res + 4 + 6];
	CHECK(month >= 0x01 && month <= 0x12);
	CHECK_EQ(Call(cl + 0x40, 0x22, 0, buf, 4, res, 4), 0); // Mmode
	CHECK_EQ(R32(res), 1);

	// search file: needs the disc
	CHECK_EQ(Bind(cl + 0x80, 0x80000597), 0);
	std::memset(Ram() + buf, 0, 0x128);
	std::strcpy(reinterpret_cast<char*>(Ram() + buf + 0x24), "\\SYSTEM.CNF;1");
	W32(buf + 0x124, buf);
	CHECK_EQ(Call(cl + 0x80, 0, 0, buf, 0x128, res, 4), 0);
	const char* iso = getenv("VC_ISO");
	if (iso && iso[0])
	{
		CHECK_EQ(R32(res), 1);
		CHECK(R32(buf) > 16);       // lsn
		CHECK(R32(buf + 4) > 0);    // size
		CHECK(std::strncmp(reinterpret_cast<char*>(Ram() + buf + 8), "SYSTEM.CNF;1", 12) == 0);
		// Stream: start at that LSN, read one sector, must contain BOOT2.
		CHECK_EQ(Bind(cl + 0xc0, 0x80000595), 0);
		uint32_t lsn = R32(buf);
		uint32_t cmd[5] = {lsn, 0, 0, 1, 0};
		std::memcpy(Ram() + buf + 0x200, cmd, 20);
		CHECK_EQ(Call(cl + 0xc0, 9, 0, buf + 0x200, 0x14, res, 4), 0);
		uint32_t rd[5] = {0, 1, kOut + 0x8000, 2, 0};
		std::memcpy(Ram() + buf + 0x200, rd, 20);
		CHECK_EQ(Call(cl + 0xc0, 9, 0, buf + 0x200, 0x14, res, 4), 0);
		CHECK_EQ(R32(res), 1);
		CHECK(std::string(reinterpret_cast<char*>(Ram() + kOut + 0x8000), 2048).find("BOOT2") !=
		      std::string::npos);
		// Game CDSTREAM read of the same sector.
		CHECK_EQ(Bind(cl + 0x100, 0x65686577), 0);
		uint32_t req[4] = {0, kOut + 0x9000, lsn, 1};
		std::memcpy(Ram() + buf + 0x300, req, 16);
		CHECK_EQ(Call(cl + 0x100, 1, 0, buf + 0x300, 16, res, 16), 0);
		CHECK_EQ(R32(res), 1);
		CHECK(std::memcmp(Ram() + kOut + 0x9000, Ram() + kOut + 0x8000, 2048) == 0);
		CHECK_EQ(Call(cl + 0x100, 3, 0, buf + 0x300, 16, res, 16), 0);
		CHECK_EQ(R32(res), 0);
		// fileio API on cdrom0:
		std::strcpy(reinterpret_cast<char*>(Ram() + kOut), "cdrom0:\\SYSTEM.CNF;1");
		Set(4, kOut);
		Set(5, 1);
		ps2f_001176c8(Cpu());
		int32_t fd = V0();
		CHECK(fd > 0);
		Set(4, uint32_t(fd));
		Set(5, kOut + 0xa000);
		Set(6, 5);
		ps2f_00117d08(Cpu());
		CHECK_EQ(V0(), 5);
		CHECK(std::memcmp(Ram() + kOut + 0xa000, Ram() + kOut + 0x8000, 5) == 0);
		Set(4, uint32_t(fd));
		ps2f_00117950(Cpu());
		CHECK_EQ(V0(), 0);
	}
	else
		CHECK_EQ(R32(res), 0); // no disc: not found
}

void TestFileio()
{
	std::strcpy(reinterpret_cast<char*>(Ram() + kOut), "rom0:ROMVER");
	Set(4, kOut);
	Set(5, 1);
	ps2f_001176c8(Cpu());
	int32_t fd = V0();
	CHECK(fd > 0);
	Set(4, uint32_t(fd));
	Set(5, kOut + 0x100);
	Set(6, 64);
	ps2f_00117d08(Cpu());
	CHECK_EQ(V0(), 15);
	CHECK(std::memcmp(Ram() + kOut + 0x100, "0200A", 5) == 0);
	Set(4, uint32_t(fd));
	Set(5, 4);
	Set(6, 0);
	ps2f_00117ad0(Cpu());
	CHECK_EQ(V0(), 4);
	Set(4, uint32_t(fd));
	ps2f_00117950(Cpu());
	CHECK_EQ(V0(), 0);
	ps2f_00117950(Cpu());
	CHECK(V0() < 0); // already closed
	std::strcpy(reinterpret_cast<char*>(Ram() + kOut), "host0:debug.txt");
	Set(4, kOut);
	Set(5, 1);
	ps2f_001176c8(Cpu());
	CHECK(V0() < 0);
}

void TestSifRegs()
{
	CHECK(vc_iop_sif_get_reg(Cpu(), 4) & 0x40000);
	CHECK(vc_iop_sif_get_reg(Cpu(), 0x80000002) != 0);
	vc_iop_sif_set_reg(Cpu(), 0x80000005, 0x55);
	CHECK_EQ(vc_iop_sif_get_reg(Cpu(), 0x80000005), 0x55);
	// SIF1 DMA into IOP memory, then a padman-style transfer check through the heap.
	for (int i = 0; i < 32; i++)
		Ram()[kOut + 0xb000 + i] = uint8_t(i);
	uint32_t d[4] = {kOut + 0xb000, 0x1000, 32, 0};
	std::memcpy(Ram() + kOut + 0xc000, d, 16);
	uint32_t id = vc_iop_sif_set_dma(Cpu(), kOut + 0xc000, 1);
	CHECK(id != 0);
	CHECK(vc_iop_sif_dma_stat(Cpu(), id) < 0);
}

} // namespace

int main()
{
	std::string dir = iop_test::TempDir("rpc");
	vc_iop_config cfg{};
	cfg.iso_path = getenv("VC_ISO");
	cfg.data_dir = dir.c_str();
	cfg.ee_ram = Ram();
	CHECK_EQ(vc_iop_init(&cfg), 0);
	Cpu();
	TestBindAndUnknown();
	TestLoadfileAndHeap();
	TestMcserv();
	TestPad();
	TestCdvd();
	TestFileio();
	TestSifRegs();
	vc_iop_shutdown();
	std::printf(iop_test::g_failures ? "FAILED\n" : "OK\n");
	return iop_test::g_failures ? 1 : 0;
}
