# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(gs_smoke gs_smoke.mm)
target_link_libraries(gs_smoke PRIVATE vc_gs vc_warnings "-framework AppKit")
set_source_files_properties(gs_smoke.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# Windowed run (needs a logged-in GUI session) and a headless run.
add_test(NAME gs_smoke COMMAND gs_smoke --frames=30)
add_test(NAME gs_smoke_headless COMMAND gs_smoke --headless --frames=10)
add_test(NAME gs_smoke_sw COMMAND gs_smoke --headless --renderer=sw --frames=10)
