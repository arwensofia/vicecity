// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal interface of runtime/iop: the emulated IOP's state, EE memory access, and the RPC
// server registry that the SIF HLE (sif_hle.cpp) dispatches into. See README.md.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "iso_fs.h"
#include "mc_fs.h"
#include "ps2_abi.h"

namespace vc::iop {

// ---------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------

void Log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// Logs only the first `limit` occurrences of `key` (per tag).
bool LogLimited(const std::string& key, int limit = 1);

// ---------------------------------------------------------------------------------------
// EE memory. Addresses are EE virtual addresses (KSEG0/KSEG1/uncached mirrors accepted).
// Every access is bounds-checked; out-of-range accesses fail and are logged.
// ---------------------------------------------------------------------------------------

uint8_t* EePtr(uint32_t addr, uint32_t len);
bool EeRead(uint32_t addr, void* dst, uint32_t len);
bool EeWrite(uint32_t addr, const void* src, uint32_t len);
uint32_t EeRead32(uint32_t addr);
void EeWrite32(uint32_t addr, uint32_t v);
// Reads a NUL-terminated string of at most `max` bytes.
std::string EeString(uint32_t addr, uint32_t max);

// ---------------------------------------------------------------------------------------
// IOP memory: 2 MB, target of sceSifSetDma and owner of sceSifAllocIopHeap blocks.
// ---------------------------------------------------------------------------------------

constexpr uint32_t kIopRamSize = 2u << 20;
constexpr uint32_t kIopHeapBase = 0x00100000; // below: "resident modules"
constexpr uint32_t kIopHeapEnd = 0x001F0000;

uint8_t* IopPtr(uint32_t addr, uint32_t len);
uint32_t IopAlloc(uint32_t size); // 0 on failure
int IopFree(uint32_t addr);       // 0 ok, -1 unknown block
uint32_t IopHeapFree();           // total free bytes

// ---------------------------------------------------------------------------------------
// RPC servers
// ---------------------------------------------------------------------------------------

struct RpcCall
{
	uint32_t sid = 0;
	uint32_t fno = 0;
	uint32_t mode = 0;
	uint32_t client = 0;              // EE address of the sceSifClientData
	uint32_t send_addr = 0;
	std::vector<uint8_t> send;        // copy of the EE send buffer
	uint32_t recv_addr = 0;
	std::vector<uint8_t> recv;        // pre-filled with the EE recv buffer; written back after Call

	uint32_t Arg(size_t i) const
	{
		uint32_t v = 0;
		if ((i + 1) * 4 <= send.size())
			std::memcpy(&v, send.data() + i * 4, 4);
		return v;
	}
	void Ret(size_t i, uint32_t v)
	{
		if ((i + 1) * 4 <= recv.size())
			std::memcpy(recv.data() + i * 4, &v, 4);
	}
};

class RpcServer
{
public:
	virtual ~RpcServer() = default;
	virtual const char* Name() const = 0;
	virtual void Call(RpcCall& call) = 0;
	virtual void Reset() {}
};

void RegisterServer(uint32_t sid, RpcServer* server);
RpcServer* FindServer(uint32_t sid);
// Dispatches one call (used by sceSifCallRpc and by tests). Unknown servers/functions are
// logged once and leave `recv` untouched.
void DispatchRpc(RpcCall& call);

// ---------------------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------------------

struct State
{
	bool initialized = false;
	uint8_t* ee_ram = nullptr;
	std::string data_dir;
	std::string iso_path;
	IsoImage iso;
	std::unique_ptr<McFs> mc[2]; // mc[0] = slot 1 (card inserted), mc[1] = slot 2 (empty)
	std::vector<std::string> modules; // loaded IRX names
};

State& St();

// Services (svc_*.cpp). Each registers its servers and returns nothing.
void RegisterLoadfile();
void RegisterFileio();
void RegisterIopHeap();
void RegisterCdvd();
void RegisterPad();
void RegisterMc();
void RegisterSdr();
void RegisterGameDrivers();
void ResetServices(); // IOP reboot: forget open files, stream state, loaded modules

// Pad (svc_pad.cpp): writes the current input into every open pad area. Called per vblank.
void PadVblank();

// fileio (svc_fileio.cpp), used by the sceOpen/... overrides.
int FileOpen(const std::string& path, int flags);
int FileClose(int fd);
int FileRead(int fd, uint32_t ee_buf, int size);
int FileWrite(int fd, uint32_t ee_buf, int size);
int FileLseek(int fd, int offset, int whence);

// Calls an EE function as an interrupt-context callback would: all EE registers are saved
// and restored around the call, a0..a3 are set, and the stack pointer is moved below the
// current frame. Requires the EE context of the calling thread.
void CallEe(ps2_cpu* c, uint32_t func, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);
// Native replacement for an EE callback that CallEe runs instead of the recompiled code
// (used for library end functions that are only reachable through pointers and may be
// missing from the recompiled function table).
using NativeEeFunc = void (*)(uint32_t a0);
void RegisterNativeEe(uint32_t addr, NativeEeFunc fn);

// JST (PS2 RTC time zone) broken-down time for "now".
struct Clock
{
	int sec, min, hour, day, month, year;
};
Clock NowJst();
inline uint8_t ToBcd(int v)
{
	return uint8_t(((v / 10) % 10) << 4 | (v % 10));
}

} // namespace vc::iop
