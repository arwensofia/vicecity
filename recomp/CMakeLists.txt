# SPDX-License-Identifier: GPL-3.0-or-later
#
# ps2recomp: R5900 (EE) -> C static recompiler, and the vc_game library built
# from its output (cmake/vc_game.cmake).
#   cmake -S recomp -B build/recomp -G Ninja && cmake --build build/recomp
#   ctest --test-dir build/recomp --output-on-failure
#   cmake --build build/recomp --target vc_game     # regenerate recomp_out + compile
# As a subproject: add_subdirectory(<repo>/recomp <bindir>/recomp), then link vc_game.
cmake_minimum_required(VERSION 3.24)
project(ps2recomp LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PS2_ABI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include)
set(PS2_OPS_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(recomp_core STATIC
	src/decoder.cpp
	src/image.cpp
	src/translator.cpp
	src/emit_insn.cpp)
target_include_directories(recomp_core PUBLIC src)
target_compile_options(recomp_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(ps2recomp src/main.cpp)
target_link_libraries(ps2recomp PRIVATE recomp_core)
target_compile_options(ps2recomp PRIVATE -Wall -Wextra -Wpedantic)

# Flags every consumer of recompiled C must use (see ps2_ops.h).
set(PS2_GENERATED_C_FLAGS -ffp-contract=off -fno-math-errno -fno-strict-aliasing
	-Wno-unused-label -Wno-unused-function -Wno-pedantic)

option(PS2RECOMP_BUILD_TESTS "Build the recompiler semantic tests" ${PROJECT_IS_TOP_LEVEL})
if(PROJECT_IS_TOP_LEVEL)
	include(CTest)
endif()
if(PS2RECOMP_BUILD_TESTS AND (BUILD_TESTING OR NOT DEFINED BUILD_TESTING))
	enable_testing()
	# Stage 1: translate every test snippet in tests/cases.inc with the real
	# emitter. Stage 2: compile the generated C with the runner and execute it.
	add_executable(recomp_test_gen tests/test_gen.cpp)
	target_link_libraries(recomp_test_gen PRIVATE recomp_core)
	target_include_directories(recomp_test_gen PRIVATE tests)

	set(GEN_C ${CMAKE_CURRENT_BINARY_DIR}/tests_generated.c)
	set(GEN_H ${CMAKE_CURRENT_BINARY_DIR}/tests_generated.h)
	add_custom_command(
		OUTPUT ${GEN_C} ${GEN_H}
		COMMAND recomp_test_gen ${GEN_C} ${GEN_H}
		DEPENDS recomp_test_gen tests/cases.inc
		COMMENT "Translating recompiler test snippets")

	add_executable(recomp_tests tests/test_runner.cpp tests/test_runtime.cpp ${GEN_C})
	target_include_directories(recomp_tests PRIVATE tests ${CMAKE_CURRENT_BINARY_DIR}
		${PS2_ABI_INCLUDE} ${PS2_OPS_INCLUDE})
	set_source_files_properties(${GEN_C} PROPERTIES COMPILE_OPTIONS
		"-O2;-Wall;-Werror=implicit-function-declaration;${PS2_GENERATED_C_FLAGS}")
	target_compile_options(recomp_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
	add_test(NAME recomp_tests COMMAND recomp_tests)

	# Same snippets compiled with every trace/debug point enabled, linked with the
	# support library (trace ring, symbols, default dispatch).
	set(GEN_C_TRACE ${CMAKE_CURRENT_BINARY_DIR}/tests_generated_trace.c)
	add_custom_command(OUTPUT ${GEN_C_TRACE}
		COMMAND ${CMAKE_COMMAND} -E copy ${GEN_C} ${GEN_C_TRACE}
		DEPENDS ${GEN_C})
	add_executable(recomp_trace_tests tests/test_trace.cpp tests/test_runtime.cpp ${GEN_C_TRACE}
		support/ps2_recomp_support.c)
	target_include_directories(recomp_trace_tests PRIVATE tests ${CMAKE_CURRENT_BINARY_DIR}
		${PS2_ABI_INCLUDE} ${PS2_OPS_INCLUDE})
	set_source_files_properties(${GEN_C_TRACE} PROPERTIES COMPILE_OPTIONS
		"-O2;-Wall;-Werror=implicit-function-declaration;-DPS2_TRACE=3;-DPS2_TRACK_PC=2;-DPS2_BACKEDGE_HOOK=1;${PS2_GENERATED_C_FLAGS}")
	target_compile_options(recomp_trace_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>
		$<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)
	add_test(NAME recomp_trace_tests COMMAND recomp_trace_tests)
endif()

include(cmake/vc_game.cmake)
