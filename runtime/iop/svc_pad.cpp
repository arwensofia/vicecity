// SPDX-License-Identifier: GPL-3.0-or-later
//
// padman (0x80000100/0x80000101) as libpad 4.x (SDK 2.4) uses it. All commands go through
// fno 1 with the command number in the first word of the 128-byte buffer; results are
// written back into the same buffer. Pad data reaches the EE the way padman's SIF DMA
// delivers it: two 128-byte frames in the 64-byte aligned pad area passed to
// scePadPortOpen, the newer one having the larger frame counter (+0x58). PadVblank()
// writes a frame for every open port once per vblank.

#include <algorithm>
#include <mutex>

#include "iop.h"
#include "vc_pad.h"

namespace vc::iop {

// GameController.framework polling (vc_pad_gc.mm). Returns false if no gamepad.
bool PollGameController(vc_pad_state* out);

namespace {

// ---------------------------------------------------------------------------------------
// Input sources
// ---------------------------------------------------------------------------------------

struct Input
{
	std::mutex m;
	vc_pad_state injected[2] = {};
	bool injected_on[2] = {};
	uint32_t keys = 0;  // bitmask of pressed mapped keys (index into kKeyMap)
	bool gc_enabled = true;
	uint8_t rumble_small[2] = {};
	uint8_t rumble_large[2] = {};
};

Input& In()
{
	static Input in;
	return in;
}

enum KeyAction : uint8_t
{
	kButton,
	kLeftX,
	kLeftY,
	kRightX,
	kRightY,
};

struct KeyMap
{
	uint16_t keycode;
	KeyAction action;
	int value; // button bit, or -1/+1 for axes
};

const KeyMap kKeyMap[] = {
	{13, kLeftY, -1},  {1, kLeftY, +1},   {0, kLeftX, -1},   {2, kLeftX, +1},    // W S A D
	{34, kRightY, -1}, {40, kRightY, +1}, {38, kRightX, -1}, {37, kRightX, +1}, // I K J L
	{126, kButton, VC_PAD_UP},       {125, kButton, VC_PAD_DOWN},
	{123, kButton, VC_PAD_LEFT},     {124, kButton, VC_PAD_RIGHT},
	{49, kButton, VC_PAD_CROSS},     {12, kButton, VC_PAD_CIRCLE},
	{3, kButton, VC_PAD_SQUARE},     {14, kButton, VC_PAD_TRIANGLE},
	{6, kButton, VC_PAD_L1},         {7, kButton, VC_PAD_R1},
	{8, kButton, VC_PAD_L2},         {9, kButton, VC_PAD_R2},
	{15, kButton, VC_PAD_L3},        {17, kButton, VC_PAD_R3},
	{36, kButton, VC_PAD_START},     {48, kButton, VC_PAD_SELECT},
};
static_assert(sizeof(kKeyMap) / sizeof(kKeyMap[0]) <= 32, "key bitmask");

vc_pad_state Neutral()
{
	vc_pad_state s{};
	s.lx = s.ly = s.rx = s.ry = 0x80;
	return s;
}

uint8_t Stronger(uint8_t a, uint8_t b)
{
	return std::abs(int(a) - 0x80) >= std::abs(int(b) - 0x80) ? a : b;
}

void Merge(vc_pad_state& dst, const vc_pad_state& src)
{
	dst.buttons |= src.buttons;
	dst.lx = Stronger(dst.lx, src.lx);
	dst.ly = Stronger(dst.ly, src.ly);
	dst.rx = Stronger(dst.rx, src.rx);
	dst.ry = Stronger(dst.ry, src.ry);
	for (int i = 0; i < 12; i++)
		dst.pressure[i] = std::max(dst.pressure[i], src.pressure[i]);
}

// Returns the merged state; false if the port has no controller.
bool Gather(int port, vc_pad_state* out)
{
	Input& in = In();
	vc_pad_state s = Neutral();
	bool connected = port == 0;
	bool gc;
	{
		std::lock_guard<std::mutex> lock(in.m);
		if (in.injected_on[port])
		{
			Merge(s, in.injected[port]);
			connected = true;
		}
		if (port == 0)
		{
			int axis[4] = {};
			for (size_t i = 0; i < sizeof(kKeyMap) / sizeof(kKeyMap[0]); i++)
			{
				if (!(in.keys & (1u << i)))
					continue;
				const KeyMap& k = kKeyMap[i];
				if (k.action == kButton)
					s.buttons |= uint16_t(k.value);
				else
					axis[k.action - kLeftX] += k.value;
			}
			auto ax = [](int v) { return uint8_t(v < 0 ? 0x00 : v > 0 ? 0xFF : 0x80); };
			vc_pad_state k = Neutral();
			k.lx = ax(axis[0]);
			k.ly = ax(axis[1]);
			k.rx = ax(axis[2]);
			k.ry = ax(axis[3]);
			Merge(s, k);
		}
		gc = in.gc_enabled && port == 0;
	}
	vc_pad_state g;
	if (gc && PollGameController(&g))
		Merge(s, g);
	*out = s;
	return connected;
}

// ---------------------------------------------------------------------------------------
// padman
// ---------------------------------------------------------------------------------------

constexpr uint8_t kStateDisconnected = 0, kStateStable = 6;

struct Port
{
	bool open = false;
	uint32_t area = 0;     // EE pad area (2 x 128 bytes)
	uint32_t act_iop = 0;  // IOP buffer receiving scePadSetActDirect (2 x 32 bytes)
	bool analog = true;
	bool locked = false;
	bool pressure = false;
	uint32_t frame = 0;
};

Port g_ports[2];

void WriteFrame(int port)
{
	Port& p = g_ports[port];
	if (!p.open || !p.area)
		return;
	vc_pad_state s;
	bool connected = Gather(port, &s);

	uint8_t f[128] = {};
	uint8_t id = p.pressure ? 0x79 : p.analog ? 0x73 : 0x41;
	f[0] = 0;
	f[1] = id;
	uint16_t wire = uint16_t(~s.buttons);
	f[2] = uint8_t(wire);
	f[3] = uint8_t(wire >> 8);
	f[4] = s.rx;
	f[5] = s.ry;
	f[6] = s.lx;
	f[7] = s.ly;
	static const uint16_t kPressureBits[12] = {VC_PAD_RIGHT, VC_PAD_LEFT, VC_PAD_UP, VC_PAD_DOWN,
	                                           VC_PAD_TRIANGLE, VC_PAD_CIRCLE, VC_PAD_CROSS,
	                                           VC_PAD_SQUARE, VC_PAD_L1, VC_PAD_R1, VC_PAD_L2,
	                                           VC_PAD_R2};
	for (int i = 0; i < 12; i++)
		f[8 + i] = s.pressure[i] ? s.pressure[i] : (s.buttons & kPressureBits[i]) ? 0xFF : 0;
	if (!p.analog)
		std::fill(f + 4, f + 20, 0); // digital mode carries buttons only
	// actuator info (scePadInfoAct): {func, subfunc, size, current}
	const uint8_t act[8] = {0, 1, 1, 1, 0, 0, 8, 1};
	std::memcpy(f + 0x30, act, sizeof act);
	const uint16_t modes[4] = {4, 7, 0, 0}; // digital, analog (scePadInfoMode table)
	std::memcpy(f + 0x50, modes, sizeof modes);
	uint32_t frame = ++p.frame;
	uint32_t length = 32;
	std::memcpy(f + 0x58, &frame, 4);
	std::memcpy(f + 0x60, &length, 4);
	f[0x64] = 2;                       // mode configuration available
	f[0x65] = id;                      // current mode id
	f[0x66] = 3;                       // model: DualShock 2
	f[0x67] = 1;                       // button data ready
	f[0x68] = 2;                       // number of modes
	f[0x69] = p.analog ? 1 : 0;        // current mode table offset
	f[0x6a] = 2;                       // actuators
	f[0x6b] = 1;                       // actuator combinations
	f[0x6d] = p.analog ? 1 : 0;
	f[0x6e] = p.locked ? 3 : 0;
	f[0x70] = connected ? kStateStable : kStateDisconnected;
	f[0x71] = 0;                       // request state: complete
	f[0x72] = 1;                       // current task: reading
	f[0x73] = 1;
	f[0x79] = 0xFF; f[0x7a] = 0xFF; f[0x7b] = 0x03; f[0x7c] = 0x00; // button mask 0x3ffff
	// Overwrite the older of the two frames.
	uint32_t f0 = EeRead32(p.area + 0x58), f1 = EeRead32(p.area + 0x80 + 0x58);
	uint32_t dst = (f0 <= f1) ? p.area : p.area + 0x80;
	EeWrite(dst, f, sizeof f);
}

void ReadRumble(int port)
{
	Port& p = g_ports[port];
	if (!p.open || !p.act_iop)
		return;
	uint8_t* b = IopPtr(p.act_iop, 64);
	if (!b)
		return;
	uint32_t c0, c1;
	std::memcpy(&c0, b, 4);
	std::memcpy(&c1, b + 32, 4);
	const uint8_t* a = (c1 > c0 ? b + 32 : b) + 0x0c;
	std::lock_guard<std::mutex> lock(In().m);
	In().rumble_small[port] = a[0] ? 1 : 0;
	In().rumble_large[port] = a[1];
}

class Padman : public RpcServer
{
public:
	const char* Name() const override { return "PADMAN"; }
	void Call(RpcCall& call) override
	{
		uint32_t cmd = call.Arg(0);
		uint32_t port = call.Arg(1) & 1, slot = call.Arg(2);
		switch (cmd)
		{
		case 0x01: { // open(port, slot, ?, pad area)
			Port& p = g_ports[port];
			p.open = slot == 0;
			p.area = call.Arg(4);
			if (!p.act_iop)
				p.act_iop = IopAlloc(64);
			if (uint8_t* b = IopPtr(p.act_iop, 64))
				std::memset(b, 0, 64);
			Log("pad", "scePadPortOpen(%u, %u, area %08x)", port, slot, p.area);
			WriteFrame(int(port));
			call.Ret(3, p.open ? 1 : 0);
			call.Ret(5, p.act_iop);
			break;
		}
		case 0x06: // set main mode(port, slot, mode, lock)
			g_ports[port].analog = call.Arg(3) != 0;
			g_ports[port].locked = call.Arg(4) == 3;
			if (!g_ports[port].analog)
				g_ports[port].pressure = false;
			call.Ret(5, 1);
			break;
		case 0x08: // set actuator alignment
			call.Ret(5, 1);
			break;
		case 0x0a: // set button info (pressure mode)
			g_ports[port].pressure = g_ports[port].analog && call.Arg(3) != 0;
			call.Ret(4, 1);
			break;
		case 0x0b: // set vref
			call.Ret(7, 1);
			break;
		case 0x0c: // port max
			call.Ret(3, 2);
			break;
		case 0x0d: // slot max(port)
			call.Ret(3, 1);
			break;
		case 0x0e: // close(port, slot)
			g_ports[port].open = false;
			call.Ret(3, 1);
			break;
		case 0x0f: // end
		case 0x10: // init
			call.Ret(3, 1);
			break;
		case 0x12: // module version: libpad requires major 4
			call.Ret(3, 0x0402);
			break;
		case 0x14:
			call.Ret(2, 1);
			break;
		default:
			if (LogLimited("pad-cmd-" + std::to_string(cmd)))
				Log("pad", "padman command %x not emulated", cmd);
			call.Ret(3, 1);
			break;
		}
	}
	void Reset() override
	{
		for (Port& p : g_ports)
			p = Port();
	}
};

class PadmanExt : public RpcServer
{
public:
	const char* Name() const override { return "PADMAN (ext)"; }
	void Call(RpcCall& call) override
	{
		if (LogLimited("padext"))
			Log("pad", "padman ext call fno %x cmd %x ignored", call.fno, call.Arg(0));
	}
};

Padman g_padman;
PadmanExt g_padext;

} // namespace

void RegisterPad()
{
	RegisterServer(0x80000100, &g_padman);
	RegisterServer(0x80000101, &g_padext);
}

void PadVblank()
{
	for (int port = 0; port < 2; port++)
	{
		ReadRumble(port);
		WriteFrame(port);
	}
}

} // namespace vc::iop

using namespace vc::iop;

extern "C" {

void vc_pad_set_state(int port, const vc_pad_state* state, int connected)
{
	if (port < 0 || port > 1)
		return;
	std::lock_guard<std::mutex> lock(In().m);
	In().injected_on[port] = connected && state;
	if (state)
		In().injected[port] = *state;
}

void vc_pad_key(unsigned keycode, int down)
{
	std::lock_guard<std::mutex> lock(In().m);
	for (size_t i = 0; i < sizeof(kKeyMap) / sizeof(kKeyMap[0]); i++)
	{
		if (kKeyMap[i].keycode != keycode)
			continue;
		if (down)
			In().keys |= 1u << i;
		else
			In().keys &= ~(1u << i);
	}
}

void vc_pad_enable_gamecontroller(int enable)
{
	std::lock_guard<std::mutex> lock(In().m);
	In().gc_enabled = enable != 0;
}

void vc_pad_get_rumble(int port, uint8_t* small_motor, uint8_t* large_motor)
{
	if (port < 0 || port > 1)
		return;
	std::lock_guard<std::mutex> lock(In().m);
	if (small_motor)
		*small_motor = In().rumble_small[port];
	if (large_motor)
		*large_motor = In().rumble_large[port];
}

} // extern "C"
