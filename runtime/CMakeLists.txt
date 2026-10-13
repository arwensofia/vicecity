# SPDX-License-Identifier: GPL-3.0-or-later
#
# vicecity runtime. Standalone for now:
#   cmake -S runtime -B build/runtime -G Ninja && cmake --build build/runtime
cmake_minimum_required(VERSION 3.24)
project(vicecity_runtime LANGUAGES C CXX OBJC OBJCXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning set for our own code (zero warnings policy).
add_library(vc_warnings INTERFACE)
target_compile_options(vc_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror=return-type)

include(CTest)

add_subdirectory(../third_party "${CMAKE_BINARY_DIR}/third_party")
add_subdirectory(gs)
add_subdirectory(kernel)
add_subdirectory(iop)

# Recompiled game (recomp/cmake/vc_game.cmake). Generated C goes to the build tree by default so
# runtime builds don't race with recompiler work in recomp_out/.
set(VC_GAME_OUT_DIR "${CMAKE_BINARY_DIR}/recomp_out" CACHE PATH "Generated C output directory (git-ignored)")
# c->pc per basic block during boot bring-up: crash reports and logs name the EE address.
set(VC_GAME_TRACK_PC 1 CACHE STRING "c->pc tracking: 0 off, 1 per basic block, 2 also per load/store")
add_subdirectory(../recomp "${CMAKE_BINARY_DIR}/recomp")

add_subdirectory(app)

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()
