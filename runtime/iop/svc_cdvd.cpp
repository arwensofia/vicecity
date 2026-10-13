// SPDX-License-Identifier: GPL-3.0-or-later
//
// cdvdfsv services used by libcdvd (SDK 2.4): init, S commands, N commands (incl. the
// sceCdSt* stream interface), search file and disk ready. Data comes from the disc image.

#include <algorithm>

#include "iop.h"

namespace vc::iop {

namespace {

// libcdvd status codes
constexpr uint32_t kCdComplete = 2;

// sceCdlFILE as libcdvd expects it (36 bytes).
#pragma pack(push, 1)
struct CdlFile
{
	uint32_t lsn;
	uint32_t size;
	char name[16];
	uint8_t date[8]; // [1]=sec [2]=min [3]=hour [4]=day [5]=month [6..7]=year (binary)
	uint32_t flag;
};
#pragma pack(pop)
static_assert(sizeof(CdlFile) == 0x24, "sceCdlFILE");

class CdInit : public RpcServer
{
public:
	const char* Name() const override { return "cdvdfsv init"; }
	void Call(RpcCall& call) override
	{
		// recv: [0] result, [1] cdvdfsv version, [2] cdvdman version, [3] debug mode.
		call.Ret(0, 1);
		call.Ret(1, 0x0225);
		call.Ret(2, 0x0225);
		call.Ret(3, 0);
		if (call.Arg(0) == 5)
			Log("cdvd", "sceCdInit(exit)");
		else
			Log("cdvd", "sceCdInit(%u): disc %s", call.Arg(0), St().iso.IsOpen() ? "ready" : "MISSING");
	}
};

class CdSCmd : public RpcServer
{
public:
	const char* Name() const override { return "cdvdfsv S-commands"; }
	void Call(RpcCall& call) override
	{
		switch (call.fno)
		{
		case 0x01: { // ReadClock: [0] result, [4..11] sceCdCLOCK (BCD, JST)
			Clock t = NowJst();
			uint8_t clk[8] = {0, ToBcd(t.sec), ToBcd(t.min), ToBcd(t.hour), 0, ToBcd(t.day),
			                  ToBcd(t.month), ToBcd(t.year % 100)};
			call.Ret(0, 1);
			if (call.recv.size() >= 12)
				std::memcpy(call.recv.data() + 4, clk, 8);
			break;
		}
		case 0x16: // Break
		case 0x22: // Mmode
			call.Ret(0, 1);
			break;
		default:
			if (LogLimited("cdvd-s-" + std::to_string(call.fno)))
				Log("cdvd", "S-command %02x not emulated; returning success", call.fno);
			call.Ret(0, 1);
			break;
		}
	}
};

// N commands, including the stream interface (fno 9).
class CdNCmd : public RpcServer
{
public:
	const char* Name() const override { return "cdvdfsv N-commands"; }
	void Call(RpcCall& call) override
	{
		switch (call.fno)
		{
		case 0x06: // Standby
		case 0x07: // Stop
		case 0x08: // Pause
		case 0x05: // Seek
			call.Ret(0, 1);
			break;
		case 0x0e: // disk ready (N-command variant used by sceCdStandby)
			call.Ret(0, kCdComplete);
			break;
		case 0x09:
			Stream(call);
			break;
		default:
			if (LogLimited("cdvd-n-" + std::to_string(call.fno)))
				Log("cdvd", "N-command %02x not emulated; returning failure", call.fno);
			call.Ret(0, 0);
			break;
		}
	}
	void Reset() override
	{
		lsn_ = 0;
		running_ = false;
	}

private:
	void Stream(RpcCall& call)
	{
		// send: {u32 lbn, u32 size, u32 buf, u32 cmd, sceCdRMode mode[3]}
		uint32_t lbn = call.Arg(0), size = call.Arg(1), buf = call.Arg(2), cmd = call.Arg(3);
		uint32_t ret = 1;
		switch (cmd)
		{
		case 1: // start(lbn)
			lsn_ = lbn;
			running_ = true;
			Log("cdvd", "sceCdStStart(lsn %u)", lbn);
			break;
		case 2: { // read(size sectors into buf) -> (err << 16) | sectors
			uint32_t n = 0;
			if (running_ && St().iso.IsOpen())
			{
				uint64_t avail = St().iso.SectorCount() > lsn_ ? St().iso.SectorCount() - lsn_ : 0;
				n = uint32_t(std::min<uint64_t>(avail, size));
				n = std::min<uint32_t>(n, 0x4000); // at most 32 MB per request
				uint8_t* dst = n ? EePtr(buf, n * kSectorSize) : nullptr;
				if (!dst || !St().iso.ReadSectors(lsn_, n, dst))
					n = 0;
				lsn_ += n;
			}
			ret = n | (n == 0 && size ? (0x30u << 16) : 0); // 0x30: SCECdErREAD
			break;
		}
		case 3: // stop
			running_ = false;
			break;
		case 4: // seek(lbn)
		case 9: // seekF(lbn)
			lsn_ = lbn;
			break;
		case 5: // init(bufmax, bankmax, iop buffer)
			Log("cdvd", "sceCdStInit(bufmax %u, bankmax %u)", lbn, size);
			break;
		case 6: // stat: sectors buffered -- reads complete instantly, so "plenty"
			ret = running_ ? 0x80 : 0;
			break;
		case 7: // pause
		case 8: // resume
			break;
		default:
			Log("cdvd", "stream command %u not emulated", cmd);
			ret = 0;
			break;
		}
		call.Ret(0, ret);
	}

	uint32_t lsn_ = 0;
	bool running_ = false;
};

class CdSearchFile : public RpcServer
{
public:
	const char* Name() const override { return "cdvdfsv search file"; }
	void Call(RpcCall& call) override
	{
		// send: {sceCdlFILE (0x24), char name[256] @0x24, u32 ee_result_addr @0x124}
		if (call.send.size() < 0x128)
		{
			call.Ret(0, 0);
			return;
		}
		std::string name(reinterpret_cast<const char*>(call.send.data() + 0x24),
		                 strnlen(reinterpret_cast<const char*>(call.send.data() + 0x24), 256));
		uint32_t dst;
		std::memcpy(&dst, call.send.data() + 0x124, 4);
		IsoEntry e;
		if (!St().iso.IsOpen() || !St().iso.Find(name, &e) || e.is_dir)
		{
			Log("cdvd", "sceCdSearchFile('%s'): not found", name.c_str());
			call.Ret(0, 0);
			return;
		}
		CdlFile f{};
		f.lsn = e.lsn;
		f.size = e.size;
		std::string disc_name = e.name + ";1";
		std::memcpy(f.name, disc_name.data(), std::min<size_t>(disc_name.size(), sizeof f.name));
		f.date[1] = e.date[5];
		f.date[2] = e.date[4];
		f.date[3] = e.date[3];
		f.date[4] = e.date[2];
		f.date[5] = e.date[1];
		uint16_t year = uint16_t(1900 + e.date[0]);
		std::memcpy(&f.date[6], &year, 2);
		EeWrite(dst, &f, sizeof f);
		call.Ret(0, 1);
		if (LogLimited("search-" + name, 1))
			Log("cdvd", "sceCdSearchFile('%s') -> lsn %u size %u", name.c_str(), e.lsn, e.size);
	}
};

class CdDiskReady : public RpcServer
{
public:
	const char* Name() const override { return "cdvdfsv disk ready"; }
	void Call(RpcCall& call) override { call.Ret(0, St().iso.IsOpen() ? kCdComplete : 6); }
};

CdInit g_init;
CdSCmd g_scmd;
CdNCmd g_ncmd;
CdSearchFile g_search;
CdDiskReady g_ready;

} // namespace

void RegisterCdvd()
{
	RegisterServer(0x80000592, &g_init);
	RegisterServer(0x80000593, &g_scmd);
	RegisterServer(0x80000595, &g_ncmd);
	RegisterServer(0x80000597, &g_search);
	RegisterServer(0x8000059a, &g_ready);
}

} // namespace vc::iop
