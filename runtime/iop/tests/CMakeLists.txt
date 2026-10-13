# SPDX-License-Identifier: GPL-3.0-or-later

foreach(t iop_iso_test iop_mcfs_test iop_rpc_test)
	add_executable(${t} ${t}.cpp test_support.cpp test_support.h)
	target_link_libraries(${t} PRIVATE vc_iop vc_warnings)
	add_test(NAME ${t} COMMAND ${t})
endforeach()
# Disc tests read the user's image from $VC_ISO and skip (exit code 77) when it is not set.
add_test(NAME iop_disc_test COMMAND iop_iso_test --disc)
set_tests_properties(iop_disc_test PROPERTIES SKIP_RETURN_CODE 77)
