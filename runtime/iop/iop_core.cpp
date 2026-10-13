// SPDX-License-Identifier: GPL-3.0-or-later
//
// IOP core: state, EE/IOP memory, RPC server registry, and the vc_iop.h entry points that the
// EE kernel calls (init, vblank/poll, SIF registers and SIF DMA).

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <set>
#include <unordered_map>

#include "iop.h"
#include "vc_iop.h"

namespace vc::iop {

// ---------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------

void Log(const char* tag, const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	vc_log(tag, "%s", buf);
}

bool LogLimited(const std::string& key, int limit)
{
	static std::mutex m;
	static std::unordered_map<std::string, int> counts;
	std::lock_guard<std::mutex> lock(m);
	return ++counts[key] <= limit;
}

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------

State& St()
{
	static State s;
	return s;
}

// ---------------------------------------------------------------------------------------
// EE memory
// ---------------------------------------------------------------------------------------

uint8_t* EePtr(uint32_t addr, uint32_t len)
{
	if (addr >= PS2_SPR_BASE && addr < PS2_SPR_BASE + PS2_SPR_SIZE)
	{
		uint32_t off = addr - PS2_SPR_BASE;
		if (uint64_t(off) + len > PS2_SPR_SIZE)
			return nullptr;
		uint8_t* spr = vc_ee_ptr(PS2_SPR_BASE);
		return spr ? spr + off : nullptr;
	}
	uint32_t seg = addr >> 28;
	uint32_t phys;
	if (seg == 0x0 || seg == 0x2 || seg == 0x3)
		phys = addr & 0x0FFFFFFFu;
	else if (seg == 0x8 || seg == 0x9 || seg == 0xA || seg == 0xB)
		phys = addr & 0x1FFFFFFFu;
	else
		return nullptr;
	if (uint64_t(phys) + len > PS2_RAM_SIZE)
		return nullptr;
	uint8_t* ram = St().ee_ram;
	if (!ram)
		ram = vc_ee_ptr(0);
	return ram ? ram + phys : nullptr;
}

bool EeRead(uint32_t addr, void* dst, uint32_t len)
{
	if (len == 0)
		return true;
	uint8_t* p = EePtr(addr, len);
	if (!p)
	{
		Log("iop", "EE read out of range: %08x+%x", addr, len);
		std::memset(dst, 0, len);
		return false;
	}
	std::memcpy(dst, p, len);
	return true;
}

bool EeWrite(uint32_t addr, const void* src, uint32_t len)
{
	if (len == 0)
		return true;
	uint8_t* p = EePtr(addr, len);
	if (!p)
	{
		Log("iop", "EE write out of range: %08x+%x", addr, len);
		return false;
	}
	std::memcpy(p, src, len);
	return true;
}

uint32_t EeRead32(uint32_t addr)
{
	uint32_t v = 0;
	EeRead(addr, &v, 4);
	return v;
}

void EeWrite32(uint32_t addr, uint32_t v)
{
	EeWrite(addr, &v, 4);
}

std::string EeString(uint32_t addr, uint32_t max)
{
	std::string s;
	for (uint32_t i = 0; i < max; i++)
	{
		uint8_t* p = EePtr(addr + i, 1);
		if (!p || *p == 0)
			break;
		s += char(*p);
	}
	return s;
}

// ---------------------------------------------------------------------------------------
// IOP memory and heap (first-fit over [kIopHeapBase, kIopHeapEnd), 64-byte granularity)
// ---------------------------------------------------------------------------------------

namespace {

std::vector<uint8_t>& IopRam()
{
	static std::vector<uint8_t> ram(kIopRamSize);
	return ram;
}

std::map<uint32_t, uint32_t>& IopBlocks() // addr -> size
{
	static std::map<uint32_t, uint32_t> blocks;
	return blocks;
}

} // namespace

uint8_t* IopPtr(uint32_t addr, uint32_t len)
{
	addr &= 0x1FFFFFFFu; // IOP KSEG0/KSEG1
	if (uint64_t(addr) + len > kIopRamSize)
		return nullptr;
	return IopRam().data() + addr;
}

uint32_t IopAlloc(uint32_t size)
{
	if (size == 0 || size > kIopHeapEnd - kIopHeapBase)
		return 0;
	size = (size + 63) & ~63u;
	auto& blocks = IopBlocks();
	uint32_t cur = kIopHeapBase;
	for (const auto& [addr, sz] : blocks)
	{
		if (addr - cur >= size)
			break;
		cur = addr + sz;
	}
	if (uint64_t(cur) + size > kIopHeapEnd)
		return 0;
	blocks[cur] = size;
	return cur;
}

int IopFree(uint32_t addr)
{
	addr &= 0x1FFFFFFFu;
	return IopBlocks().erase(addr) ? 0 : -1;
}

uint32_t IopHeapFree()
{
	uint32_t used = 0;
	for (const auto& [addr, sz] : IopBlocks())
		used += sz;
	return (kIopHeapEnd - kIopHeapBase) - used;
}

// ---------------------------------------------------------------------------------------
// RPC registry
// ---------------------------------------------------------------------------------------

namespace {

std::map<uint32_t, RpcServer*>& Servers()
{
	static std::map<uint32_t, RpcServer*> servers;
	return servers;
}

} // namespace

void RegisterServer(uint32_t sid, RpcServer* server)
{
	Servers()[sid] = server;
}

RpcServer* FindServer(uint32_t sid)
{
	auto it = Servers().find(sid);
	return it == Servers().end() ? nullptr : it->second;
}

void DispatchRpc(RpcCall& call)
{
	RpcServer* s = FindServer(call.sid);
	if (!s)
	{
		char key[64];
		snprintf(key, sizeof key, "rpc-unknown-%08x-%x", call.sid, call.fno);
		if (LogLimited(key))
			Log("sif", "call to unknown server %08x fno %x (ssize %zu rsize %zu) ignored", call.sid,
			    call.fno, call.send.size(), call.recv.size());
		return;
	}
	s->Call(call);
}

void ResetServices()
{
	for (auto& [sid, s] : Servers())
		s->Reset();
	St().modules.clear();
	IopBlocks().clear();
}

Clock NowJst()
{
	time_t t = time(nullptr) + 9 * 3600;
	struct tm tm;
	gmtime_r(&t, &tm);
	return Clock{tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900};
}

// ---------------------------------------------------------------------------------------
// SIF registers and DMA
// ---------------------------------------------------------------------------------------

namespace {

// SMFLG bits reported by the (always booted) IOP: SIF initialised, command system ready,
// boot finished.
constexpr uint32_t kSmflgReady = 0x00010000 | 0x00020000 | 0x00040000;

struct SifRegs
{
	uint32_t mscom = 0, smcom = 0, msflg = 0, ctrl = 0, bd6 = 0;
	uint32_t sreg[32] = {};
	uint32_t dma_id = 0;
	SifRegs()
	{
		// Software registers read by sceSifInitCmd / sceSifInitRpc: non-zero means "the IOP
		// side is already set up", which skips their handshakes if the original code runs.
		sreg[0] = 0x0001F000; // IOP command buffer
		sreg[1] = 0x0001F100;
		sreg[2] = 1;          // RPC initialised
	}
};

SifRegs& Sif()
{
	static SifRegs r;
	return r;
}

} // namespace

} // namespace vc::iop

using namespace vc::iop;

extern "C" {

int vc_iop_init(const vc_iop_config* cfg)
{
	State& s = St();
	if (cfg && cfg->ee_ram)
		s.ee_ram = cfg->ee_ram;
	if (cfg && cfg->data_dir && cfg->data_dir[0])
		s.data_dir = cfg->data_dir;
	else
	{
		const char* home = getenv("HOME");
		s.data_dir = std::string(home ? home : ".") + "/Library/Application Support/vicecity";
	}
	const char* iso = (cfg && cfg->iso_path) ? cfg->iso_path : getenv("VC_ISO");
	int rc = 0;
	if (iso && iso[0])
	{
		std::string err;
		if (s.iso.Open(iso, &err))
		{
			s.iso_path = iso;
			Log("iop", "disc image '%s': volume '%s', %llu sectors", iso, s.iso.VolumeId().c_str(),
			    (unsigned long long)s.iso.SectorCount());
		}
		else
		{
			Log("iop", "cannot use disc image: %s", err.c_str());
			rc = -1;
		}
	}
	else
		Log("iop", "no disc image configured; cdrom0: is empty");

	s.mc[0] = std::make_unique<McFs>(s.data_dir + "/mc0");
	if (!s.mc[0]->Formatted())
	{
		int r = s.mc[0]->Format();
		Log("mc", "memory card 1: new formatted card at '%s' (%d)", s.mc[0]->HostDir().c_str(), r);
	}
	s.mc[1].reset();

	if (!s.initialized)
	{
		RegisterLoadfile();
		RegisterFileio();
		RegisterIopHeap();
		RegisterCdvd();
		RegisterPad();
		RegisterMc();
		RegisterSdr();
		RegisterGameDrivers();
	}
	s.initialized = true;
	return rc;
}

void vc_iop_shutdown(void)
{
	State& s = St();
	ResetServices();
	if (s.mc[0])
		s.mc[0]->CloseAll();
	s.iso.Close();
}

int vc_iop_read_disc_file(const char* path, void** data, size_t* size)
{
	IsoEntry e;
	if (!path || !St().iso.IsOpen() || !St().iso.Find(path, &e) || e.is_dir)
		return -1;
	void* buf = malloc(e.size ? e.size : 1);
	if (!buf)
		return -1;
	if (St().iso.ReadFile(e, 0, buf, e.size) != int64_t(e.size))
	{
		free(buf);
		return -1;
	}
	*data = buf;
	*size = e.size;
	return 0;
}

void vc_iop_poll(void)
{
	// All RPC work completes synchronously inside sceSifCallRpc; nothing is pending.
}

void vc_iop_vblank(void)
{
	PadVblank();
}

uint32_t vc_iop_sif_read32(uint32_t addr)
{
	SifRegs& r = Sif();
	switch (addr & 0xF0)
	{
	case 0x00: return r.mscom;
	case 0x10: return r.smcom;
	case 0x20: return r.msflg;
	case 0x30: return kSmflgReady;
	case 0x40: return r.ctrl;
	case 0x60: return r.bd6;
	default: return 0;
	}
}

void vc_iop_sif_write32(uint32_t addr, uint32_t value)
{
	SifRegs& r = Sif();
	switch (addr & 0xF0)
	{
	case 0x00: r.mscom = value; break;
	case 0x20: r.msflg |= value; break; // EE sets MSFLG bits
	case 0x30: break;                    // EE clears SMFLG bits; the IOP is always ready
	case 0x40: r.ctrl = value; break;
	case 0x60: r.bd6 = value; break;
	default: break;
	}
}

uint32_t vc_iop_sif_set_dma(ps2_cpu* c, uint32_t desc, uint32_t count)
{
	(void)c;
	SifRegs& r = Sif();
	for (uint32_t i = 0; i < count && i < 32; i++)
	{
		uint32_t d[4];
		if (!EeRead(desc + i * 16, d, 16))
			break;
		uint32_t src = d[0], dst = d[1], size = d[2];
		uint8_t* to = IopPtr(dst, size);
		uint8_t* from = EePtr(src, size);
		if (!to || !from)
		{
			Log("sif", "SIF1 DMA dropped: EE %08x -> IOP %08x size %x", src, dst, size);
			continue;
		}
		std::memcpy(to, from, size);
	}
	if (++r.dma_id == 0)
		r.dma_id = 1;
	return r.dma_id;
}

int32_t vc_iop_sif_dma_stat(ps2_cpu* c, uint32_t id)
{
	(void)c;
	(void)id;
	return -1; // transfers complete synchronously
}

void vc_iop_sif_set_dchain(ps2_cpu* c)
{
	(void)c;
}

uint32_t vc_iop_sif_set_reg(ps2_cpu* c, uint32_t reg, uint32_t value)
{
	uint32_t old = vc_iop_sif_get_reg(c, reg);
	SifRegs& r = Sif();
	if (reg & 0x80000000u)
	{
		if ((reg & 0x7FFFFFFFu) < 32)
			r.sreg[reg & 31] = value;
		return old;
	}
	switch (reg)
	{
	case 1: r.mscom = value; break;
	case 3: r.msflg |= value; break;
	case 4: break; // clearing SMFLG bits: the IOP reboots instantly, so they stay set
	default: break;
	}
	return old;
}

uint32_t vc_iop_sif_get_reg(ps2_cpu* c, uint32_t reg)
{
	(void)c;
	SifRegs& r = Sif();
	if (reg & 0x80000000u)
		return (reg & 0x7FFFFFFFu) < 32 ? r.sreg[reg & 31] : 0;
	switch (reg)
	{
	case 1: return r.mscom;
	case 2: return r.smcom;
	case 3: return r.msflg;
	case 4: return kSmflgReady;
	default: return 0;
	}
}

void vc_iop_sif_stop_dma(ps2_cpu* c)
{
	(void)c;
}

void vc_iop_dma_start(int channel)
{
	if (LogLimited("dma-start-" + std::to_string(channel)))
		Log("sif", "EE started SIF DMA channel %d directly; not emulated (SIF is HLE'd above the "
		           "DMAC)", channel);
}

} // extern "C"
