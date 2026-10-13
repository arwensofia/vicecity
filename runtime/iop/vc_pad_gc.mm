// SPDX-License-Identifier: GPL-3.0-or-later
//
// GameController.framework source for the emulated DualShock 2 (see vc_pad.h).

#import <GameController/GameController.h>

#include <cmath>

#include "vc_pad.h"

namespace vc::iop {

namespace {

uint8_t Axis(float v, bool invert)
{
	if (invert)
		v = -v;
	float f = (v + 1.0f) * 127.5f;
	return uint8_t(std::fmax(0.0f, std::fmin(255.0f, std::round(f))));
}

uint8_t Pressure(GCControllerButtonInput* b)
{
	return b ? uint8_t(std::fmin(255.0f, std::round(b.value * 255.0f))) : 0;
}

} // namespace

bool PollGameController(vc_pad_state* out)
{
	@autoreleasepool
	{
		GCController* pad = nil;
		for (GCController* c in [GCController controllers])
		{
			if (c.extendedGamepad)
			{
				pad = c;
				break;
			}
		}
		if (!pad)
			return false;
		GCExtendedGamepad* g = [pad.extendedGamepad capture];
		vc_pad_state s{};
		auto btn = [&](GCControllerButtonInput* b, uint16_t bit) {
			if (b && b.pressed)
				s.buttons |= bit;
		};
		btn(g.buttonA, VC_PAD_CROSS);
		btn(g.buttonB, VC_PAD_CIRCLE);
		btn(g.buttonX, VC_PAD_SQUARE);
		btn(g.buttonY, VC_PAD_TRIANGLE);
		btn(g.leftShoulder, VC_PAD_L1);
		btn(g.rightShoulder, VC_PAD_R1);
		btn(g.leftTrigger, VC_PAD_L2);
		btn(g.rightTrigger, VC_PAD_R2);
		btn(g.leftThumbstickButton, VC_PAD_L3);
		btn(g.rightThumbstickButton, VC_PAD_R3);
		btn(g.buttonMenu, VC_PAD_START);
		btn(g.buttonOptions, VC_PAD_SELECT);
		btn(g.dpad.up, VC_PAD_UP);
		btn(g.dpad.down, VC_PAD_DOWN);
		btn(g.dpad.left, VC_PAD_LEFT);
		btn(g.dpad.right, VC_PAD_RIGHT);
		s.lx = Axis(g.leftThumbstick.xAxis.value, false);
		s.ly = Axis(g.leftThumbstick.yAxis.value, true); // PS2: 0 = up
		s.rx = Axis(g.rightThumbstick.xAxis.value, false);
		s.ry = Axis(g.rightThumbstick.yAxis.value, true);
		s.pressure[0] = Pressure(g.dpad.right);
		s.pressure[1] = Pressure(g.dpad.left);
		s.pressure[2] = Pressure(g.dpad.up);
		s.pressure[3] = Pressure(g.dpad.down);
		s.pressure[4] = Pressure(g.buttonY);
		s.pressure[5] = Pressure(g.buttonB);
		s.pressure[6] = Pressure(g.buttonA);
		s.pressure[7] = Pressure(g.buttonX);
		s.pressure[8] = Pressure(g.leftShoulder);
		s.pressure[9] = Pressure(g.rightShoulder);
		s.pressure[10] = Pressure(g.leftTrigger);
		s.pressure[11] = Pressure(g.rightTrigger);
		*out = s;
		return true;
	}
}

} // namespace vc::iop
