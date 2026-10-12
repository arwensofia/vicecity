# vicecity

Personal project: a native macOS (Apple Silicon) port of *Grand Theft Auto: Vice City*
(PS2, NTSC-U v1.40, `SLUS_205.52`) by static recompilation.

**No game data is in this repo.** You need your own disc image; everything derived from
it (`game/`, `ghidra_project/`, `out/`) is generated locally and git-ignored.

## Requirements

- macOS on Apple Silicon
- `brew install ghidra` (pulls in `openjdk@21`)

## Usage

```bash
scripts/setup.sh "/path/to/Grand Theft Auto - Vice City (USA) (v1.40).iso"
```

This mounts the ISO read-only, extracts `SLUS_205.52`, runs Ghidra headless analysis,
labels functions from leftover debug strings, and writes `out/functions.csv`.

Known-good executable SHA-1: `80d9fc73514308a16683f0b807f75967b67e1c87`

See [docs/PLAN.md](docs/PLAN.md) for the roadmap and current status.
