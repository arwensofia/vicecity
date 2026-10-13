# SPDX-License-Identifier: GPL-3.0-or-later
#
# runtime/iop: emulated IOP services (SIF RPC HLE, CDVD/ISO, pad, memory card, sound stubs).
#
# Target `vc_iop` is an OBJECT library: its ps2f_XXXXXXXX overrides and vc_iop_* functions
# must replace weak definitions (recompiled code, runtime/kernel/iop_weak.c), which a static
# archive member would not do. Link it directly into the executable:
#     target_link_libraries(<app> PRIVATE vc_iop)
# It needs the KERNEL PROVIDES half of vc_iop.h (vc_ee_ptr, vc_log, ...) and
# ps2_call_indirect from the rest of the runtime.
#
# Standalone (tests only):
#     cmake -S runtime/iop -B build/runtime-iop -G Ninja && cmake --build build/runtime-iop
#     ctest --test-dir build/runtime-iop --output-on-failure      (VC_ISO=<image> for disc tests)
cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(vicecity_iop LANGUAGES C CXX OBJCXX)
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "")
	if(NOT CMAKE_BUILD_TYPE)
		set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
	endif()
	include(CTest)
endif()

if(NOT TARGET vc_warnings)
	add_library(vc_warnings INTERFACE)
	target_compile_options(vc_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror=return-type)
endif()

add_library(vc_iop OBJECT
	iop.h
	iop_core.cpp
	iso_fs.h
	iso_fs.cpp
	mc_fs.h
	mc_fs.cpp
	sif_hle.cpp
	svc_system.cpp
	svc_cdvd.cpp
	svc_pad.cpp
	svc_mc.cpp
	svc_game.cpp
	vc_pad.h
	vc_pad_gc.mm)
target_compile_features(vc_iop PUBLIC cxx_std_20)
target_include_directories(vc_iop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(vc_iop PRIVATE vc_warnings PUBLIC "-framework GameController" "-framework Foundation")
set_source_files_properties(vc_pad_gc.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()
