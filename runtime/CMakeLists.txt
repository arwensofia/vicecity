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

add_subdirectory(../third_party "${CMAKE_BINARY_DIR}/third_party")
add_subdirectory(gs)

include(CTest)
if(BUILD_TESTING)
	add_subdirectory(tests)
endif()
