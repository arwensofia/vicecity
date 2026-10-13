// SPDX-License-Identifier: GPL-3.0-or-later
//
// IOP system services: LOADFILE (module loading), IOP heap, fileio (cdrom0:, rom0:ROMVER),
// and libsd remote (sdrdrv, silent).

#include <algorithm>

#include "iop.h"

namespace vc::iop {

namespace {

constexpr int kENOENT = 2, kEBADF = 9, kEFAULT = 14, kEINVAL = 22, kEMFILE = 24, kENODEV = 19,
              kEROFS = 30;

// ---------------------------------------------------------------------------------------
// LOADFILE (0x80000006)
// ---------------------------------------------------------------------------------------

// Modules whose services runtime/iop provides (or that need none).
const char* const kKnownModules[] = {
	"SIO2MAN", "PADMAN", "LIBSD", "SDRDRV", "MCMAN", "MCSERV", "CDSTREAM", "SAMPMAN2",
	"CDVDMAN", "CDVDFSV", "FILEIO", "IOMAN", "LOADFILE", "SIFCMD", "SIFMAN", "MODHSYN",
	"MODMIDI", "XSIO2MAN", "XPADMAN", "XMCMAN", "XMCSERV", "IOPRP",
};

std::string ModuleName(const std::string& path)
{
	size_t b = path.find_last_of("\\/:");
	std::string n = b == std::string::npos ? path : path.substr(b + 1);
	size_t dot = n.find_first_of(".;");
	if (dot != std::string::npos)
		n.resize(dot);
	for (char& ch : n)
		ch = char(toupper(static_cast<unsigned char>(ch)));
	return n;
}

class Loadfile : public RpcServer
{
public:
	const char* Name() const override { return "LOADFILE"; }
	void Call(RpcCall& call) override
	{
		if (call.fno == 0xff)
		{
			// Library version check against "2410" (sceSifLoadModule refuses a mismatch).
			if (call.recv.size() >= 4)
				std::memcpy(call.recv.data(), "2410", 4);
			return;
		}
		if (call.fno == 0 || call.fno == 1)
		{
			std::string path, args;
			if (call.send.size() >= 0x104)
				path.assign(reinterpret_cast<const char*>(call.send.data() + 8),
				            strnlen(reinterpret_cast<const char*>(call.send.data() + 8), 252));
			std::string name = ModuleName(path);
			bool known = std::any_of(std::begin(kKnownModules), std::end(kKnownModules),
			                         [&](const char* k) { return name == k; });
			bool on_disc = true;
			if (path.compare(0, 6, "cdrom0") == 0 || path.compare(0, 5, "cdrom") == 0)
			{
				IsoEntry e;
				on_disc = St().iso.IsOpen() && St().iso.Find(path, &e);
			}
			St().modules.push_back(name);
			int id = int(St().modules.size()) + 32;
			Log("iop", "load module '%s'%s%s -> id %d", path.c_str(),
			    known ? "" : " (UNKNOWN module, no service emulated)",
			    on_disc ? "" : " (not found on disc)", id);
			call.Ret(0, uint32_t(id));
			call.Ret(1, 0); // module start result: resident
			return;
		}
		Log("iop", "LOADFILE fno %x not supported", call.fno);
		call.Ret(0, uint32_t(-1));
	}
};

// ---------------------------------------------------------------------------------------
// IOP heap (0x80000003)
// ---------------------------------------------------------------------------------------

class IopHeap : public RpcServer
{
public:
	const char* Name() const override { return "IOP heap"; }
	void Call(RpcCall& call) override
	{
		switch (call.fno)
		{
		case 1: { // alloc(size)
			uint32_t a = IopAlloc(call.Arg(0));
			Log("iop", "sceSifAllocIopHeap(%u) -> %08x", call.Arg(0), a);
			call.Ret(0, a);
			break;
		}
		case 2: // free(addr)
			call.Ret(0, uint32_t(IopFree(call.Arg(0))));
			break;
		default:
			Log("iop", "IOP heap fno %x not supported", call.fno);
			call.Ret(0, uint32_t(-1));
			break;
		}
	}
};

// ---------------------------------------------------------------------------------------
// fileio: API-level (sceOpen & co. are overridden in sif_hle.cpp); the RPC server only
// exists so a bind succeeds.
// ---------------------------------------------------------------------------------------

class FileioServer : public RpcServer
{
public:
	const char* Name() const override { return "fileio (API-level HLE)"; }
	void Call(RpcCall& call) override
	{
		if (call.fno == 0xff && call.recv.size() >= 4)
			std::memcpy(call.recv.data(), "2410", 4);
		else if (LogLimited("fileio-rpc"))
			Log("fileio", "fileio RPC fno %x ignored (fileio is HLE'd at the API level)", call.fno);
	}
	void Reset() override;
};

struct OpenFile
{
	bool used = false;
	bool is_mem = false;
	IsoEntry entry;
	std::string mem;
	uint64_t pos = 0;
	uint64_t Size() const { return is_mem ? mem.size() : entry.size; }
};

constexpr int kMaxFiles = 32;
OpenFile g_files[kMaxFiles];

// rom0:ROMVER of a US console with BIOS 2.00 ("T" at [4] would mean a TOOL).
const char kRomver[] = "0200AC20020207\n";

void FileioServer::Reset()
{
	for (OpenFile& f : g_files)
		f = OpenFile();
}

// ---------------------------------------------------------------------------------------
// libsd remote (sdrdrv, 0x80000701): accepted, silent.
// ---------------------------------------------------------------------------------------

class Sdrdrv : public RpcServer
{
public:
	const char* Name() const override { return "SDRDRV (silent)"; }
	void Call(RpcCall& call) override
	{
		char key[32];
		snprintf(key, sizeof key, "sdr-%x", call.fno);
		if (LogLimited(key))
			Log("sdr", "sceSdRemote command %04x (args %08x %08x %08x) accepted, no audio", call.fno,
			    call.Arg(1), call.Arg(2), call.Arg(3));
		call.Ret(0, 0);
	}
};

Loadfile g_loadfile;
IopHeap g_iopheap;
FileioServer g_fileio;
Sdrdrv g_sdr;

} // namespace

void RegisterLoadfile()
{
	RegisterServer(0x80000006, &g_loadfile);
}

void RegisterIopHeap()
{
	RegisterServer(0x80000003, &g_iopheap);
}

void RegisterFileio()
{
	RegisterServer(0x80000001, &g_fileio);
}

void RegisterSdr()
{
	RegisterServer(0x80000701, &g_sdr);
}

int FileOpen(const std::string& path, int flags)
{
	size_t colon = path.find(':');
	std::string dev = colon == std::string::npos ? std::string() : path.substr(0, colon);
	for (char& ch : dev)
		ch = char(tolower(static_cast<unsigned char>(ch)));
	OpenFile f;
	f.used = true;
	if (dev == "cdrom0" || dev == "cdrom")
	{
		if ((flags & 3) == 2 || (flags & 3) == 3 || (flags & 0x0600))
		{
			Log("fileio", "open '%s' flags %x: read-only device", path.c_str(), flags);
			return -kEROFS;
		}
		if (!St().iso.IsOpen() || !St().iso.Find(path, &f.entry) || f.entry.is_dir)
		{
			Log("fileio", "open '%s': not found", path.c_str());
			return -kENOENT;
		}
	}
	else if (dev == "rom0" && (path.substr(colon + 1) == "ROMVER"))
	{
		f.is_mem = true;
		f.mem = kRomver;
	}
	else
	{
		Log("fileio", "open '%s': device '%s' not supported", path.c_str(), dev.c_str());
		return dev.empty() ? -kEINVAL : -kENODEV;
	}
	for (int fd = 1; fd < kMaxFiles; fd++)
	{
		if (!g_files[fd].used)
		{
			g_files[fd] = std::move(f);
			Log("fileio", "open '%s' -> fd %d (%llu bytes)", path.c_str(), fd,
			    (unsigned long long)g_files[fd].Size());
			return fd;
		}
	}
	return -kEMFILE;
}

static OpenFile* GetFile(int fd)
{
	if (fd <= 0 || fd >= kMaxFiles || !g_files[fd].used)
		return nullptr;
	return &g_files[fd];
}

int FileClose(int fd)
{
	OpenFile* f = GetFile(fd);
	if (!f)
		return -kEBADF;
	*f = OpenFile();
	return 0;
}

int FileRead(int fd, uint32_t ee_buf, int size)
{
	OpenFile* f = GetFile(fd);
	if (!f)
		return -kEBADF;
	if (size < 0)
		return -kEINVAL;
	uint64_t avail = f->pos < f->Size() ? f->Size() - f->pos : 0;
	uint32_t n = uint32_t(std::min<uint64_t>(avail, uint32_t(size)));
	if (n == 0)
		return 0;
	uint8_t* dst = EePtr(ee_buf, n);
	if (!dst)
		return -kEFAULT;
	if (f->is_mem)
		std::memcpy(dst, f->mem.data() + f->pos, n);
	else if (St().iso.ReadFile(f->entry, f->pos, dst, n) != int64_t(n))
		return -kEFAULT;
	f->pos += n;
	return int(n);
}

int FileWrite(int fd, uint32_t ee_buf, int size)
{
	(void)ee_buf;
	(void)size;
	return GetFile(fd) ? -kEROFS : -kEBADF;
}

int FileLseek(int fd, int offset, int whence)
{
	OpenFile* f = GetFile(fd);
	if (!f)
		return -kEBADF;
	int64_t base = whence == 0 ? 0 : whence == 1 ? int64_t(f->pos) : whence == 2 ? int64_t(f->Size()) : -1;
	if (base < 0)
		return -kEINVAL;
	int64_t np = base + offset;
	if (np < 0 || np > INT32_MAX)
		return -kEINVAL;
	f->pos = uint64_t(np);
	return int(np);
}

} // namespace vc::iop
