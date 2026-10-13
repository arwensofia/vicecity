// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE kernel syscall table (ps2_syscall hook). Numbers follow the EE kernel; negative numbers
// are the interrupt-context ("i") variants. Includes the syscalls libkernl's _InitSys would
// have installed with kernel patches (alarms 0xFC-0xFF, TLB helpers 0x54-0x56), see
// docs/MODULES.md.

#include "kernel.h"
#include "log.h"
#include "vc_gs.h"
#include "vc_iop.h"

#include <cstring>
#include <map>

namespace vc::kernel {

namespace {

uint32_t A(const ps2_cpu* c, int n) { return c->r[4 + n].uw[0]; }
int32_t S(const ps2_cpu* c, int n) { return c->r[4 + n].sw[0]; }

void Ret(ps2_cpu* c, int64_t v) { c->r[2].ud[0] = static_cast<uint64_t>(v); }
void Ret32(ps2_cpu* c, int32_t v) { Ret(c, v); }

std::map<int32_t, uint32_t> g_user_syscalls; // SetSyscall registrations (logged, mostly unused)
bool g_log_syscalls = false;

const char* Name(int32_t n)
{
	switch (n < 0 ? -n : n) {
	case 2: return "SetGsCrt";
	case 4: return "Exit";
	case 6: return "LoadExecPS2";
	case 7: return "ExecPS2";
	case 13: return "SetVTLBRefillHandler";
	case 16: return "AddIntcHandler";
	case 17: return "RemoveIntcHandler";
	case 18: return "AddDmacHandler";
	case 19: return "RemoveDmacHandler";
	case 20: return "EnableIntc";
	case 21: return "DisableIntc";
	case 22: return "EnableDmac";
	case 23: return "DisableDmac";
	case 26: return "iEnableIntc";
	case 27: return "iDisableIntc";
	case 28: return "iEnableDmac";
	case 29: return "iDisableDmac";
	case 32: return "CreateThread";
	case 33: return "DeleteThread";
	case 34: return "StartThread";
	case 35: return "ExitThread";
	case 36: return "ExitDeleteThread";
	case 37: return "TerminateThread";
	case 41: return "ChangeThreadPriority";
	case 43: return "RotateThreadReadyQueue";
	case 47: return "GetThreadId";
	case 48: return "ReferThreadStatus";
	case 50: return "SleepThread";
	case 51: return "WakeupThread";
	case 52: return "iWakeupThread";
	case 55: return "SuspendThread";
	case 56: return "iSuspendThread";
	case 57: return "ResumeThread";
	case 60: return "SetupThread";
	case 61: return "SetupHeap";
	case 62: return "EndOfHeap";
	case 64: return "CreateSema";
	case 65: return "DeleteSema";
	case 66: return "SignalSema";
	case 67: return "iSignalSema";
	case 68: return "WaitSema";
	case 69: return "PollSema";
	case 74: return "SetOsdConfigParam";
	case 75: return "GetOsdConfigParam";
	case 100: return "FlushCache";
	case 107: return "sceSifStopDma";
	case 111: return "GetOsdConfigParam2";
	case 112: return "GsGetIMR";
	case 113: return "GsPutIMR";
	case 115: return "SetVSyncFlag";
	case 116: return "SetSyscall";
	case 118: return "sceSifDmaStat";
	case 119: return "sceSifSetDma";
	case 120: return "sceSifSetDChain";
	case 121: return "sceSifSetReg";
	case 122: return "sceSifGetReg";
	case 123: return "ExecOSD";
	case 124: return "Deci2Call";
	case 127: return "GetMemorySize";
	case 130: return "InitTLB";
	case 252: return "SetAlarm";
	case 253: return "iSetAlarm";
	case 254: return "ReleaseAlarm";
	case 255: return "iReleaseAlarm";
	default: return "?";
	}
}

uint32_t g_osd_config = (1u << 13) | (1u << 16); // version 1, English, 4:3, tz 0

} // namespace

} // namespace vc::kernel

using namespace vc;
using namespace vc::kernel;

extern "C" void ps2_syscall(ps2_cpu* c, uint32_t pc, uint32_t code)
{
	(void)code;
	const int32_t n = c->r[3].sw[0];
	if (g_log_syscalls || getenv("VC_LOG_SYSCALLS")) {
		g_log_syscalls = true;
		Log("sys", "[t%d] %08x %s(%d) a0=%08x a1=%08x a2=%08x a3=%08x", CurrentThreadId(), pc, Name(n), n, A(c, 0),
			A(c, 1), A(c, 2), A(c, 3));
	}

	const bool icall = n < 0 && !InInterrupt();
	if (icall)
		BeginIContext();

	switch (n) {
	// --- video ---------------------------------------------------------------------------
	case 2: // SetGsCrt(interlace, mode, field)
		Log("sys", "SetGsCrt(%u, %u, %u)", A(c, 0), A(c, 1), A(c, 2));
		vc::gs::SetGsCrt(A(c, 0) != 0, A(c, 1), A(c, 2) != 0);
		Ret(c, 0);
		break;
	case 112:
	case -112: // GsGetIMR
		Ret(c, static_cast<int64_t>(vc::gs::GsPrivRead64(0x12001010u)));
		break;
	case 113:
	case -113: // GsPutIMR
		vc::gs::GsPrivWrite64(0x12001010u, c->r[4].ud[0]);
		Ret(c, 0);
		break;
	case 115: // SetVSyncFlag(flag*, csr*)
		SetVSyncFlag(A(c, 0), A(c, 1));
		Ret(c, 0);
		break;

	// --- program -------------------------------------------------------------------------
	case 4: // Exit
		Log("sys", "Exit(%d) from %08x", S(c, 0), pc);
		ExitProgram(S(c, 0));
	case 6: // LoadExecPS2(path, argc, argv)
		Log("sys", "LoadExecPS2(\"%s\") from %08x: not supported", LoadCString(A(c, 0)).c_str(), pc);
		ExitProgram(0);
	case 123: // ExecOSD
		Log("sys", "ExecOSD from %08x", pc);
		ExitProgram(0);
	case 60: // SetupThread(gp, stack, stack_size, args, root)
		Ret(c, static_cast<int32_t>(SetupThread(c, A(c, 0), A(c, 1), A(c, 2), A(c, 3), c->r[8].uw[0])));
		break;
	case 61: // SetupHeap(start, size)
		Ret(c, static_cast<int32_t>(SetupHeap(A(c, 0), A(c, 1))));
		break;
	case 62: // EndOfHeap
		Ret(c, static_cast<int32_t>(EndOfHeap()));
		break;
	case 127: // GetMemorySize
		Ret(c, PS2_RAM_SIZE);
		break;

	// --- interrupts ----------------------------------------------------------------------
	case 16: // AddIntcHandler(cause, handler, next[, arg])
		Ret32(c, AddIntcHandler(S(c, 0), A(c, 1), S(c, 2), A(c, 3), c->r[28].uw[0]));
		break;
	case 17:
		Ret32(c, RemoveIntcHandler(S(c, 0), S(c, 1)));
		break;
	case 18: // AddDmacHandler(channel, handler, next[, arg])
		Ret32(c, AddDmacHandler(S(c, 0), A(c, 1), S(c, 2), A(c, 3), c->r[28].uw[0]));
		break;
	case 19:
		Ret32(c, RemoveDmacHandler(S(c, 0), S(c, 1)));
		break;
	case 20:
	case -26:
		Ret32(c, EnableIntc(S(c, 0)));
		break;
	case 21:
	case -27:
		Ret32(c, DisableIntc(S(c, 0)));
		break;
	case 22:
	case -28:
		Ret32(c, EnableDmac(S(c, 0)));
		break;
	case 23:
	case -29:
		Ret32(c, DisableDmac(S(c, 0)));
		break;
	case 92:
	case -92:
		Ret32(c, EnableIntcHandler(S(c, 0), S(c, 1), true));
		break;
	case 93:
	case -93:
		Ret32(c, EnableIntcHandler(S(c, 0), S(c, 1), false));
		break;
	case 94:
	case -94:
		Ret32(c, EnableDmacHandler(S(c, 0), S(c, 1), true));
		break;
	case 95:
	case -95:
		Ret32(c, EnableDmacHandler(S(c, 0), S(c, 1), false));
		break;
	case 252: // SetAlarm(hsyncs, handler, arg)
	case 253:
		Ret32(c, SetAlarm(A(c, 0), A(c, 1), A(c, 2), c->r[28].uw[0]));
		break;
	case 254:
	case -254:
	case 255:
	case -255: // ReleaseAlarm / iReleaseAlarm
		Ret32(c, ReleaseAlarm(S(c, 0)));
		break;

	// --- threads -------------------------------------------------------------------------
	case 32:
		Ret32(c, CreateThread(c, A(c, 0)));
		break;
	case 33:
		Ret32(c, DeleteThread(S(c, 0)));
		break;
	case 34:
		Ret32(c, StartThread(S(c, 0), A(c, 1)));
		break;
	case 35:
		ExitThread();
		break;
	case 36:
		ExitDeleteThread();
		break;
	case 37:
	case -38:
		Ret32(c, TerminateThread(S(c, 0)));
		break;
	case 41:
	case -42:
		Ret32(c, ChangeThreadPriority(S(c, 0), S(c, 1)));
		break;
	case 43:
	case -44:
		Ret32(c, RotateThreadReadyQueue(S(c, 0)));
		break;
	case 45:
	case -46:
		Ret32(c, ReleaseWaitThread(S(c, 0)));
		break;
	case 47:
	case -47:
		Ret32(c, CurrentThreadId());
		break;
	case 48:
	case -49:
		Ret32(c, ReferThreadStatus(S(c, 0), A(c, 1)));
		break;
	case 50:
		Ret32(c, SleepThread());
		break;
	case 51:
	case -52:
		Ret32(c, WakeupThread(S(c, 0)));
		break;
	case 53:
	case -54:
		Ret32(c, CancelWakeupThread(S(c, 0)));
		break;
	case 55:
	case -56:
		Ret32(c, SuspendThread(S(c, 0)));
		break;
	case 57:
	case -58:
		Ret32(c, ResumeThread(S(c, 0)));
		break;
	case 39:
	case 40: // Disable/EnableDispatchThread
		Ret(c, 0);
		break;

	// --- semaphores ----------------------------------------------------------------------
	case 64:
		Ret32(c, CreateSema(A(c, 0)));
		break;
	case 65:
		Ret32(c, DeleteSema(S(c, 0)));
		break;
	case 66:
	case -67:
		Ret32(c, SignalSema(S(c, 0)));
		break;
	case 68:
		Ret32(c, WaitSema(S(c, 0)));
		break;
	case 69:
	case -70:
		Ret32(c, PollSema(S(c, 0)));
		break;
	case 71:
	case -72:
		Ret32(c, ReferSemaStatus(S(c, 0), A(c, 1)));
		break;

	// --- configuration -------------------------------------------------------------------
	case 74: // SetOsdConfigParam(config*)
		g_osd_config = Load32(A(c, 0));
		Ret(c, 0);
		break;
	case 75: // GetOsdConfigParam(config*)
		Store32(A(c, 0), g_osd_config);
		Ret(c, 0);
		break;
	case 110: // SetOsdConfigParam2
		Ret(c, 0);
		break;
	case 111: // GetOsdConfigParam2(buf, size, offset): date/time format etc. -> defaults
		if (uint8_t* p = EePtr(A(c, 0), A(c, 1)))
			std::memset(p, 0, A(c, 1));
		Ret(c, 0);
		break;

	// --- caches / TLB / kernel patching (all no-ops for a recompiled program) -------------
	case 100:
	case -104: // FlushCache
	case 13:   // SetVTLBRefillHandler
	case 130:  // InitTLB
	case 0x54:
	case 0x55:
	case 0x56: // libkernl TLB helper syscalls
		Ret(c, 0);
		break;
	case 116: // SetSyscall(n, handler)
		g_user_syscalls[S(c, 0)] = A(c, 1);
		Log("sys", "SetSyscall(0x%x, %08x) recorded (handled natively)", A(c, 0), A(c, 1));
		Ret(c, 0);
		break;
	case 0x5A: // libkernl kCopy (word copy into kernel memory)
	{
		const uint32_t dst = A(c, 0), src = A(c, 1), len = A(c, 2) & ~3u;
		uint8_t* d = EePtr(dst, len);
		const uint8_t* s = EePtr(src, len);
		if (d && s)
			std::memmove(d, s, len);
		Ret(c, 0);
		break;
	}
	case 0x5B:
	case 0x83:
		Ret(c, 0);
		break;
	case 124: // Deci2Call: no debugger attached
		Ret(c, -1);
		break;

	// --- SIF -----------------------------------------------------------------------------
	case 107:
		vc_iop_sif_stop_dma(c);
		Ret(c, 0);
		break;
	case 118:
	case -118:
		Ret32(c, vc_iop_sif_dma_stat(c, A(c, 0)));
		break;
	case 119:
	case -119:
		Ret(c, static_cast<int32_t>(vc_iop_sif_set_dma(c, A(c, 0), A(c, 1))));
		break;
	case 120:
	case -120:
		vc_iop_sif_set_dchain(c);
		Ret(c, 0);
		break;
	case 121:
		Ret(c, static_cast<int32_t>(vc_iop_sif_set_reg(c, A(c, 0), A(c, 1))));
		break;
	case 122:
		Ret(c, static_cast<int32_t>(vc_iop_sif_get_reg(c, A(c, 0))));
		break;

	default: {
		static std::map<int32_t, int> seen;
		if (seen[n]++ == 0)
			Log("sys", "unknown syscall %d (0x%x) at %08x a0=%08x a1=%08x", n, static_cast<uint32_t>(n), pc,
				A(c, 0), A(c, 1));
		Ret(c, 0);
		break;
	}
	}

	if (icall) {
		EndIContext();
		return; // the deferred reschedule happens at the next delivery point
	}
	// Interrupts are taken on the way out of the kernel.
	if (!InInterrupt())
		Poll();
}
