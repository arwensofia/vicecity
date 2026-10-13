// SPDX-License-Identifier: GPL-3.0-or-later
//
// mcserv (0x80000400) for libmc (SDK 2.4). Slot 1 (port 0) holds an 8 MB PS2 card backed by
// McFs; slot 2 (port 1) is empty. Buffer layouts, as built by libmc:
//   descriptor (48 bytes): fd/port @0, port/slot @4/@8, size @0x0c, offset @0x10,
//       origin @0x14, buffer @0x18, param @0x1c, data[16] @0x20
//   name block (0x414 bytes): port @0, slot @4, flags/mode @8, maxent @0x0c,
//       buffer/table @0x10, name[1024] @0x14
// Every call returns one int in the 4-byte receive buffer (sceMcSync's result).

#include <algorithm>

#include "iop.h"

namespace vc::iop {

namespace {

constexpr int kFdShift = 4; // fd = port << 4 | card handle

class Mcserv : public RpcServer
{
public:
	const char* Name() const override { return "MCSERV"; }

	void Call(RpcCall& call) override
	{
		int r = Dispatch(call);
		if (call.fno != 0xfe)
			call.Ret(0, uint32_t(r));
	}

	void Reset() override
	{
		for (bool& s : seen_)
			s = false;
		for (auto& c : St().mc)
			if (c)
				c->CloseAll();
	}

private:
	static McFs* Card(uint32_t port) { return port < 2 ? St().mc[port].get() : nullptr; }

	static McFs* CardForFd(int32_t fd, int* local)
	{
		if (fd < 0)
			return nullptr;
		*local = fd & ((1 << kFdShift) - 1);
		return Card(uint32_t(fd) >> kFdShift);
	}

	static std::string Name(const RpcCall& call)
	{
		if (call.send.size() < 0x15)
			return std::string();
		const char* p = reinterpret_cast<const char*>(call.send.data() + 0x14);
		return std::string(p, strnlen(p, std::min<size_t>(1024, call.send.size() - 0x14)));
	}

	int Dispatch(RpcCall& call)
	{
		switch (call.fno)
		{
		case 0xfe: // init: {result, mcserv version, mcman version}
			call.Ret(0, 0);
			call.Ret(1, 0x020b);
			call.Ret(2, 0x020f);
			Log("mc", "sceMcInit");
			return 0;
		case 0x01: return GetInfo(call);
		case 0x02: return Open(call);
		case 0x03: { // close(fd)
			int local;
			McFs* c = CardForFd(int32_t(call.Arg(0)), &local);
			return c ? c->Close(local) : kMcNoEntry;
		}
		case 0x04: { // seek(fd, offset, origin)
			int local;
			McFs* c = CardForFd(int32_t(call.Arg(0)), &local);
			return c ? c->Seek(local, int32_t(call.Arg(4)), int32_t(call.Arg(5))) : kMcNoEntry;
		}
		case 0x05: return Read(call);
		case 0x06: return Write(call);
		case 0x0a: { // flush(fd)
			int local;
			McFs* c = CardForFd(int32_t(call.Arg(0)), &local);
			return c ? c->Flush(local) : kMcNoEntry;
		}
		case 0x0c: return Chdir(call);
		case 0x0d: return GetDir(call);
		case 0x0e: return SetFileInfo(call);
		case 0x0f: { // delete(port, slot, name)
			McFs* c = Card(call.Arg(0));
			int r = c ? c->Delete(Name(call)) : kMcNoCard;
			Log("mc", "delete '%s' -> %d", Name(call).c_str(), r);
			return r;
		}
		case 0x10: { // format(port, slot)
			McFs* c = Card(call.Arg(1));
			int r = c ? c->Format() : kMcNoCard;
			Log("mc", "format port %u -> %d", call.Arg(1), r);
			return r;
		}
		case 0x11: { // unformat(port, slot)
			McFs* c = Card(call.Arg(1));
			int r = c ? c->Unformat() : kMcNoCard;
			Log("mc", "unformat port %u -> %d", call.Arg(1), r);
			return r;
		}
		case 0x12: { // get entry space(port, slot, path)
			McFs* c = Card(call.Arg(0));
			return c ? c->GetEntSpace(Name(call)) : kMcNoCard;
		}
		case 0x14: // change thread priority
		case 0x15:
			return 0;
		default:
			Log("mc", "mcserv fno %x not emulated", call.fno);
			return kMcNoEntry;
		}
	}

	int GetInfo(RpcCall& call)
	{
		uint32_t port = call.Arg(1);
		uint32_t param = call.Arg(7);
		McFs* c = Card(port);
		int32_t type = 0, free_clusters = 0, format = 0;
		int r;
		if (!c)
			r = kMcNoCard;
		else
		{
			type = 2; // sceMcTypePS2
			format = c->Formatted() ? 1 : 0;
			free_clusters = c->FreeClusters();
			r = seen_[port] ? kMcOk : (format ? kMcChangedCard : kMcNoFormat);
			seen_[port] = true;
		}
		// The end function copies type/free/format out of `param` into the caller's
		// variables (+0, +4, +0x90).
		EeWrite32(param + 0x00, uint32_t(type));
		EeWrite32(param + 0x04, uint32_t(free_clusters));
		EeWrite32(param + 0x90, uint32_t(format));
		if (LogLimited("mc-getinfo-" + std::to_string(port), 2))
			Log("mc", "sceMcGetInfo(port %u) -> %d (type %d, free %d, formatted %d)", port, r, type,
			    free_clusters, format);
		return r;
	}

	int Open(RpcCall& call)
	{
		uint32_t port = call.Arg(0);
		int mode = int(call.Arg(2));
		McFs* c = Card(port);
		if (!c)
			return kMcNoCard;
		std::string name = Name(call);
		int r = c->Open(name, mode);
		Log("mc", "%s '%s' (mode %x) -> %d", (mode & kMcCreateDir) ? "mkdir" : "open", name.c_str(), mode, r);
		if (r >= 0 && !(mode & kMcCreateDir))
			r |= int(port) << kFdShift;
		return r;
	}

	int Read(RpcCall& call)
	{
		int local;
		McFs* c = CardForFd(int32_t(call.Arg(0)), &local);
		uint32_t size = call.Arg(3), buf = call.Arg(6), param = call.Arg(7);
		// No unaligned head/tail for the end function to copy: data goes straight to `buf`.
		EeWrite32(param + 0, 0);
		EeWrite32(param + 4, 0);
		if (!c)
			return kMcNoEntry;
		if (size > (8u << 20))
			return kMcNoEntry;
		uint8_t* dst = EePtr(buf, size);
		if (!dst && size)
			return kMcNoEntry;
		return c->Read(local, dst, int(size));
	}

	int Write(RpcCall& call)
	{
		int local;
		McFs* c = CardForFd(int32_t(call.Arg(0)), &local);
		if (!c)
			return kMcNoEntry;
		uint32_t size = call.Arg(3), head = call.Arg(5), buf = call.Arg(6);
		if (head > 16 || size > (8u << 20) || call.send.size() < 0x30)
			return kMcNoEntry;
		std::vector<uint8_t> data(head + size);
		std::memcpy(data.data(), call.send.data() + 0x20, head);
		if (size && !EeRead(buf, data.data() + head, size))
			return kMcNoEntry;
		return c->Write(local, data.data(), int(data.size()));
	}

	int Chdir(RpcCall& call)
	{
		McFs* c = Card(call.Arg(0));
		uint32_t out = call.Arg(4);
		if (!c)
			return kMcNoCard;
		std::string prev;
		int r = c->Chdir(Name(call), &prev);
		prev.resize(std::min<size_t>(prev.size(), 1023));
		EeWrite(out, prev.c_str(), uint32_t(prev.size() + 1));
		Log("mc", "chdir '%s' -> %d (cwd %s)", Name(call).c_str(), r, c->Cwd().c_str());
		return r;
	}

	int GetDir(RpcCall& call)
	{
		McFs* c = Card(call.Arg(0));
		if (!c)
			return kMcNoCard;
		int cont = int(call.Arg(2)), maxent = int(call.Arg(3));
		uint32_t table = call.Arg(4);
		maxent = std::clamp(maxent, 0, 4096);
		std::vector<McDirEntry> ents;
		int r = c->GetDir(Name(call), cont, maxent, &ents);
		if (r > 0)
			EeWrite(table, ents.data(), uint32_t(ents.size() * sizeof(McDirEntry)));
		Log("mc", "getdir '%s' (cont %d, max %d) -> %d", Name(call).c_str(), cont, maxent, r);
		return r;
	}

	int SetFileInfo(RpcCall& call)
	{
		McFs* c = Card(call.Arg(0));
		if (!c)
			return kMcNoCard;
		McDirEntry info{};
		EeRead(call.Arg(4), &info, sizeof info);
		int r = c->SetFileInfo(Name(call), info, int(call.Arg(2)));
		Log("mc", "setfileinfo '%s' flags %x -> %d", Name(call).c_str(), call.Arg(2), r);
		return r;
	}

	bool seen_[2] = {};
};

Mcserv g_mcserv;

// libmc end functions that are reached only through the sceSifCallRpc end-function pointer
// (absent from the first recompiler output). Addresses and globals are SLUS_205.52's libmc.

// 0x33a558, sceMcGetInfo: copy type/free/format from the reply block into the caller's
// pointers, which sceMcGetInfo saved at 0x6ba2a8/ac/b0.
void McGetInfoEnd(uint32_t param)
{
	const uint32_t ptrs[3] = {EeRead32(0x006ba2a8), EeRead32(0x006ba2ac), EeRead32(0x006ba2b0)};
	const uint32_t offs[3] = {0x00, 0x04, 0x90};
	for (int i = 0; i < 3; i++)
		if (ptrs[i])
			EeWrite32(ptrs[i], EeRead32(param + offs[i]));
}

// 0x33a0d8, sceMcRead: copy the unaligned head/tail fragments {size1, size2, dst1, dst2,
// data1[64], data2[64]} into place.
void McReadEnd(uint32_t param)
{
	uint32_t size1 = EeRead32(param), size2 = EeRead32(param + 4);
	uint8_t tmp[64];
	if (size1 > 0 && size1 <= 64 && EeRead(param + 0x10, tmp, size1))
		EeWrite(EeRead32(param + 8), tmp, size1);
	if (size2 > 0 && size2 <= 64 && EeRead(param + 0x50, tmp, size2))
		EeWrite(EeRead32(param + 0xc), tmp, size2);
}

} // namespace

void RegisterMc()
{
	RegisterServer(0x80000400, &g_mcserv);
	RegisterNativeEe(0x0033a558, McGetInfoEnd);
	RegisterNativeEe(0x0033a0d8, McReadEnd);
}

} // namespace vc::iop
