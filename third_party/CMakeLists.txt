# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the PCSX2 subset imported into third_party/pcsx2 (see third_party/README.md) as
# static libraries for macOS arm64:
#
#   pcsx2_fmt, pcsx2_imgui, pcsx2_cpuinfo, pcsx2_lzma, pcsx2_vixl   bundled third-party libs
#   pcsx2_common      PCSX2 common/ support library
#   pcsx2_core        GS (core, HW + SW + Null renderers, Metal device), VU0/VU1 interpreters,
#                     VIF0/1 command processing + unpack (NEON dynarec), GIF unit, EE-side GS
#                     privileged registers, Pcsx2Config
#
# PCSX2 subsystems these expect but we do not import (VMManager, MTGS thread, Host UI,
# ImGuiManager, EE/IOP cores, DMAC, scheduler, ...) are provided by runtime/gs/pcsx2_host/,
# so pcsx2_core must always be linked together with vc_gs.
#
# Upstream sources are compiled unmodified except for third_party/patches/pcsx2/*.patch.

cmake_minimum_required(VERSION 3.24)

set(PCSX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pcsx2")
set(PCSX2_3RD "${PCSX2_DIR}/3rdparty")

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
	message(FATAL_ERROR "third_party/pcsx2 is configured for macOS arm64 only")
endif()

enable_language(OBJCXX)

find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
if(TARGET zstd::libzstd_static)
	set(PCSX2_ZSTD zstd::libzstd_static)
else()
	set(PCSX2_ZSTD zstd::libzstd_shared)
endif()

# Flags PCSX2 applies to all of its own code (cmake/BuildParameters.cmake, pcsx2/CMakeLists.txt).
# pcsx2_flags is what consumers of the PCSX2 headers need (definitions, include paths as
# system headers so our zero-warning build is not affected, target CPU). pcsx2_private holds
# the flags only PCSX2's own sources are compiled with.
add_library(pcsx2_flags INTERFACE)
target_compile_features(pcsx2_flags INTERFACE cxx_std_20)
target_compile_definitions(pcsx2_flags INTERFACE
	__POSIX__
	FMT_USE_EXCEPTIONS=0
	FMT_USE_RTTI=0
	"$<$<CONFIG:Debug>:PCSX2_DEVBUILD;PCSX2_DEBUG;_DEBUG>"
)
target_compile_options(pcsx2_flags INTERFACE
	-march=armv8.4-a -mcpu=apple-m1
	-fvisibility=hidden -pthread
	-fno-strict-aliasing
)
target_include_directories(pcsx2_flags SYSTEM INTERFACE
	"${PCSX2_DIR}"
	"${PCSX2_DIR}/pcsx2"
	"${PCSX2_3RD}/include"
)

add_library(pcsx2_private INTERFACE)
target_compile_options(pcsx2_private INTERFACE
	-ffp-contract=fast
	"$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fno-exceptions>"
	# Upstream code: not held to our zero-warning rule; keep the build log readable.
	-w
)

# --- fmt ---------------------------------------------------------------------------------
add_library(pcsx2_fmt STATIC
	"${PCSX2_3RD}/fmt/src/format.cc"
	"${PCSX2_3RD}/fmt/src/os.cc")
target_include_directories(pcsx2_fmt SYSTEM PUBLIC "${PCSX2_3RD}/fmt/include")
target_compile_definitions(pcsx2_fmt PUBLIC FMT_USE_EXCEPTIONS=0 FMT_USE_RTTI=0)
target_compile_options(pcsx2_fmt PRIVATE -w)

# --- fast_float (header only) ------------------------------------------------------------
add_library(pcsx2_fast_float INTERFACE)
target_include_directories(pcsx2_fast_float SYSTEM INTERFACE "${PCSX2_3RD}/fast_float/include")

# --- Dear ImGui (PCSX2's copy; built without FreeType, see config/vc_imconfig.h) ---------
add_library(pcsx2_imgui STATIC
	"${PCSX2_3RD}/imgui/src/imgui.cpp"
	"${PCSX2_3RD}/imgui/src/imgui_draw.cpp"
	"${PCSX2_3RD}/imgui/src/imgui_tables.cpp"
	"${PCSX2_3RD}/imgui/src/imgui_widgets.cpp"
	"${PCSX2_3RD}/imgui/src/imgui_stdlib.cpp")
target_include_directories(pcsx2_imgui SYSTEM PUBLIC
	"${PCSX2_3RD}/imgui/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/config")
target_compile_definitions(pcsx2_imgui PUBLIC IMGUI_USER_CONFIG="vc_imconfig.h")
target_compile_features(pcsx2_imgui PRIVATE cxx_std_17)
target_compile_options(pcsx2_imgui PRIVATE -w)

# --- cpuinfo (macOS arm64 sources only) --------------------------------------------------
add_library(pcsx2_cpuinfo STATIC
	"${PCSX2_3RD}/cpuinfo/src/api.c"
	"${PCSX2_3RD}/cpuinfo/src/cache.c"
	"${PCSX2_3RD}/cpuinfo/src/init.c"
	"${PCSX2_3RD}/cpuinfo/src/log.c"
	"${PCSX2_3RD}/cpuinfo/src/arm/uarch.c"
	"${PCSX2_3RD}/cpuinfo/src/arm/cache.c"
	"${PCSX2_3RD}/cpuinfo/src/arm/mach/init.c"
	"${PCSX2_3RD}/cpuinfo/src/mach/topology.c")
target_include_directories(pcsx2_cpuinfo PUBLIC "${PCSX2_3RD}/cpuinfo/include")
target_include_directories(pcsx2_cpuinfo PRIVATE "${PCSX2_3RD}/cpuinfo/src" "${PCSX2_3RD}/cpuinfo/deps/clog/include")
target_compile_definitions(pcsx2_cpuinfo PRIVATE CPUINFO_LOG_LEVEL=2 CPUINFO_LOG_TO_STDIO=1)
target_compile_options(pcsx2_cpuinfo PRIVATE -w)

# --- LZMA SDK (xz GS dumps) --------------------------------------------------------------
file(GLOB PCSX2_LZMA_SRCS CONFIGURE_DEPENDS "${PCSX2_3RD}/lzma/src/*.c")
add_library(pcsx2_lzma STATIC ${PCSX2_LZMA_SRCS})
target_include_directories(pcsx2_lzma PUBLIC "${PCSX2_3RD}/lzma/include")
target_compile_definitions(pcsx2_lzma PRIVATE _7ZIP_ST)
target_compile_options(pcsx2_lzma PRIVATE -w)

# --- vixl (AArch64 assembler for the SW renderer's JIT and the VIF unpack dynarec) -------
add_library(pcsx2_vixl STATIC
	"${PCSX2_3RD}/vixl/src/aarch64/assembler-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/assembler-sve-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/cpu-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/cpu-features-auditor-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/decoder-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/disasm-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/instructions-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/logic-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/macro-assembler-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/macro-assembler-sve-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/operands-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/pointer-auth-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/aarch64/registers-aarch64.cc"
	"${PCSX2_3RD}/vixl/src/code-buffer-vixl.cc"
	"${PCSX2_3RD}/vixl/src/compiler-intrinsics-vixl.cc"
	"${PCSX2_3RD}/vixl/src/cpu-features.cc"
	"${PCSX2_3RD}/vixl/src/utils-vixl.cc")
target_include_directories(pcsx2_vixl PUBLIC "${PCSX2_3RD}/vixl/include")
target_include_directories(pcsx2_vixl PRIVATE
	"${PCSX2_3RD}/vixl/include/vixl"
	"${PCSX2_3RD}/vixl/include/vixl/aarch64")
target_compile_definitions(pcsx2_vixl PUBLIC VIXL_INCLUDE_TARGET_A64)
target_compile_definitions(pcsx2_vixl PRIVATE VIXL_CODE_BUFFER_MALLOC)
target_compile_features(pcsx2_vixl PRIVATE cxx_std_17)
target_compile_options(pcsx2_vixl PRIVATE -w)

# --- common/ -----------------------------------------------------------------------------
set(C "${PCSX2_DIR}/common")
add_library(pcsx2_common STATIC
	${C}/AlignedMalloc.cpp ${C}/Assertions.cpp ${C}/Console.cpp ${C}/DynamicLibrary.cpp
	${C}/Error.cpp ${C}/FastJmp.cpp ${C}/FileSystem.cpp ${C}/HostSys.cpp ${C}/Image.cpp
	${C}/MD5Digest.cpp ${C}/MemorySettingsInterface.cpp ${C}/Perf.cpp ${C}/ProgressCallback.cpp
	${C}/ReadbackSpinManager.cpp ${C}/Semaphore.cpp ${C}/SettingsWrapper.cpp ${C}/SmallString.cpp
	${C}/StringUtil.cpp ${C}/TextureDecompress.cpp ${C}/Timer.cpp ${C}/WindowInfo.cpp
	${C}/CocoaTools.mm ${C}/Darwin/DarwinThreads.cpp ${C}/Darwin/DarwinMisc.cpp
	${C}/Linux/LnxHostSys.cpp)
target_link_libraries(pcsx2_common PUBLIC pcsx2_flags pcsx2_fmt pcsx2_fast_float PRIVATE pcsx2_private)
target_link_libraries(pcsx2_common PRIVATE pcsx2_cpuinfo PNG::PNG JPEG::JPEG WebP::webp
	"-framework Foundation" "-framework IOKit" "-framework AppKit")
set_source_files_properties(${C}/CocoaTools.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# --- core: GS + VU + VIF + GIF -----------------------------------------------------------
set(P "${PCSX2_DIR}/pcsx2")
set(G "${P}/GS")
set(PCSX2_GS_SOURCES
	# GS core
	${G}/GS.cpp ${G}/GSClut.cpp ${G}/GSDrawingContext.cpp ${G}/GSDrawingEnvironment.cpp
	${G}/GSDump.cpp ${G}/GSLocalMemory.cpp ${G}/GSLzma.cpp ${G}/GSPerfMon.cpp ${G}/GSPng.cpp
	${G}/GSRingHeap.cpp ${G}/GSState.cpp ${G}/GSTables.cpp ${G}/GSUtil.cpp ${G}/GSVector.cpp
	${G}/MultiISA.cpp ${G}/GSBlock.cpp ${G}/GSLocalMemoryMultiISA.cpp ${G}/GSXXH.cpp
	# renderer-independent
	${G}/Renderers/Common/GSDevice.cpp ${G}/Renderers/Common/GSDirtyRect.cpp
	${G}/Renderers/Common/GSFunctionMap.cpp ${G}/Renderers/Common/GSRenderer.cpp
	${G}/Renderers/Common/GSTexture.cpp ${G}/Renderers/Common/GSVertexTrace.cpp
	${G}/Renderers/Common/GSVertexTraceFMM.cpp
	# null / hardware / software (texture replacement packs are not imported; stubbed in
	# runtime/gs/pcsx2_host since they need libzip)
	${G}/Renderers/Null/GSRendererNull.cpp
	${G}/Renderers/HW/GSHwHack.cpp ${G}/Renderers/HW/GSRendererHW.cpp
	${G}/Renderers/HW/GSRendererHWMultiISA.cpp ${G}/Renderers/HW/GSTextureCache.cpp
	${G}/Renderers/SW/GSTextureCacheSW.cpp ${G}/Renderers/SW/GSDrawScanline.cpp
	${G}/Renderers/SW/GSRasterizer.cpp ${G}/Renderers/SW/GSRendererSW.cpp
	${G}/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.cpp
	${G}/Renderers/SW/GSSetupPrimCodeGenerator.arm64.cpp
	# Metal
	${G}/Renderers/Metal/GSDeviceMTL.mm ${G}/Renderers/Metal/GSMTLDeviceInfo.mm
	${G}/Renderers/Metal/GSTextureMTL.mm)
set(PCSX2_EE_SOURCES
	# EE-side GS privileged registers, GIF unit (PATH1/2/3 arbitration), GIF DMA, FIFOs
	${P}/GS.cpp ${P}/Gif.cpp ${P}/Gif_Unit.cpp ${P}/FiFo.cpp
	# DMAC channel/control registers and chain-tag helpers, INTC/DMAC status
	${P}/Dmac.cpp ${P}/Hw.cpp
	# trace log objects referenced by the logging macros
	${P}/SourceLog.cpp
	# VU0/VU1 interpreters
	${P}/VUops.cpp ${P}/VUflags.cpp ${P}/VUmicro.cpp ${P}/VUmicroMem.cpp
	${P}/VU0.cpp ${P}/VU0micro.cpp ${P}/VU0microInterp.cpp
	${P}/VU1micro.cpp ${P}/VU1microInterp.cpp ${P}/MTVU.cpp
	# VIF command processing, unpack, VIF1 DMA
	${P}/Vif.cpp ${P}/Vif0_Dma.cpp ${P}/Vif1_Dma.cpp ${P}/Vif1_MFIFO.cpp ${P}/Vif_Codes.cpp
	${P}/Vif_Transfer.cpp ${P}/Vif_Unpack.cpp
	${P}/arm64/Vif_UnpackNEON.cpp ${P}/arm64/Vif_Dynarec.cpp ${P}/arm64/AsmHelpers.cpp
	# VU microcode disassemblers (VU interpreters log through them in debug builds)
	${P}/DebugTools/DisVU0Micro.cpp ${P}/DebugTools/DisVU1Micro.cpp
	# configuration structures
	${P}/Pcsx2Config.cpp)

add_library(pcsx2_core STATIC ${PCSX2_GS_SOURCES} ${PCSX2_EE_SOURCES})
target_link_libraries(pcsx2_core PUBLIC pcsx2_common pcsx2_imgui PRIVATE pcsx2_private)
target_link_libraries(pcsx2_core PRIVATE pcsx2_cpuinfo pcsx2_lzma pcsx2_vixl PNG::PNG ZLIB::ZLIB
	${PCSX2_ZSTD}
	"-framework Metal" "-framework QuartzCore" "-framework AppKit" "-framework Foundation")
set_source_files_properties(
	${G}/Renderers/Metal/GSDeviceMTL.mm ${G}/Renderers/Metal/GSMTLDeviceInfo.mm
	${G}/Renderers/Metal/GSTextureMTL.mm
	PROPERTIES COMPILE_OPTIONS "-fno-objc-arc;-fobjc-weak")

# Metal shaders: the Command Line Tools have no offline `metal` compiler, so the .metal
# sources are amalgamated into a C string at build time and compiled by the Metal framework
# at runtime (patches/pcsx2/0001-metal-runtime-shader-library.patch).
set(MTL "${G}/Renderers/Metal")
set(PCSX2_METAL_SHADERS ${MTL}/tfx.metal ${MTL}/convert.metal ${MTL}/present.metal
	${MTL}/merge.metal ${MTL}/misc.metal ${MTL}/interlace.metal ${MTL}/cas.metal ${MTL}/fxaa.metal)
set(PCSX2_METAL_AMALGAM "${CMAKE_CURRENT_BINARY_DIR}/generated/pcsx2_metal_shaders.inc")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${PCSX2_METAL_AMALGAM}"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
	COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/amalgamate_metal.py"
		"${PCSX2_METAL_AMALGAM}" ${PCSX2_METAL_SHADERS}
	DEPENDS ${PCSX2_METAL_SHADERS} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/amalgamate_metal.py"
		${MTL}/GSMTLShaderCommon.h ${MTL}/GSMTLSharedHeader.h
	COMMENT "Amalgamating PCSX2 Metal shaders")
set_source_files_properties(${MTL}/GSMTLDeviceInfo.mm PROPERTIES OBJECT_DEPENDS "${PCSX2_METAL_AMALGAM}")
target_sources(pcsx2_core PRIVATE "${PCSX2_METAL_AMALGAM}")
target_include_directories(pcsx2_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(pcsx2_core PRIVATE VC_METAL_RUNTIME_SHADERS)
