# ps2recomp

Static recompiler: R5900 (Emotion Engine) ELF + function list → C for arm64 macOS.

```sh
cmake -S recomp -B build/recomp -G Ninja && cmake --build build/recomp
ctest --test-dir build/recomp --output-on-failure        # semantic tests
build/recomp/ps2recomp --elf game/SLUS_205.52 --functions out/functions.csv \
    --config recomp/config/SLUS_205.52.cfg --out recomp_out
recomp/check_output.sh                                   # clang -fsyntax-only over recomp_out
recomp/check_output.sh recomp_out -O2 -c                 # full compile to recomp_out/obj
```

`recomp_out/` is derived from the game and git-ignored. Never commit it.

## Layout

- `src/decoder.*`, `src/ops.def` — instruction tables (core, MMI, COP0/1/2).
- `src/image.*` — bounds-checked ELF loader (the executable is untrusted input), CSV reader.
- `src/translator.cpp` — per-function recursive descent, jump-table recovery, control flow
  and delay-slot emission. `src/emit_insn.cpp` — C for each non-branch instruction.
- `src/main.cpp` — entry discovery (CSV + `.ctors` + jal/branch targets, iterated to a
  fixpoint), file splitting, dispatch table, `recomp_report.txt`.
- `include/ps2_ops.h` — inline instruction semantics used by generated code.
- `../runtime/include/ps2_abi.h` — contract with the runtime (context, memory, hooks).
- `tests/` — `cases.inc` snippets are assembled (`asm.h`), translated by the real emitter
  (`test_gen`), compiled and checked against register/memory expectations (`test_runner`).

## Translation model

One weak C function `ps2f_XXXXXXXX(ps2_cpu *c)` per entry. Branches are `goto`s; branch
conditions are evaluated before the delay slot; branch-likely annuls the delay slot when
not taken. `jal` is a direct call, `jr $ra` a return, branches leaving the function are
`musttail` calls, `jalr`/unresolved `jr` go through `ps2_call_indirect`/`ps2_jump_indirect`.
GCC-style jump tables (`lw` from `lui`-based table + index, bounded by `sltiu`) become a
`switch`. The runtime overrides any function by defining a strong symbol with its name.
