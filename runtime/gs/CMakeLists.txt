# SPDX-License-Identifier: GPL-3.0-or-later
#
# vc_gs: the runtime's GS/VU1/VIF/GIF interface (vc_gs.h) on top of the PCSX2 import.

add_library(vc_gs STATIC
	vc_gs.h
	vc_gs.cpp
	pcsx2_host/pcsx2_host.h
	pcsx2_host/EECore.cpp
	pcsx2_host/HostStubs.cpp
	pcsx2_host/HwRegs.cpp
	pcsx2_host/ImGuiHost.cpp
	pcsx2_host/SyncMTGS.cpp)
target_include_directories(vc_gs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vc_gs PUBLIC pcsx2_core PRIVATE vc_warnings)
# PCSX2 headers are included as system headers (third_party/CMakeLists.txt), so our warning
# set applies only to our own code.
