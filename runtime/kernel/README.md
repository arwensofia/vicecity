# runtime/kernel, runtime/mem, runtime/app — EE side of the runtime

The EE kernel HLE, the EE memory map and the `vicecity` macOS executable. Contracts:
`runtime/include/ps2_abi.h` (recompiled code), `runtime/include/vc_iop.h` (IOP side),
`runtime/gs/vc_gs.h` (graphics), `recomp/include/ps2_recomp.h` (symbols, trace, back-edge hook).

```
cmake -S runtime -B build/runtime -G Ninja && cmake --build build/runtime
build/runtime/app/vicecity --iso <image> [--log run.log] [--syscalls] [--headless]
                           [--screenshot 120:frame.ppm] [--renderer=sw]
```

The executable is loaded from `game/SLUS_205.52` (or `--elf`, or the disc via `vc_iop_read_disc_file`).
The recompiled C is generated into `build/runtime/recomp_out` (`VC_GAME_OUT_DIR`) by recomp's
`vc_game` target. Debug builds of the game code: `-DVC_GAME_TRACE=1` (function-entry ring, `--trace`
prints entries), `=3` (also basic blocks; `VC_TRACE_ON_UNMAPPED=<n>` dumps the last n records at the
first access to unmapped memory), `-DVC_GAME_TRACK_PC=1` (default: `c->pc` per block, so logs and
crash reports name EE addresses).

## Files

| File | |
|---|---|
| `kernel/threads.cpp` | EE threads and semaphores. Each thread = own `ps2_cpu` + own host fiber (`fiber_arm64.S`, 8 MB lazily committed stack with guard page; the main thread uses the EE host thread's stack). Strict priorities, FIFO per priority, no time slicing; preempted threads go to the head of their queue, voluntary priority changes/rotations to the tail. COP2 state is copied to the incoming thread on every switch (one VU0). `SetupThread`/`SetupHeap` (argc/argv block), `ReferThreadStatus`, wakeup counts, suspend, `DumpThreads()` for hangs. |
| `kernel/intr.cpp` | Interrupt delivery: INTC/DMAC handler chains (`Add/RemoveIntcHandler`, `Add/RemoveDmacHandler`, `Enable/DisableIntc/Dmac`, per-handler enable), alarms (`SetAlarm`/`ReleaseAlarm`, the libkernl Timer-3 patch done natively, HSYNC units), vblank at 59.94 Hz host time (`SetVSyncFlag`, `vc::gs::VSync()`, INTC VBLANK_S, VBLANK_E 1.43 ms later), EE timers 0–3 (count/mode/comp/hold, compare/overflow interrupts), IOP polling, idle wait + "all threads blocked" diagnostics, `--screenshot`. |
| `kernel/syscalls.cpp` | `ps2_syscall`: all 65 syscalls of docs/MODULES.md plus the alarm (252–255) and TLB (0x54–0x56) syscalls of the libkernl patches, `SetSyscall`/kCopy (0x5A)/0x5B/0x83 as harmless no-ops, Enable/DisableIntcHandler, Release/CancelWakeup, ReferSemaStatus, ResumeThread. Unknown numbers log once and return 0. SIF syscalls go to `vc_iop_sif_*`. |
| `kernel/cop.cpp` | COP0 (per-thread Status/EIE, `di`/`ei`, Count at 294.912 MHz, CPCOND0 from D_PCR/D_STAT), COP2 control (`vcallms` on PCSX2's VU0 interpreter via `vc::gs::ExecuteVU0`, CMSAR1 starts VU1, FBRST resets), break/trap (log once per site), unimplemented (fatal). |
| `kernel/dispatch.cpp` | `ps2_call_indirect`/`ps2_jump_indirect` (miss = fatal with symbolized site, trace dump, registers, threads), `ps2_backedge` (interrupt delivery inside loops), trace hooks, crash handlers (SIGSEGV/SIGBUS/... print host backtrace — recompiled functions are `ps2f_XXXXXXXX`/`ee_<name>` — plus EE state). |
| `kernel/spr_dma.cpp` | Scratchpad DMA (channels 8/9: normal, interleave, toSPR source chain, fromSPR destination chain), run after the register write that started it. |
| `kernel/hle.cpp` | Function overrides: `_InitSys` 0x11c140 (keeps the library semaphores and the libkernl helper thread, skips the kernel-mode patch installers), libgcc exception throw paths 0x11d088/0x11d200/0x11da60/0x11dc40 (fatal), FMV player 0x3819b0 (skipped: no IPU yet). |
| `kernel/iop_weak.c` | Weak defaults for the IOP PROVIDES half of `vc_iop.h`. |
| `mem/mem.cpp` | `ps2_mem_*_slow`: RAM mirrors (KSEG0/1, 0x2/0x3 uncached), timers, vc_gs registers, SIF → `vc_iop_sif_*`, KPUTCHAR → log, VU0/VU1 memory, GS privileged registers, ROM (zero). Every register access is an interrupt delivery point. Also the KERNEL PROVIDES half of `vc_iop.h`. |
| `app/main.mm` | NSWindow + layer-backed view (keys → `vc_pad_key`), EE host thread (64 MB stack), `vc::gs::Init`, `vc_iop_init`, ELF load (`elf_loader.cpp`, bounds-checked), `kernel::Run`. Headless mode keeps the main run loop alive for the Metal device. |

## Interrupt model

Recompiled code is never interrupted asynchronously. Interrupts are taken at *delivery points*:
syscalls (on exit), EE register accesses, `ei`/`mtc0 Status`, CMSAR1 writes, back edges (every
1024 backward branches), and while all threads are blocked (idle). vc_gs's `int0`/`int1`
callbacks only set a flag; handlers never run inside a vc_gs call. Handlers run on a separate
`ps2_cpu` with an EE stack at 0x40000–0x50000, with the gp recorded at registration; i* calls
in handlers (or in i* syscalls made from thread context, e.g. RPC end functions run by
runtime/iop) defer the reschedule to the end of the interrupt / next delivery point.
