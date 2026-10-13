# SPDX-License-Identifier: GPL-3.0-or-later
#
# vc_game: the recompiled game as a static library.
#
# Included by recomp/CMakeLists.txt. Consumers (runtime/CMakeLists.txt):
#     add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../recomp ${CMAKE_BINARY_DIR}/recomp)
#     if(TARGET vc_game)
#         target_link_libraries(vicecity PRIVATE vc_game)
#     endif()
#
# At build time ps2recomp regenerates ${VC_GAME_OUT_DIR} whenever the ELF, the
# function list, the config, the name/module tables or ps2recomp itself change.
# Generated files are only rewritten when their content changes, so only the
# affected translation units recompile (Ninja restat).
#
# The output is derived from the game executable: it lives in the git-ignored
# recomp_out/ (default) and must never be committed.

get_filename_component(_vc_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(VC_GAME_ELF "${_vc_root}/game/SLUS_205.52" CACHE FILEPATH "Game executable (SLUS_205.52)")
set(VC_GAME_FUNCTIONS "${_vc_root}/out/functions.csv" CACHE FILEPATH "Function list (scripts/setup.sh)")
set(VC_GAME_CONFIG "${_vc_root}/recomp/config/SLUS_205.52.cfg" CACHE FILEPATH "ps2recomp config")
set(VC_GAME_NAMES "${_vc_root}/tools/library_names.csv" CACHE FILEPATH "Library function names (optional)")
set(VC_GAME_MODULES "${_vc_root}/out/modules.csv" CACHE FILEPATH "Module map (optional, scripts/classify.sh)")
set(VC_GAME_OUT_DIR "${_vc_root}/recomp_out" CACHE PATH "Generated C output directory (git-ignored)")
set(VC_GAME_FILES 64 CACHE STRING "Number of generated ps2_funcs_NNN.c translation units")
set(VC_GAME_TRACE 0 CACHE STRING "Trace hooks in recompiled code: 0 off, 1 function entry, 2 basic blocks, 3 both")
set(VC_GAME_TRACK_PC 0 CACHE STRING "c->pc tracking: 0 off, 1 per basic block, 2 also per load/store")
option(VC_GAME_BACKEDGE "Loop back-edge hook (ps2_backedge) for interrupt delivery in busy-wait loops" ON)
set(VC_GAME_OPT "-O2" CACHE STRING "Optimisation flag for recompiled code")
option(VC_GAME_DEBUG_INFO "Line tables for recompiled code (-gline-tables-only): host pc -> generated line (shows the EE address)" ON)
option(VC_GAME_LINK_ALIASES "Link ee_<name> alias symbols for named EE functions (ld64 -alias_list)" ${APPLE})
option(VC_GAME_CCACHE "Use ccache/sccache for recompiled code if found" ON)

set_property(CACHE VC_GAME_TRACE PROPERTY STRINGS 0 1 2 3)
set_property(CACHE VC_GAME_TRACK_PC PROPERTY STRINGS 0 1 2)

if(NOT EXISTS "${VC_GAME_ELF}" OR NOT EXISTS "${VC_GAME_FUNCTIONS}")
	message(STATUS "vc_game: ${VC_GAME_ELF} or ${VC_GAME_FUNCTIONS} missing -- target not defined "
		"(run scripts/setup.sh <iso>)")
	return()
endif()

set(_out "${VC_GAME_OUT_DIR}")
set(_gen_c)
math(EXPR _last "${VC_GAME_FILES} - 1")
foreach(_i RANGE 0 ${_last})
	string(LENGTH "${_i}" _len)
	set(_n "${_i}")
	if(_len EQUAL 1)
		set(_n "00${_i}")
	elseif(_len EQUAL 2)
		set(_n "0${_i}")
	endif()
	list(APPEND _gen_c "${_out}/ps2_funcs_${_n}.c")
endforeach()
list(APPEND _gen_c "${_out}/ps2_dispatch.c" "${_out}/ps2_names.c")
set(_gen_other "${_out}/ps2_funcs.h" "${_out}/ps2_names.h" "${_out}/ps2_aliases.txt" "${_out}/recomp_report.txt")

set(_args --elf "${VC_GAME_ELF}" --functions "${VC_GAME_FUNCTIONS}" --out "${_out}"
	--config "${VC_GAME_CONFIG}" --files "${VC_GAME_FILES}" --quiet)
set(_deps ps2recomp "${VC_GAME_ELF}" "${VC_GAME_FUNCTIONS}" "${VC_GAME_CONFIG}")
if(EXISTS "${VC_GAME_NAMES}")
	list(APPEND _args --names "${VC_GAME_NAMES}")
	list(APPEND _deps "${VC_GAME_NAMES}")
endif()
if(EXISTS "${VC_GAME_MODULES}")
	list(APPEND _args --modules "${VC_GAME_MODULES}")
	list(APPEND _deps "${VC_GAME_MODULES}")
endif()

set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/vc_game_generate.stamp")
add_custom_command(
	OUTPUT "${_stamp}"
	BYPRODUCTS ${_gen_c} ${_gen_other}
	COMMAND ps2recomp ${_args}
	COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
	DEPENDS ${_deps}
	COMMENT "ps2recomp: regenerating ${_out}"
	VERBATIM)
add_custom_target(vc_game_generate DEPENDS "${_stamp}")

add_library(vc_game STATIC ${_gen_c} "${CMAKE_CURRENT_SOURCE_DIR}/support/ps2_recomp_support.c")
add_library(vc::game ALIAS vc_game)
add_dependencies(vc_game vc_game_generate)
target_include_directories(vc_game PUBLIC "${PS2_ABI_INCLUDE}" "${PS2_OPS_INCLUDE}" "${_out}")
set_target_properties(vc_game PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_definitions(vc_game
	PRIVATE PS2_TRACE=${VC_GAME_TRACE} PS2_TRACK_PC=${VC_GAME_TRACK_PC} PS2_BACKEDGE_HOOK=$<BOOL:${VC_GAME_BACKEDGE}>
	INTERFACE VC_GAME_TRACE=${VC_GAME_TRACE} VC_GAME_TRACK_PC=${VC_GAME_TRACK_PC} VC_GAME_BACKEDGE=$<BOOL:${VC_GAME_BACKEDGE}>)
# Options last so they win over CMAKE_C_FLAGS_<CONFIG> (-O0/-g in Debug).
set(_vc_game_opts ${VC_GAME_OPT} ${PS2_GENERATED_C_FLAGS} -Wno-unused-variable -Wno-unused-but-set-variable)
if(VC_GAME_DEBUG_INFO)
	list(APPEND _vc_game_opts -gline-tables-only)
else()
	list(APPEND _vc_game_opts -g0)
endif()
target_compile_options(vc_game PRIVATE ${_vc_game_opts})

if(VC_GAME_CCACHE)
	find_program(VC_CCACHE_PROGRAM NAMES ccache sccache)
	if(VC_CCACHE_PROGRAM)
		set_target_properties(vc_game PROPERTIES C_COMPILER_LAUNCHER "${VC_CCACHE_PROGRAM}")
	endif()
endif()

if(VC_GAME_LINK_ALIASES AND APPLE)
	target_link_options(vc_game INTERFACE "LINKER:-alias_list,${_out}/ps2_aliases.txt")
	set_property(TARGET vc_game APPEND PROPERTY INTERFACE_LINK_DEPENDS "${_out}/ps2_aliases.txt")
endif()

message(STATUS "vc_game: ${VC_GAME_FILES} units in ${_out}, trace=${VC_GAME_TRACE} track_pc=${VC_GAME_TRACK_PC} backedge=${VC_GAME_BACKEDGE}")
