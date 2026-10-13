# vicecity

Native macOS (Apple Silicon) port of GTA: Vice City PS2 (NTSC-U v1.40, `SLUS_205.52`) by
static recompilation. Roadmap and status: `docs/PLAN.md`. Module/hardware map: `docs/MODULES.md`.

## Hard rules

- **Never commit game data or anything derived from it**: the ISO, `SLUS_205.52`, extracted
  assets, Ghidra project, decompiled/recompiled output, string dumps. These live in
  git-ignored `game/`, `ghidra_project/`, `out/`, `recomp_out/`. Check `git status` before
  every commit.
- Docs may contain addresses, counts and function names, not copied game code or bulk strings.
- License is GPL-3.0 (required because we reuse PCSX2 code). Keep PCSX2 copyright headers
  intact and record imported files and their upstream commit in `third_party/README.md`.
- The game executable is untrusted input. Run Python that reads it with `python3 -I`.
- Never fetch, consult or copy source code from re3/reVC or other decompilations taken down
  by rights holders. Everything is derived from our own analysis of the binary; well-known
  class/function names may be used as labels, with evidence from the binary.

## Layout

- `scripts/setup.sh <iso>` — extract ELF, Ghidra headless analysis, function CSV.
- `scripts/classify.sh` — per-function HW/syscall/VU0/MMI fingerprint + module map (`out/modules.md`).
- `tools/ghidra/*.java` — Ghidra scripts (R5900 language: `r5900:LE:32:default`).
- `tools/modules.py` — module labelling.
- Planned: `recomp/` (MIPS→C recompiler), `runtime/` (kernel, SIF/IOP HLE, PCSX2 GS+VU1), `assets/`.

## Environment

- Ghidra 12.1.4 via Homebrew: `/opt/homebrew/opt/ghidra/libexec`; JDK:
  `$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home` (export `JAVA_HOME`).
- R5900 support: ghidra-emotionengine-reloaded in `~/Library/ghidra/ghidra_12.1.4_PUBLIC/Extensions/`.
- Run ad-hoc Ghidra scripts against the existing project with
  `analyzeHeadless ghidra_project vicecity -process SLUS_205.52 -noanalysis -readOnly -scriptPath <dir> -postScript <Script>.java <args>`.
- R5900 Ghidra gotchas: `Instruction.getRegister()` returns `v0_lo`-style aliases and is null for
  some 64/128-bit forms — read registers from `getOpObjects()` and normalise names.

## Decisions

- Graphics: reuse PCSX2's GS renderer (Metal backend) and VU1 for now. Replacing RenderWare's
  PS2 pipelines with a native Metal renderer is a possible later phase.
- Runtime replaces Sony SDK library functions wholesale (HLE) where possible, rather than
  emulating hardware registers.

## Commits

Author as `arwen <arwensofia23@gmail.com>`. End messages with
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Update `docs/PLAN.md` status when a
task lands.
