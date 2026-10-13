# ps2recomp

Static recompiler: R5900 (Emotion Engine) ELF + function list → C for arm64 macOS,
plus the `vc_game` CMake target that builds the generated C into a static library.

```sh
cmake -S recomp -B build/recomp -G Ninja && cmake --build build/recomp
ctest --test-dir build/recomp --output-on-failure        # semantic + trace tests
cmake --build build/recomp --target vc_game              # regenerate recomp_out/ + compile
build/recomp/ps2recomp --elf game/SLUS_205.52 --disasm 0x0011d088 32   # quick disassembly
```

`recomp_out/` is derived from the game and git-ignored. Never commit it.

## Using vc_game from the runtime

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../recomp ${CMAKE_BINARY_DIR}/recomp)
if(TARGET vc_game)                        # not defined when game/ or out/ is missing
	target_link_libraries(vicecity PRIVATE vc_game)   # alias: vc::game
endif()
```

- Include dirs (PUBLIC): `runtime/include` (`ps2_abi.h`), `recomp/include` (`ps2_ops.h`,
  `ps2_recomp.h`), `recomp_out/` (`ps2_funcs.h`, `ps2_names.h`).
- The runtime implements every `RUNTIME PROVIDES` hook of `ps2_abi.h`. `vc_game` already
  contains weak defaults for `ps2_call_indirect` / `ps2_jump_indirect` (table lookup,
  unknown targets → `ps2_dispatch_unknown`), `ps2_trace_fn/bb` and `ps2_dispatch_unknown`.
- HLE overrides: define a strong `ps2f_XXXXXXXX`, or by name
  `void PS2_FUNC_sceMcInit(ps2_cpu *c) { ... }` (`ps2_names.h`, also `PS2_ADDR_<name>`).
- Linking adds `-alias_list recomp_out/ps2_aliases.txt`: every named function is also
  `ee_<name>` (e.g. `b ee_sceMcInit` in lldb). The `ee_` prefix keeps EE names such as
  `powf` from clashing with host libraries.

Cache options (all `VC_GAME_*`):

| option | default | |
|---|---|---|
| `VC_GAME_ELF`, `VC_GAME_FUNCTIONS`, `VC_GAME_CONFIG` | `game/SLUS_205.52`, `out/functions.csv`, `recomp/config/SLUS_205.52.cfg` | inputs |
| `VC_GAME_NAMES`, `VC_GAME_MODULES` | `tools/library_names.csv`, `out/modules.csv` | optional name/module tables |
| `VC_GAME_OUT_DIR` | `recomp_out` | generated C |
| `VC_GAME_FILES` | 64 | translation units (fixed address buckets) |
| `VC_GAME_TRACE` | 0 | 1 = hook on function entry, 2 = on every basic block, 3 = both |
| `VC_GAME_TRACK_PC` | 0 | 1 = `c->pc` = current block, 2 = also before every load/store |
| `VC_GAME_BACKEDGE` | ON | loop back-edge hook: `if (--ps2_backedge_budget < 0) ps2_backedge(c, loop_head)` on every backward branch, for interrupt delivery in busy-wait loops |
| `VC_GAME_OPT` | `-O2` | |
| `VC_GAME_DEBUG_INFO` | ON | `-gline-tables-only`: host pc → generated line, which carries the EE address comment |
| `VC_GAME_LINK_ALIASES` | ON (Apple) | `ee_<name>` symbols |
| `VC_GAME_CCACHE` | ON | uses ccache/sccache when found |

Regeneration runs when the ELF, CSVs, config or `ps2recomp` change. Files are only
rewritten when their content changes and each unit declares only the functions it uses,
so e.g. adding one CSV entry recompiles one unit (Ninja `restat`).

## Debugging aids (`ps2_recomp.h`)

- `ps2_recomp_symbols[]` (entry, size, name, module) and `ps2_recomp_find_symbol`,
  `ps2_recomp_format_addr` ("sceMcInit+0x24 [sony:libmc]", async-signal-safe),
  `ps2_recomp_host_to_ee` (host pc → EE entry via the `ps2f_` symbol).
- Trace points are emitted everywhere but compile to nothing unless `VC_GAME_TRACE` /
  `VC_GAME_TRACK_PC` are set. The default hooks write `{addr|kind, sp}` into the lock-free
  ring `ps2_trace_buffer` (8192 entries); `ps2_trace_dump(fd, n)` prints it symbolized and
  is safe in a signal handler.
- The host call stack mirrors the EE call stack (every `jal` is a C call), so a host
  backtrace symbolized with `ps2_recomp_host_to_ee` is an EE backtrace, minus frames
  replaced by tail calls.

## Layout

- `src/decoder.*`, `src/ops.def`: instruction tables (core, MMI, COP0/1/2).
- `src/image.*`: bounds-checked ELF loader (the executable is untrusted input), CSV readers.
- `src/translator.cpp`: per-function recursive descent, jump-table recovery
  (predecessor-aware backward walk with a linear fallback), control flow, delay slots,
  trace points. `src/emit_insn.cpp`: C for each non-branch instruction.
- `src/main.cpp`: entry discovery, symbols, file split, dispatch/name tables, report.
- `include/ps2_ops.h`: inline instruction semantics used by generated code.
  `include/ps2_recomp.h` + `support/ps2_recomp_support.c`: symbols, tracing, default dispatch.
- `cmake/vc_game.cmake`: the `vc_game` target.
- `tests/`: `cases.inc` snippets are assembled (`asm.h`), translated by the real emitter
  (`test_gen`), compiled and checked (`test_runner`); `test_trace.cpp` rebuilds them with
  all trace points on and checks hooks, ring buffer, `c->pc`, symbols and the miss hook.
  The `x_*` cases cross-check corner cases against PCSX2's interpreter/recompiler.

## Entry discovery

CSV entries, the ELF entry and `.ctors` are seeds. Then, to a fixpoint: direct call and
branch targets; then *address-taken* code: `lui`+`addiu`/`ori` constants and data words that
point into a `code` range (config) at code no function reaches and that follows a function
terminator (`jr`/`j`/`b`, optional nop padding). Data words forming jump-table-like runs are
skipped, and a code pointer into a hole of its own function is reported as a label (e.g.
`__builtin_setjmp` receivers), not made an entry. The game has one RWX segment and no section
names, hence the explicit `code` ranges in `config/SLUS_205.52.cfg`.

## Translation model

One weak C function `ps2f_XXXXXXXX(ps2_cpu *c)` per entry. Branches are `goto`s; branch
conditions are evaluated before the delay slot; branch-likely annuls the delay slot when
not taken. `jal` is a direct call, `jr $ra` a return, branches leaving the function are
`musttail` calls, `jalr`/unresolved `jr` go through `ps2_call_indirect`/`ps2_jump_indirect`.
GCC-style jump tables become a `switch` whose default goes to `ps2_jump_indirect`.

## Known non-translatable control flow (runtime must override)

- `0x0011d088` (libgcc `__sjthrow`) ends in `__builtin_longjmp` (`jr v0` at `0x0011d1f8`, fp/sp
  reloaded from the handler's jmpbuf). `0x0011da60`/`0x0011dc40` (`__throw`-style DWARF
  unwinders) use `__builtin_eh_return`: they overwrite their own saved-`$ra` stack slot with
  `0x0011dbd0`/`0x0011ddb8` and "return" into it; in C, `jr ra` returns to the caller instead.
  No function outside libgcc registers a SJLJ handler, so reaching any of these means an
  uncaught C++ exception: override them with a fatal error (trace dump + abort).
