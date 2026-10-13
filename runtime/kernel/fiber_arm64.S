// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal arm64 (Apple ABI) fiber switch for EE threads. See fiber.h.
//
// vc_fiber_ctx layout (bytes):
//   0  x19 x20   16 x21 x22   32 x23 x24   48 x25 x26   64 x27 x28
//   80 x29 x30   96 sp       104 d8 d9    120 d10 d11  136 d12 d13  152 d14 d15
//   168 fpcr

	.text
	.p2align 2

// void vc_fiber_switch(vc_fiber_ctx *from, const vc_fiber_ctx *to)
	.globl _vc_fiber_switch
_vc_fiber_switch:
	stp x19, x20, [x0, #0]
	stp x21, x22, [x0, #16]
	stp x23, x24, [x0, #32]
	stp x25, x26, [x0, #48]
	stp x27, x28, [x0, #64]
	stp x29, x30, [x0, #80]
	mov x9, sp
	str x9, [x0, #96]
	stp d8, d9, [x0, #104]
	stp d10, d11, [x0, #120]
	stp d12, d13, [x0, #136]
	stp d14, d15, [x0, #152]
	mrs x9, fpcr
	str x9, [x0, #168]

	ldp x19, x20, [x1, #0]
	ldp x21, x22, [x1, #16]
	ldp x23, x24, [x1, #32]
	ldp x25, x26, [x1, #48]
	ldp x27, x28, [x1, #64]
	ldp x29, x30, [x1, #80]
	ldr x9, [x1, #96]
	mov sp, x9
	ldp d8, d9, [x1, #104]
	ldp d10, d11, [x1, #120]
	ldp d12, d13, [x1, #136]
	ldp d14, d15, [x1, #152]
	ldr x9, [x1, #168]
	msr fpcr, x9
	ret

// First code a new fiber runs: x19 = entry(void *), x20 = argument. The entry must not
// return; trap if it does.
	.globl _vc_fiber_trampoline
_vc_fiber_trampoline:
	mov x29, #0
	mov x0, x20
	blr x19
	brk #0xf1
