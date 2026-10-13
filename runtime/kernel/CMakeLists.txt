# SPDX-License-Identifier: GPL-3.0-or-later
#
# vc_kernel: EE kernel HLE (threads/fibers, interrupts, syscalls, COP0/VU0 hooks, indirect
# dispatch, HLE overrides) and the EE memory map (runtime/mem). OBJECT library so that its
# strong ps2f_XXXXXXXX overrides always replace the recompiler's weak definitions, and so
# that runtime/iop's strong vc_iop_* functions replace the weak defaults in iop_weak.c.

enable_language(ASM)

add_library(vc_kernel OBJECT
	kernel.h
	fiber.h
	fiber.cpp
	fiber_arm64.S
	log.h
	log.cpp
	threads.cpp
	intr.cpp
	syscalls.cpp
	cop.cpp
	dispatch.cpp
	hle.cpp
	spr_dma.cpp
	iop_weak.c
	../mem/mem.cpp
	../include/ps2_abi.h
	../include/vc_iop.h)
target_include_directories(vc_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../include"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/include")
target_compile_features(vc_kernel PUBLIC cxx_std_20)
target_link_libraries(vc_kernel PUBLIC vc_gs PRIVATE vc_warnings)

