# Plan

## Target

| | |
|---|---|
| Executable | `SLUS_205.52`, MIPS R5900 ELF, loaded at `0x00100000`, 3.7 MB |
| Symbols | None (`.symtab` present but empty) |
| Engine | RenderWare 3.3 (`RW33Active`), PS2 `sky2` driver |
| Disc layout | `MODELS/` 1.8 GB, `AUDIO/` 2.3 GB, `ANIM/`, `TXD/`, `DATA/`, `MOVIES/`, `TEXT/` |

## Phases

1. **Map the executable** — Ghidra project, function list, naming. *(in progress)*
2. **Recompile** — translate all EE code to C that builds for arm64.
3. **Runtime** — replace PS2 hardware: GS → Metal, VU1 microcode, VU0 macro ops,
   IOP/SIF (CD streaming, pad, memory card), SPU2 → CoreAudio, pad → GameController.
4. **Boot** — iterate on crashes until title screen, then gameplay.
5. **Assets** — read PS2-native RenderWare streams directly from the disc at runtime.
6. *(Later, optional)* **Native renderer** — replace RenderWare's PS2 pipelines and the
   PCSX2 GS/VU1 layer with a Metal renderer at the atomic/world-sector level.

## Agents

Project subagents in `.claude/agents/`: `ghidra-analyst` (static analysis),
`recompiler-engineer` (`recomp/`), `runtime-engineer` (`runtime/`, `third_party/`),
`asset-tools` (`assets/`). Shared rules in `CLAUDE.md`.

## Status

### Phase 1 — first pass (2026-10-13)

- Ghidra 12.1.4 headless analysis with stock `MIPS:LE:64:64-32addr`: **4,478 functions**.
- 144 decode errors where analysis hit R5900-only instructions (MMI 128-bit ops,
  VU0 macro mode / COP2). Code after these is not followed, so the real function count
  is likely much higher.
- Debug strings reference 47 source files, only 7 with function names. Automatic
  naming so far: 1 function renamed (`Population::UpdatePedCount` @ `0x001d36e0`),
  8 tagged with their source file. Many string references aren't resolved yet.

### Phase 1 — R5900 processor (2026-10-13)

Switched to the `r5900:LE:32:default` language from
[ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) v2.1.38.

| | Stock MIPS | R5900 |
|---|---|---|
| Decode errors | 144 | 0 |
| Functions | 4,478 | 5,345 |
| Code inside functions | 0.61 MB | 3.06 MB (of 3.7 MB) |
| Named from debug strings | 1 | 7 |
| Tagged with source file | 8 | 20 |

Named: `Explosion::AddExplosion` `0x147350`, `ProjectileInfo::AddProjectile` `0x149410`,
`Ped::SetWanderPath` `0x1ac930`, `Population::UpdatePedCount` `0x1d36e0`,
`Population::AddPed` `0x1d4bc0`, `Crime::RegisterCrime` `0x29d8d0`,
`EventList::RegisterEvent` `0x2c2340`.

### Next

- [x] Add an R5900 (Emotion Engine) processor module to Ghidra.
- [ ] Resolve `$gp`-relative and `lui`/`addiu` string refs to improve naming yield.
- [x] Identify statically linked libraries and the hardware boundary — see
      [MODULES.md](MODULES.md): 82 functions touch hardware, 65 syscalls, RenderWare is
      ~12% of code, game ~83%.
- [x] Decide graphics strategy (2026-10-13): reuse PCSX2's GS (Metal) + VU1 now; replacing
      RenderWare's PS2 pipelines with a native Metal renderer is a possible later phase.
      Project relicensed GPL-3.0 accordingly.
- [x] Identify the remaining string-less libraries (2026-10-13) — `libmc`, `libcdvd`, `libipu`,
      `libscf`, `libdev` (not libvu0), libgcc, newlib libm, a DTS Interactive encoder, RenderWare
      toolkit code, libkernl kernel patches and static constructors; compiler-fingerprint rule
      + `tools/module_overrides.csv`; 0 functions left unknown. See [MODULES.md](MODULES.md).
- [ ] Pick or write the recompiler (MIPS R5900 → C).

### Phase 3 — GS/VU1 runtime layer (2026-10-13)

- PCSX2 (`0f71e668`) GS (HW renderer on Metal, SW renderer), VU0/VU1 interpreters, VIF
  (NEON unpack dynarec), GIF unit and DMAC imported into `third_party/pcsx2` (435 files,
  2 local patches; see `third_party/README.md`), built as static libraries with CMake.
- `runtime/gs/vc_gs.h`: narrow interface (register-level EE/GS access, PATH1/2/3 HLE
  submission, VU1 upload/start, vsync/present, readback), synchronous execution, PS2 memory
  owned by vc_gs. Design: `runtime/gs/README.md`.
- `runtime/tests/gs_smoke` draws through PATH3 (HLE and GIF DMA), PATH2 (VIF1 DIRECT) and
  PATH1 (VIF1 DMA → MPG/UNPACK/MSCAL → VU1 XGKICK) and verifies the displayed image, GS memory
  and VSINT/DMAC interrupts. Passes on Metal (windowed and headless) and on the SW renderer.
