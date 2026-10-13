# runtime/gs — GS, VU1, VIF1, GIF

`vc_gs.h` is the runtime's only view of the PS2 graphics hardware. Behind it sits PCSX2's
GS (hardware renderer on Metal, or its software renderer), VU0/VU1 interpreters, VIF
command processing with the NEON unpack dynarec, the GIF unit (PATH1/2/3 arbitration) and
the DMAC channel logic, imported unmodified into `third_party/pcsx2` (see
`third_party/README.md`). The header has no PCSX2 types, so a native RenderWare→Metal
renderer can replace the backend later without touching callers.

```
 recompiled EE ──► ps2_mem_*_slow ──► vc::gs::HwWrite32 / GsPrivWrite64   (register level)
                └► HLE'd libgraph/libdma/RenderWare ──► SubmitPath2/3, StartVU1, ...
                                         │
     ┌───────────── pcsx2_host/ ─────────┴──────────────────────────────────────────┐
     │ HwRegs.cpp   EE register dispatch (GIF, VIF0/1, FIFOs, DMAC, INTC)           │
     │ EECore.cpp   eeHw/eeMem/VU memory, event scheduler, INTC/DMAC lines → runtime │
     │ SyncMTGS.cpp GIF unit → GS renderer, synchronous (replaces PCSX2's MTGS)      │
     │ ImGuiHost / HostStubs   frontend services PCSX2 expects (mostly no-ops)       │
     └───────────────────────────────────────────────────────────────────────────────┘
          │ Dmac.cpp, Vif1_Dma.cpp, Vif_Codes.cpp, Gif.cpp, Gif_Unit.cpp, VU*.cpp …
          ▼
     GSRendererHW + GSDeviceMTL (Metal)  ──►  CAMetalLayer on the host NSView
```

## Interface (`vc_gs.h`)

| Group | Functions | Notes |
|---|---|---|
| Lifetime | `Init(Config)`, `Shutdown()`, `GetMemory()` | `Config` carries the `NSView*`, drawable size/scale, renderer (Metal / Software / Null), upscale, vsync, and the interrupt callbacks. |
| Memory | `Memory{ee_ram, scratchpad, vu0_micro, vu0_mem, vu1_micro, vu1_mem}` | Owned by vc_gs; hand these to `ps2_cpu::ram/spr/vu0_mem` so DMA, VIF and VU see the bytes the EE writes. |
| Register level | `HwRead32/64/128`, `HwWrite32/64/128` | GIF `0x10003000`, VIF0/1 `0x10003800/0x10003C00`, FIFOs `0x10004000–0x10006FFF`, DMAC `0x10008000–0x1000EFFF`, `INTC_STAT/INTC_MASK`, `D_ENABLER/W`. Return `false` for addresses vc_gs doesn't own (timers, IPU, SIF, ...). Writing `Dn_CHCR` with STR runs the transfer to completion before returning. |
| GS privileged | `GsPrivRead32/64`, `GsPrivWrite32/64` | `0x12000000–0x12001FFF`: PMODE, SMODE1/2, DISPFB1/2, DISPLAY1/2, BGCOLOR, CSR (FINISH/SIGNAL/VSINT acknowledge, FIELD), IMR, BUSDIR, SIGLBLID. |
| HLE paths | `SubmitPath3(data, qwc)` | GIF packets into the GIF unit as GIF DMA would deliver them. |
| | `SubmitPath2(data, qwc)` | A VIF1 command stream (UNPACK, MPG, MSCAL/MSCNT, STCYCL, DIRECT/DIRECTHL, ...), fed through the VIF1 FIFO. DIRECT is PATH2; MSCAL runs VU1, whose XGKICKs are PATH1. |
| | `UploadVU1Micro`, `UploadVU1Data`, `StartVU1(pc)`, `VU1Busy()` | Direct VU1 access, equivalent to MPG/UNPACK/MSCAL or `ctc2 CMSAR1`. |
| | `RunEvents()` | Drains pending DMA/VIF/GIF/VU events (called internally after every write). |
| Video | `SetGsCrt(interlaced, mode, frame_mode)` | HLE of syscall 2: sets SMODE1/SMODE2/SYNCV like the BIOS. |
| | `VSync()` | Start of vblank: latches display registers, flushes and presents, swaps CSR.FIELD, raises VSINT (via IMR → INTC bit 0 → `int0`). The runtime still raises INTC VBLANK_S/E itself. |
| | `ResizeSurface(w, h, scale)` | Host window resized. |
| Readback | `ReadLocalMemory(bitbltbuf, trxpos, trxreg, out, qwc)` | GS→host transfer (TRXDIR=1); syncs the HW renderer's texture cache first. |
| | `CaptureDisplay(&w, &h, &rgba)` | The last displayed image after the PCRTC merge, at internal resolution. |

Callbacks (`Config::callbacks`, all invoked synchronously from inside the vc_gs call that
caused them):

- `int0(user)`: `INTC_STAT & INTC_MASK` became non-zero (GS interrupt, etc.).
- `int1(user)`: DMAC `CIS & CIM` or `BEIS` is set.
- `dma_start(user, channel)`: the game started a channel vc_gs doesn't implement (3/4 IPU,
  5/6/7 SIF, 8/9 SPR). Its registers can be read with `HwRead32`.

## Execution model

- **Synchronous.** PCSX2 runs the GS on its own thread (MTGS) and paces DMA by EE cycle
  counts. Here there is no EE cycle counter, so `pcsx2_host/EECore.cpp` reimplements
  `CPU_INT` as a priority queue on a virtual cycle count: `RunEvents()` fires pending events
  in due order and jumps time forward. A DMA kick, VIF stream or `StartVU1` has finished
  (VU1 included, `vu1Instant`) by the time the call returns, and GIF packets have been
  drawn. `VSync()` also advances the virtual clock by one field.
- **Threading.** Make every call from one thread (the "GS thread", normally the EE thread).
  When it isn't the main thread, the main thread must be running its run loop:
  `Init` and the Metal device attach the `CAMetalLayer` with `dispatch_sync` to the main
  queue.
- **FP environment.** Recompiled EE code runs with round-toward-zero (`ps2_abi.h`). Each
  vc_gs entry point that can reach the GS switches to the default FPCR and restores it on
  return; the VU interpreters set their own mode.
- **Metal shaders** are compiled at `Init` from source embedded in the binary (about 0.7 s
  on an M1). The Command Line Tools have no offline `metal` compiler; see
  `third_party/patches/pcsx2/0001-*`.

## PCSX2 on arm64 macOS: what runs

| Component | arm64 status | Used here |
|---|---|---|
| GS hardware renderer + Metal device | Native, Metal 2.3 shaders (ROV, framebuffer fetch on Apple GPUs) | yes (default) |
| GS software renderer | Native, JIT via vixl (`GS*CodeGenerator.arm64.cpp`) | yes (`Renderer::Software`) |
| VU0/VU1 | microVU is x86-only (`pcsx2/x86/microVU*`); arm64 uses the interpreters. `REC_VU1`/`THREAD_VU1` are compile-time false off x86, so there's no MTVU | VU1 interpreter, instant mode |
| VIF unpack | NEON dynarec (`pcsx2/arm64/Vif_Dynarec.cpp`, `Vif_UnpackNEON.cpp`) | yes |
| GIF unit, PATH1/2/3, DMAC | Portable C++ | yes |
| EE/IOP recompilers | x86-only (arm64 upstream uses interpreters) | not imported; the game is statically recompiled |

## What is stubbed (`pcsx2_host/HostStubs.cpp`)

OSD and fullscreen UI (the ImGui context exists but draws nothing), video capture
(FFmpeg), GS dump replay, texture replacement packs (libzip), save states, settings files,
memory-card, pad and USB config plumbing referenced by `Pcsx2Config.cpp`, the EE
interpreter's COP2 tables (macro mode is recompiled), timers/SPU2/SIF/IPU resets, and
SPR/SIF/IPU DMA (forwarded to `dma_start`).

## Tests

`runtime/tests/gs_smoke` opens a window and draws through every path: PATH3 HLE (clear),
PATH3 through GIF DMA, PATH2 through VIF1 DIRECT, and PATH1 through VIF1 chain DMA (MPG
microcode + UNPACK + MSCAL → VU1 XGKICK). It then checks pixels in both the displayed
image and GS local memory, and checks that VSINT and DMAC interrupts reach the callbacks.

```
cmake -S runtime -B build/runtime -G Ninja && cmake --build build/runtime
ctest --test-dir build/runtime            # gs_smoke (window), gs_smoke_headless, gs_smoke_sw
build/runtime/tests/gs_smoke --keep-open  # watch it; --dump=prefix writes PPMs
VC_GS_TRACE=1 build/runtime/tests/gs_smoke --headless   # log every GS packet
```

## Known limits / next steps

- No timing model: DMA and VU1 are instantaneous, and PATH3 masking/IMT interleaving only
  happens when the game drives it explicitly.
- `SetGsCrt` writes only SMODE1/SMODE2/SYNCV (not SYNCH1/2, SRFSH); the renderer only needs
  these.
- SPR (scratchpad) DMA isn't implemented yet. If RenderWare uses toSPR/fromSPR, import
  `pcsx2/SPR.cpp`.
- VU0 micro mode (`vcallms`, ps2_abi `ps2_vu0_callms`) can reuse the imported VU0
  interpreter. It needs `ps2_cpu::vf/vi` copied to and from `VU0.VF/VI`; the layouts match.
