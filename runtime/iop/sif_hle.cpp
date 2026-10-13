// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE-side HLE of libkernl's SIF command/RPC layer and fileio API (SDK 2.4 as linked into
// SLUS_205.52). Each ps2f_XXXXXXXX below is a strong override of the recompiled weak function
// at that address (addresses from tools/library_names.csv). Libraries above this layer
// (libcdvd, libmc, libpad, libsdr, loadfile, IOP heap, the game's own drivers) stay
// recompiled and reach the native services through sceSifBindRpc/sceSifCallRpc.
//
// Completion model: an RPC is executed synchronously inside sceSifCallRpc. Its reply is in
// EE memory and its end function (if any) has run before sceSifCallRpc returns, so
// sceSifCheckStatRpc always reports "done". Non-blocking callers (libmc, libcdvd N commands)
// poll and find the result immediately.

#include <cinttypes>

#include "iop.h"
#include "vc_iop.h"

using namespace vc::iop;

namespace {

// sceSifClientData field offsets.
constexpr uint32_t kCdPaddr = 0x00;
constexpr uint32_t kCdPid = 0x04;
constexpr uint32_t kCdSema = 0x08;
constexpr uint32_t kCdCommand = 0x10;
constexpr uint32_t kCdEndFunc = 0x1c;
constexpr uint32_t kCdEndParam = 0x20;
constexpr uint32_t kCdServe = 0x24;

constexpr uint32_t kMaxRpcBuffer = 0x10000;

// Value written to sceSifClientData::serve: any non-zero IOP address means "bound".
constexpr uint32_t kFakeServeBase = 0x0001E000;

inline uint32_t A(ps2_cpu* c, int r)
{
	return c->r[r].uw[0];
}

inline void Ret(ps2_cpu* c, int32_t v)
{
	c->r[2].sd[0] = v;
}

struct CmdHandler
{
	uint32_t func = 0;
	uint32_t data = 0;
};

std::map<uint32_t, CmdHandler>& CmdHandlers()
{
	static std::map<uint32_t, CmdHandler> h;
	return h;
}

std::map<uint32_t, uint32_t>& BoundServers() // sid -> fake serve address
{
	static std::map<uint32_t, uint32_t> m;
	return m;
}

std::map<uint32_t, uint32_t>& Clients() // EE sceSifClientData address -> sid
{
	static std::map<uint32_t, uint32_t> m;
	return m;
}

const char* Fourcc(uint32_t sid, char* buf)
{
	// Rockstar's driver ids are ASCII ("ehew" = 0x65686577 ...); print them readably.
	for (int i = 0; i < 4; i++)
	{
		char ch = char(sid >> (24 - 8 * i));
		buf[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
	}
	buf[4] = 0;
	return buf;
}

} // namespace

namespace vc::iop {

static std::map<uint32_t, NativeEeFunc>& NativeEe()
{
	static std::map<uint32_t, NativeEeFunc> m;
	return m;
}

void RegisterNativeEe(uint32_t addr, NativeEeFunc fn)
{
	NativeEe()[addr] = fn;
}

void CallEe(ps2_cpu* c, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	if (!func)
		return;
	auto it = NativeEe().find(func);
	if (it != NativeEe().end())
	{
		it->second(a0);
		return;
	}
	if (!c)
		return;
	ps2_cpu saved;
	std::memcpy(&saved, c, sizeof saved);
	c->r[4].sd[0] = int32_t(a0);
	c->r[5].sd[0] = int32_t(a1);
	c->r[6].sd[0] = int32_t(a2);
	c->r[7].sd[0] = int32_t(a3);
	// Interrupt handlers run below the interrupted frame; leave a gap and keep alignment.
	c->r[29].ud[0] = (c->r[29].ud[0] - 0x200) & ~uint64_t(0xF);
	c->r[31].ud[0] = c->pc;
	ps2_call_indirect(c, c->pc, func);
	std::memcpy(c, &saved, sizeof saved);
}

} // namespace vc::iop

extern "C" {

// ---------------------------------------------------------------------------------------
// SIF command layer
// ---------------------------------------------------------------------------------------

// sceSifInitCmd(void)
void ps2f_001157a0(ps2_cpu* c)
{
	Ret(c, 0);
}

// sceSifExitCmd(void)
void ps2f_00116010(ps2_cpu* c)
{
	Ret(c, 0);
}

// sceSifAddCmdHandler(int cid, sceSifCmdHandler func, void* data)
void ps2f_00115a88(ps2_cpu* c)
{
	uint32_t cid = A(c, 4);
	CmdHandlers()[cid] = CmdHandler{A(c, 5), A(c, 6)};
	Log("sif", "EE command handler %08x -> %08x (data %08x)", cid, A(c, 5), A(c, 6));
}

// sceSifRemoveCmdHandler(int cid)
void ps2f_00115ab8(ps2_cpu* c)
{
	CmdHandlers().erase(A(c, 4));
}

static void SendCmd(ps2_cpu* c)
{
	uint32_t cid = A(c, 4), pkt = A(c, 5), psize = A(c, 6);
	uint32_t src = A(c, 7), dest = A(c, 8), size = A(c, 9);
	if (size > 0)
	{
		uint8_t* to = IopPtr(dest, size);
		uint8_t* from = EePtr(src, size);
		if (to && from)
			std::memcpy(to, from, size);
	}
	char key[48];
	snprintf(key, sizeof key, "sendcmd-%08x", cid);
	if (LogLimited(key))
		Log("sif", "sceSifSendCmd %08x (packet %08x/%u, data %u bytes) accepted, no IOP handler",
		    cid, pkt, psize, size);
	static uint32_t id = 0;
	Ret(c, int32_t(++id == 0 ? ++id : id));
}

// sceSifSendCmd(cid, packet, packet_size, src_extra, dest_extra, size_extra)
void ps2f_00115c18(ps2_cpu* c)
{
	SendCmd(c);
}

// isceSifSendCmd
void ps2f_00115c58(ps2_cpu* c)
{
	SendCmd(c);
}

// ---------------------------------------------------------------------------------------
// SIF RPC (client side)
// ---------------------------------------------------------------------------------------

// sceSifInitRpc(int mode)
void ps2f_00115e70(ps2_cpu* c)
{
	Ret(c, 0);
}

// sceSifBindRpc(sceSifClientData* cd, unsigned int sid, int mode)
void ps2f_001164e0(ps2_cpu* c)
{
	uint32_t cd = A(c, 4), sid = A(c, 5);
	auto& bound = BoundServers();
	auto it = bound.find(sid);
	if (it == bound.end())
	{
		uint32_t serve = kFakeServeBase + uint32_t(bound.size()) * 0x40;
		it = bound.emplace(sid, serve).first;
		RpcServer* s = FindServer(sid);
		char cc[5];
		Log("sif", "bind %08x ('%s') -> %s", sid, Fourcc(sid, cc), s ? s->Name() : "UNKNOWN (calls ignored)");
	}
	EeWrite32(cd + kCdPaddr, 0);
	EeWrite32(cd + kCdPid, 0);
	EeWrite32(cd + kCdSema, 0xFFFFFFFFu);
	EeWrite32(cd + kCdCommand, 0);
	EeWrite32(cd + kCdServe, it->second);
	Clients()[cd & 0x01FFFFFFu] = sid;
	Ret(c, 0);
}

// sceSifCallRpc(cd, fno, mode, send, ssize, recv, rsize, end_func, end_param)
void ps2f_001166b0(ps2_cpu* c)
{
	uint32_t cd = A(c, 4);
	RpcCall call;
	call.client = cd;
	call.fno = A(c, 5);
	call.mode = A(c, 6);
	call.send_addr = A(c, 7);
	uint32_t ssize = A(c, 8);
	call.recv_addr = A(c, 9);
	uint32_t rsize = A(c, 10);
	uint32_t end_func = A(c, 11);
	uint32_t end_param = EeRead32(A(c, 29)); // 9th argument: first stack slot

	auto cl = Clients().find(cd & 0x01FFFFFFu);
	if (cl == Clients().end() || EeRead32(cd + kCdServe) == 0)
	{
		Log("sif", "sceSifCallRpc on unbound client %08x (fno %x)", cd, call.fno);
		Ret(c, -1);
		return;
	}
	call.sid = cl->second;
	if (ssize > kMaxRpcBuffer || rsize > kMaxRpcBuffer)
	{
		Log("sif", "sceSifCallRpc %08x fno %x: buffer too large (send %x recv %x)", call.sid, call.fno,
		    ssize, rsize);
		Ret(c, -1);
		return;
	}
	call.send.resize(ssize);
	if (call.send_addr && ssize)
		EeRead(call.send_addr, call.send.data(), ssize);
	call.recv.resize(rsize);
	if (call.recv_addr && rsize)
		EeRead(call.recv_addr, call.recv.data(), rsize);

	DispatchRpc(call);

	if (call.recv_addr && rsize)
		EeWrite(call.recv_addr, call.recv.data(), rsize);
	EeWrite32(cd + kCdPaddr, 0); // no packet in flight: sceSifCheckStatRpc == 0
	EeWrite32(cd + kCdEndFunc, end_func);
	EeWrite32(cd + kCdEndParam, end_param);
	if (end_func)
		CallEe(c, end_func, end_param);
	Ret(c, 0);
}

// sceSifCheckStatRpc(sceSifClientData* cd)
void ps2f_001168a0(ps2_cpu* c)
{
	Ret(c, 0);
}

// ---------------------------------------------------------------------------------------
// IOP reset / sync
// ---------------------------------------------------------------------------------------

// sceSifRebootIop(const char* img)
void ps2f_0011b668(ps2_cpu* c)
{
	std::string img = EeString(A(c, 4), 256);
	Log("iop", "IOP reboot requested with '%s' (emulated IOP: reset services)", img.c_str());
	ResetServices();
	Ret(c, 1);
}

// sceSifResetIop(const char* arg, int mode)
void ps2f_0011b4c8(ps2_cpu* c)
{
	ResetServices();
	Ret(c, 1);
}

// sceSifSyncIop(void)
void ps2f_0011b630(ps2_cpu* c)
{
	Ret(c, 1);
}

// sceSifIsAliveIop(void) -- SMFLG & SIFINIT
void ps2f_0011b608(ps2_cpu* c)
{
	Ret(c, 1);
}

// ---------------------------------------------------------------------------------------
// fileio (sceOpen & co.): the SDK 2.4 protocol (IOP->EE command 0x80000011/13 callbacks and
// semaphores) is replaced at the API level.
// ---------------------------------------------------------------------------------------

// _sceFsInit (binds fileio; only reached from the overridden calls)
void ps2f_00117438(ps2_cpu* c)
{
	Ret(c, 0);
}

// int sceOpen(const char* name, int flags, ...)
void ps2f_001176c8(ps2_cpu* c)
{
	std::string name = EeString(A(c, 4), 1024);
	Ret(c, FileOpen(name, int(A(c, 5))));
}

// int sceClose(int fd)
void ps2f_00117950(ps2_cpu* c)
{
	Ret(c, FileClose(int(A(c, 4))));
}

// int sceLseek(int fd, int offset, int whence)
void ps2f_00117ad0(ps2_cpu* c)
{
	Ret(c, FileLseek(int(A(c, 4)), int(A(c, 5)), int(A(c, 6))));
}

// int sceRead(int fd, void* buf, int size)
void ps2f_00117d08(ps2_cpu* c)
{
	Ret(c, FileRead(int(A(c, 4)), A(c, 5), int(A(c, 6))));
}

// int sceWrite(int fd, const void* buf, int size)
void ps2f_00117f78(ps2_cpu* c)
{
	Ret(c, FileWrite(int(A(c, 4)), A(c, 5), int(A(c, 6))));
}

} // extern "C"
