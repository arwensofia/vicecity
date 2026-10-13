# runtime/iop — emulated IOP

The IOP is not recompiled. Its services are emulated at the SIF RPC level: libkernl's
SIF/RPC functions are replaced (strong `ps2f_XXXXXXXX` overrides, `sif_hle.cpp`), and the
Sony libraries above them (libcdvd, libmc, libpad, libsdr, loadfile, IOP heap) plus the
game's own drivers stay recompiled and talk to native servers. The server IDs, function
numbers and buffer layouts are in [`docs/BOOT.md`](../../docs/BOOT.md) §3.

```
 recompiled libcdvd/libmc/libpad/... ──► sceSifBindRpc / sceSifCallRpc  (sif_hle.cpp)
                                           │ copy send buffer, DispatchRpc, copy reply,
                                           │ run end function (CallEe), return 0
                                           ▼
   RpcServer by service id (iop_core.cpp registry)
   0x80000006 LOADFILE   0x80000003 IOP heap   0x80000001 fileio*   0x80000701 sdrdrv
   0x8000059x cdvdfsv    0x80000100 padman     0x80000400 mcserv
   0x65686577 CDSTREAM   0x65686579/78 SAMPMAN2 (silent)
        │                   │                     │
     IsoImage            pad frames → EE        McFs (host dir)
   (iso_fs.cpp)         (svc_pad.cpp)         (mc_fs.cpp)
```

`*` fileio (`sceOpen`/`sceRead`/`sceLseek`/`sceClose`/`sceWrite`) is replaced at the API
level, because the SDK 2.4 protocol relies on IOP→EE command callbacks.

| File | Contents |
|---|---|
| `iop.h`, `iop_core.cpp` | State, EE/IOP memory access (bounds-checked), RPC registry, vc_iop.h entry points (init, vblank, SIF registers, SIF DMA into a 2 MB IOP RAM, IOP heap) |
| `sif_hle.cpp` | EE-side overrides: SIF cmd/RPC, IOP reboot/sync, fileio API |
| `iso_fs.*` | ISO9660 reader for the disc image (untrusted input) |
| `mc_fs.*` | Memory card filesystem in `~/Library/Application Support/vicecity/mc0` |
| `svc_system.cpp` | LOADFILE, IOP heap, fileio (`cdrom0:`, `rom0:ROMVER`), sdrdrv |
| `svc_cdvd.cpp` | cdvdfsv: init, S/N commands, `sceCdSt*` streaming, search file, disk ready |
| `svc_pad.cpp`, `vc_pad.h`, `vc_pad_gc.mm` | padman + input sources (keyboard map, GameController, injection) |
| `svc_mc.cpp` | mcserv |
| `svc_game.cpp` | Rockstar's CDSTREAM.IRX and SAMPMAN2.IRX |

Completion is synchronous: data is in EE memory and the end function has run before
`sceSifCallRpc` returns, so every `sceSifCheckStatRpc` poll reports done.

Tests (`tests/`, ctest): synthetic ISO image + the user's disc (`VC_ISO=<image>`, skipped
otherwise), memory-card semantics, and RPC dispatch through the overrides with synthetic
EE calls. Build standalone:

```sh
cmake -S runtime/iop -B build/runtime-iop -G Ninja && cmake --build build/runtime-iop
VC_ISO=~/path/to/disc.iso ctest --test-dir build/runtime-iop --output-on-failure
```
