// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rockstar's own IOP drivers, loaded from the disc as CDSTREAM.IRX and SAMPMAN2.IRX.
// Layouts come from the EE-side callers in SLUS_205.52 (docs/BOOT.md §3.3-3.5):
//
// CDSTREAM ('ehew', 0x65686577), all blocking, 16-byte send/receive:
//   fno 0 init(arg)                                  no reply
//   fno 1 read(channel, ee_buf, lsn, sectors)        [0] != 0: request accepted
//   fno 2 status(channel)                            [0] 0 idle, 0xff reading, 0xfa waiting,
//                                                        other = error
//   fno 3 sync(channel)                              [0] 0 success, != 0 error (blocks)
//   fno 4 last position()                            [0] LSN after the last read
// Reads are performed synchronously from the disc image straight into EE memory.
//
// SAMPMAN ('yehe', 0x65686579) and the music driver ('xehe', 0x65686578): fno is a command
// code ((cmd << 16) | (sub << 8)); argument and reply share one 64-byte buffer. Audio output
// is silent for now: commands are accepted and answered with plausible values.

#include <algorithm>

#include "iop.h"

namespace vc::iop {

namespace {

class CdStream : public RpcServer
{
public:
	const char* Name() const override { return "CDSTREAM (game)"; }
	void Call(RpcCall& call) override
	{
		switch (call.fno)
		{
		case 0:
			Log("cdstream", "init(%u)", call.Arg(0));
			break;
		case 1: {
			uint32_t ch = call.Arg(0), buf = call.Arg(1), lsn = call.Arg(2), n = call.Arg(3);
			bool ok = St().iso.IsOpen() && n <= 0x4000;
			uint8_t* dst = ok ? EePtr(buf, n * kSectorSize) : nullptr;
			ok = ok && dst && St().iso.ReadSectors(lsn, n, dst);
			if (!ok)
				Log("cdstream", "read ch %u lsn %u x%u -> %08x FAILED", ch, lsn, n, buf);
			else if (LogLimited("cdstream-read", 8))
				Log("cdstream", "read ch %u lsn %u x%u -> %08x", ch, lsn, n, buf);
			if (ch < 8)
				error_[ch] = ok ? 0 : 0xff;
			head_ = lsn + n;
			call.Ret(0, 1);
			break;
		}
		case 2:
			call.Ret(0, call.Arg(0) < 8 ? error_[call.Arg(0)] : 0);
			break;
		case 3: // sync: reads are already complete; non-zero reports the last error
			call.Ret(0, call.Arg(0) < 8 && error_[call.Arg(0)] ? 1 : 0);
			break;
		case 4:
			call.Ret(0, head_);
			break;
		default:
			if (LogLimited("cdstream-fno-" + std::to_string(call.fno)))
				Log("cdstream", "fno %x not emulated", call.fno);
			call.Ret(0, 0);
			break;
		}
	}
	void Reset() override
	{
		head_ = 0;
		std::fill(std::begin(error_), std::end(error_), 0u);
	}

private:
	uint32_t head_ = 0;
	uint32_t error_[8] = {};
};

class Sampman : public RpcServer
{
public:
	Sampman(const char* name, const char* tag) : name_(name), tag_(tag) {}
	const char* Name() const override { return name_; }
	void Call(RpcCall& call) override
	{
		// Default reply 0: "idle"/"done" for the status queries (music 0x080e00 polls for 0).
		uint32_t ret = 0;
		switch (call.fno)
		{
		case 0x000a0000: // sampman init: > 0 is success
		case 0x00080000: // music init: != 0 is success
			Log(tag_, "init (fno %x): accepted, audio is silent", call.fno);
			ret = 1;
			break;
		case 0x000a0b00: // sample offset/size of sample #arg
			ret = 0;
			break;
		case 0x000a0c00: // sample rate of sample #arg (must be <= 48000)
			ret = 22050;
			break;
		case 0x000b0100: // bank load status: the boot loops until byte 0 == 1
			ret = 1;
			break;
		case 0x00090000: // music: -1 = failure
			ret = 0;
			break;
		default:
			break;
		}
		char key[48];
		snprintf(key, sizeof key, "%s-%x", tag_, call.fno);
		if (LogLimited(key))
			Log(tag_, "command %08x (arg %08x %08x) -> %u", call.fno, call.Arg(0), call.Arg(1), ret);
		call.Ret(0, ret);
	}

private:
	const char* name_;
	const char* tag_;
};

CdStream g_cdstream;
Sampman g_sampman("SAMPMAN2 sfx (silent)", "sampman");
Sampman g_music("SAMPMAN2 music (silent)", "music");

} // namespace

void RegisterGameDrivers()
{
	RegisterServer(0x65686577, &g_cdstream);
	RegisterServer(0x65686579, &g_sampman);
	RegisterServer(0x65686578, &g_music);
}

} // namespace vc::iop
