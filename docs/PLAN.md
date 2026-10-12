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
- [ ] Identify statically linked libraries (Sony SDK `libgraph`/`libdma`/`libpad`/
      `libmc`/`libsd`, RenderWare) by signature — these become the runtime boundary.
- [ ] Pick or write the recompiler (MIPS R5900 → C).
