# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game executable: recompiled EE code (recomp's vc_game target; derived from the game,
# generated from the user's local copy, never committed) + runtime.

if(NOT TARGET vc_game)
	message(WARNING "vc_game is not available (game/SLUS_205.52 or out/functions.csv missing); "
		"the vicecity target is skipped. Run scripts/setup.sh <iso> first.")
	return()
endif()

add_executable(vicecity main.mm elf_loader.h elf_loader.cpp)
set_source_files_properties(main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(vicecity PRIVATE vc_kernel vc_iop vc_game vc_gs vc_warnings
	"-framework AppKit" "-framework Metal" "-framework QuartzCore")
