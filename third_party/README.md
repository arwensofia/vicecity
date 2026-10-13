# third_party

Imported third-party source. Everything here is built by `third_party/CMakeLists.txt`,
which `runtime/CMakeLists.txt` includes.

## PCSX2 (`pcsx2/`)

| | |
|---|---|
| Upstream | https://github.com/PCSX2/pcsx2 |
| Commit | `0f71e668b8adc950db7de1b1b15c2f5080c102e9` (2026-10-12, "GS/HW: Favour full screen shuffles for depth mismatches"), also in `pcsx2/UPSTREAM_COMMIT` |
| License | GPL-3.0-or-later (`pcsx2/COPYING.GPLv3`); per-file SPDX headers kept intact |
| Files | 435, listed in `pcsx2_files.txt` (13 MB) |

### What was imported, and why

| Directory | Files | Contents |
|---|---:|---|
| `pcsx2/pcsx2/GS/` | 102 | GS core (`GSState`, local memory, CLUT, drawing environment, dumps), the renderer-independent layer, the HW renderer and texture cache, the SW renderer with its arm64 JIT, the Null renderer, the Metal device and its `.metal` shaders. DX11/12, OpenGL and Vulkan are left out. |
| `pcsx2/pcsx2/` (top level + header-only subdirs: CDVD, Host, IPU, ImGui, Input, SIO, SPU2, USB, ps2) | 74 | VU0/VU1 interpreters (`VUops`, `VUflags`, `VU*micro*`), VIF (`Vif*.cpp`), GIF unit (`Gif*.cpp`), EE-side GS privileged registers (`GS.cpp`), DMAC (`Dmac.cpp`, `Hw.cpp`), FIFOs (`FiFo.cpp`), `Pcsx2Config.cpp`, `SourceLog.cpp`, plus the headers these include |
| `pcsx2/pcsx2/arm64/` | 5 | NEON VIF unpack and its dynarec, vixl helpers |
| `pcsx2/pcsx2/DebugTools/` | 5 | VU micro disassemblers (used by debug-build VU logging) and headers |
| `pcsx2/common/` | 67 | PCSX2's support library (strings, files, console, threads, host memory, Cocoa helpers) |
| `pcsx2/3rdparty/` | 178 | fmt, fast_float, Dear ImGui, cpuinfo, LZMA SDK, vixl, each with its LICENSE |
| `pcsx2/bin/resources/shaders/common/` | 3 | `fxaa.fx`, `ffx_a.h`, `ffx_cas.h`, included by the Metal shaders |

Not imported: the EE/IOP cores and recompilers, microVU (x86 only), SPU2, CDVD, SIO, USB,
DEV9, IPU, VMManager, MTGS, the Qt frontend and other GS backends.
`runtime/gs/pcsx2_host/` replaces what the imported code needs from those (see
`runtime/gs/README.md`).

Bundled-library licences: fmt (MIT), fast_float (Apache-2.0 / MIT / BSL-1.0), Dear ImGui
(MIT), cpuinfo (BSD-2-Clause), LZMA SDK (public domain), vixl (BSD-3-Clause). All are
GPL-3.0 compatible.

External build dependencies, from Homebrew: libpng, jpeg-turbo, libwebp, zstd (used by
`common/Image.cpp` and GS dumps). zlib and the Metal/QuartzCore/AppKit frameworks come
from the SDK.

### Local patches (`patches/pcsx2/`)

| Patch | File | Why |
|---|---|---|
| `0001-metal-runtime-shader-library.patch` | `pcsx2/GS/Renderers/Metal/GSMTLDeviceInfo.mm` | When no prebuilt `.metallib` is found, compile the shader source embedded at build time (`scripts/amalgamate_metal.py`) with `newLibraryWithSource:` (MSL 2.3). The Command Line Tools ship no `metal` compiler. Guarded by `VC_METAL_RUNTIME_SHADERS`. |
| `0002-fastjmp-arm64-alignment.patch` | `common/FastJmp.cpp` | `.align 16` means 2^16 bytes to Apple's assembler, which made ld warn about reducing `__TEXT` alignment; use `.p2align 4` (the intended 16 bytes). |

Everything else is byte-identical to upstream.

### Updating

```
git clone https://github.com/PCSX2/pcsx2 third_party/_upstream/pcsx2   # git-ignored
git -C third_party/_upstream/pcsx2 checkout <new commit>
third_party/scripts/import_pcsx2.sh            # copies pcsx2_files.txt, applies patches
cmake --build build/runtime                    # fix fallout in runtime/gs/pcsx2_host
third_party/scripts/update_manifest.sh build/runtime   # re-derive the file list from compiler deps
```

- `scripts/import_pcsx2.sh [manifest]`: wipes `pcsx2/`, copies the manifest's files from
  `_upstream/pcsx2`, applies `patches/pcsx2/*.patch`, writes `UPSTREAM_COMMIT`.
- `scripts/update_manifest.sh <build dir>`: rebuilds `pcsx2_files.txt` from Ninja's
  dependency log and `compile_commands.json`, plus the Metal shaders, their includes and the
  licence files. To add a file, copy it in, add it to the CMake lists, build, then run this.
- `scripts/make_patch.sh <name>.patch <path>...`: regenerates a patch from edits in
  `pcsx2/`, relative to the upstream checkout.
- `scripts/amalgamate_metal.py`: build-time helper that inlines the `.metal` files and their
  includes into a C++ string literal.
- `config/vc_imconfig.h`: Dear ImGui config (PCSX2's, without FreeType).
