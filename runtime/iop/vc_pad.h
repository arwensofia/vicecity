/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * vc_pad.h -- controller input for the emulated padman (runtime/iop). The game sees one
 * DualShock 2 in port 1 (index 0); port 2 is empty unless the app feeds it.
 *
 * Three sources are merged every vblank (buttons OR'ed, the stick with the largest
 * deflection wins, pressure = max):
 *   1. vc_pad_set_state()  -- anything the app wants to inject (tests, remapping UI, ...)
 *   2. vc_pad_key()        -- keyboard, with the default map below
 *   3. GameController.framework -- the first extended gamepad, when enabled
 * All functions may be called from any thread.
 *
 * Default keyboard map (macOS virtual key codes, port 0):
 *   W A S D    left stick          I J K L   right stick       arrows   D-pad
 *   Space      Cross               Q         Circle            F        Square
 *   E          Triangle            Z / X     L1 / R1           C / V    L2 / R2
 *   R / T      L3 / R3             Return    Start             Tab      Select
 */
#ifndef VC_PAD_H
#define VC_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Button bits, 1 = pressed (the PS2 wire format is the inverse of this, little-endian). */
enum {
	VC_PAD_SELECT = 0x0001,
	VC_PAD_L3 = 0x0002,
	VC_PAD_R3 = 0x0004,
	VC_PAD_START = 0x0008,
	VC_PAD_UP = 0x0010,
	VC_PAD_RIGHT = 0x0020,
	VC_PAD_DOWN = 0x0040,
	VC_PAD_LEFT = 0x0080,
	VC_PAD_L2 = 0x0100,
	VC_PAD_R2 = 0x0200,
	VC_PAD_L1 = 0x0400,
	VC_PAD_R1 = 0x0800,
	VC_PAD_TRIANGLE = 0x1000,
	VC_PAD_CIRCLE = 0x2000,
	VC_PAD_CROSS = 0x4000,
	VC_PAD_SQUARE = 0x8000,
};

typedef struct vc_pad_state {
	uint16_t buttons;     /* VC_PAD_* */
	uint8_t lx, ly;       /* left stick, 0..255, 0x80 centre, y: 0 = up */
	uint8_t rx, ry;       /* right stick */
	uint8_t pressure[12]; /* optional analog pressure: right, left, up, down, triangle, circle,
	                         cross, square, L1, R1, L2, R2; 0 = derive from buttons */
} vc_pad_state;

/* Source 1. connected = 0 removes the injected state. */
void vc_pad_set_state(int port, const vc_pad_state *state, int connected);
/* Source 2: key down/up with a macOS virtual key code (NSEvent.keyCode). */
void vc_pad_key(unsigned keycode, int down);
/* Source 3: poll GameController.framework on vblank (default on). */
void vc_pad_enable_gamecontroller(int enable);
/* Current vibration request from the game: small motor on/off (0/1), large motor 0..255. */
void vc_pad_get_rumble(int port, uint8_t *small_motor, uint8_t *large_motor);

#ifdef __cplusplus
}
#endif

#endif /* VC_PAD_H */
