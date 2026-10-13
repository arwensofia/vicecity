# Boot path of `SLUS_205.52`

What the game does from `entry` to its first frame, and what it needs from the IOP and the
kernel. Static analysis only (Ghidra project + `tools/callargs.py`); nothing here has been
checked by running the game yet. Function names follow re3/reVC (the public PC-port
reimplementation) or the Sony SDK where the evidence matches; names in *italics* are guesses.
Confidence: **H** high (string/SDK-signature evidence), **M** medium (call pattern), **L** low.

Conventions: `$gp = 0x4b1df0` (set by crt0). RPC calls are
`sceSifCallRpc(client, fno, mode, send, ssize, recv, rsize, end_func, end_param)`;
mode 0 = blocking, 1 = `SIF_RPCM_NOWAIT` (the caller then spins on `sceSifCheckStatRpc`).

## 1. Boot sequence

### 1.1 crt0 and kernel init (`0x100008`)

| Step | Address | What |
|---|---|---|
| 1 | `0x100008` | Clear BSS `0x4b0100–0x6ce880` with 128-bit stores |
| 2 | `0x100064` | `SetupThread` (syscall 60): gp `0x4b1df0`, stack `-1` (top of RAM), stack size `0x4000`, args block `0x4b7fc0`, root/exit `0x1000c0`. The returned value becomes `$sp` |
| 3 | `0x100080` | `SetupHeap` (syscall 61): start `0x6ce880` (end of BSS), size `-1` |
| 4 | `0x100084` | `_InitSys` `0x11c140` (see below) |
| 5 | `0x10008c` | `FlushCache(0)`, `ei` |
| 6 | `0x1000a4` | `main(argc = *0x4b7fc0, argv = 0x4b7fc4)` at `0x2739e0`, then `Exit(ret)` |

`_InitSys` (libkernl, all **H/M**): `0x11c040` creates two binary semaphores (libkernl
internal locks), `0x11c0d0` installs a kernel-mode helper via `SetSyscall`, `InitAlarm`
`0x114278` (installs the alarm syscalls `0xFC/0xFE` from `.data` patches), `InitThread`
`0x114428`, `InitOsdConfigPatch` `0x11c250`, `InitTLBFunctions` `0x11b8c8`.

`InitThread` matters for scheduling: it creates the libkernl **"top" thread**
(entry `0x114350`, priority **0**, stack `0x4b2920`, size `0x400`, semaphore max `0xff`)
and then lowers the main thread to priority **1**. The top thread waits on its semaphore and
performs `WakeupThread` / `RotateThreadReadyQueue` / `SuspendThread` requests that
interrupt handlers queued with the libkernl `i*` wrappers (`0x114500`, `0x114598`,
`0x114618`). An HLE kernel can implement those requests directly and skip this thread.

### 1.2 `main` (`0x2739e0`)

| # | Address (call site) | Callee | What | Conf. |
|---|---|---|---|---|
| 1 | `0x2739e8` | `0x123810` → `0x123770` | `__main`: run the 75 `.ctors` slots `0x4a9110–0x4a923c` (66 non-null) through `jalr` | H |
| 2 | `0x2739f0` | `0x1002f0` → `0x12d630` | `CMemoryHeap::Init(&gMainHeap 0x4b1980, size)` — size = `0x1ffa800 − (0x6ce880+0x6000) − 0x10` ≈ 25.1 MB, obtained with newlib `malloc` (`0x107190`), i.e. from the `SetupHeap` region. Assert string names `MemoryHeap.cpp` | H |
| 3 | `0x2739f8` | `0x25f750` | `sceCdInit(SCECdINIT)`, `sceCdMmode(SCECdDVD=2)` | H |
| 4 | `0x273a14` | `sprintf` `0x10a780` | Build `cdrom0:\SYSTEM\IOPRP241.IMG;1` (dir prefix `SYSTEM\` at `0x4ad250` = `$gp−0x4ba0`) | H |
| 5 | `0x273a1c–0x273a6c` | | `sceSifInitRpc(0)`; **loop** `sceSifRebootIop(path)` `0x11b668` until ≠0 (sends `rom0:UDNL <path>`); **loop** `sceSifSyncIop` `0x11b630` until ≠0 (polls `sceSifGetReg(4) & 0x40000`); `sceSifInitRpc(0)` | H |
| 6 | `0x273a70` | `0x25f750` | `sceCdInit` + `sceCdMmode(2)` again (after the IOP reboot) | H |
| 7 | `0x273a78` | `0x117690` | *`sceFsReset`*: clears the fileio client state (`0x42e6c0`, `0x4b63a8`) | M |
| 8 | `0x273a80` | `0x25f720` | `sceCdDiskReady(0)` (blocking), `sceCdStandby()`, `sceCdSync(0)` | H |
| 9 | `0x273abc–0x273e7c` | `sceSifLoadModule` `0x11b168` | Load the 8 IRX modules (§2). Each load is retried forever on failure | H |
| 10 | `0x273eb8` | `CreateThread`/`StartThread` | Idle thread: entry `0x273270` (`b .` spin), priority 127, stack `0x5bf8a0`, size `0x800` | H |
| 11 | `0x273ecc` | `0x278ee0` | `CPad::Initialise`: `scePadInit(0)`, `scePadPortOpen(0,0,0x5c0180)`, `scePadPortOpen(1,0,0x5c0280)`, clear pad state (strings `Initialising CPad...`, `CPad ready`) | H |
| 12 | `0x273ed4` | `0x275eb0(0)` | `CPad::GetPad(0)` (returns `0x5c0380 + n*0x94`); main zeroes a halfword at `+0x6a` | M |
| 13 | `0x273ee8` | `0x40c4b0(0x6c7ec0)` | `CMemoryCard::Init`: `sceMcInit()` (strings `Memory card initialsed`) | H |
| 14 | `0x273ef4` | `0x406670(0x18, 0)` | `RsEventHandler(rsINITIALIZE)` → `AppEventHandler` `0x2717a0` → `CGame::InitialiseOnceBeforeRW` `0x270d00` (`CFileMgr::Initialise` `0x25fb00`, **`CdStreamInit(5)` `0x336870`**, `size of matrix…` debug prints) then `RsInitialize` `0x406450` (640×448 defaults). Returns 0 → `main` returns | H |
| 15 | `0x273f10` | `0x409570(0x6c7ec0, 0)` | Boot memory-card check (`sceMcGetInfo`/`Chdir`/`GetDir`/`Open`/`Read`/`Close` on port 0, see §5). Result 0/3 → skip to 17; 1/2 → step 16 first. Sets `$gp−0x12f4` (selects the logo-movie variant) | M |
| 16 | `0x273f5c` | `0x273280(1)` | Early RW init + file cache (same work as step 18) followed by `0x1953d0`/`0x195970` (`CText` init: picks `AMERICAN/FRENCH/GERMAN/ITALIAN/SPANISH.GXT`), `0x2cfd30` (`CFont::Initialise`, `MODELS/FONTS.TXD`), `0x263ef0` (memory-card message front end). Only on MC result 1/2 | M |
| 17 | `0x273f90` | `0x273920` | **Logo movies** (§1.4): `cdrom0:\MOVIES\VCNTSC.PSS;1` then `cdrom0:\MOVIES\VICE.PSS;1` through the FMV player `0x3819b0`; RW video is shut down (`0x3bc760`) before and re-initialised (`0x3bc170`) after if RW was already up | H |
| 18 | `0x273f98` | `0x273280(0)` | If RW is not yet up (`$gp−0x12e4`==0): `RsRwInitialize` `0x4064f0` + `CGame::InitialiseRenderWare` `0x2708d0`; then the loading screen `0x272730`; **file-position cache**: 117 calls to `0x1324a0` (`sceCdDiskReady(0)` + `sceCdSearchFile`, retried until found) for the boot file list (path strings `0x437510–0x438580`: `ANIM\CUTS.IMG/.DIR`, `MODELS\*.TXD`, `MODELS\GTA3.DIR`, `TEXT\AMERICAN.GXT`, `MODELS\COLL\*.COL`, `DATA\*.DAT/.CFG/.IDE/.ZON/.SCM`, `DATA\MAPS\*\*.IDE/.IPL`, `TXD\LOADSC0–13.TXD`, `TXD\SPLASH1.TXD`); `AddIntcHandler(INTC_VBLANK_S=2, 0x274020)`; `sceCdReadClock` → `srand` | H |
| 19 | `0x273fa0–0x273fbc` | | **Busy-wait** until the vblank counter `$gp−0x12ec` (incremented by `0x274020`) reaches 100 (~1.7 s) | H |
| 20 | `0x273fc4` | `0x2707a0` | `CGame::InitialiseOnceAfterRW`-like: GXT, surface/ped data (`DATA\SURFACE.DAT`), loading screens (`Loading the Game`), then `Initialising audio` → `0x239320` → `cSampleManager::Initialise` → audio RPC bring-up `0x23ab80` (§3.4) | M |
| 21 | `0x273fcc` | `0x271c50` | `TheGame` (`Into TheGame!!!`): loads `DATA\GTA_VC.DAT` (`Starting Game`) via `CGame::Initialise` `0x26fff0`, memory-card load menu, then the per-frame main loop. Does not return in normal play | H |

**First frame.** The first picture on screen is the Rockstar logo movie (step 17, FMV player
driving the GS directly). The first RenderWare frame is the loading screen
`LoadingScreen(nil, nil, "loadsc0")` (`0x272730`, string at `$gp−0x4b98`) in step 18 (or 16);
the progress screens of `CGame::Initialise` (`Loading the Game` / `Setup streaming` / …) follow
in step 21.

### 1.3 RenderWare bring-up (`RsRwInitialize` `0x4064f0`, H)

1. `0x3b3710(0x180000, 0x14, 1)` — sky2 DMA packet-heap preallocation (*`_rwDMAPreAlloc`*, M).
2. `RwEngineInit(psGetMemoryFunctions(), 0, 0x15f90)` `0x3a5508`.
3. `psInstallFileSystem` `0x4063d0` → `0x131890`: registers an engine plugin whose constructor
   `0x131980` replaces 10 entries of `RwOsGetFileInterface()` with the game's CD file system
   (§5.2).
4. `RsEventHandler(rsPLUGINATTACH…)` (events 3, 9, 4 → `AppEventHandler`).
5. `RwEngineOpen` `0x3a53b8`, `rsSELECTDEVICE` (`0x406320`), `RwEngineStart` `0x3a5218`,
   `rsREGISTERIMAGELOADER` (event `0xa`), `RwTextureSetMipmapping(FALSE)` `0x3ac110`,
   `RwTextureSetAutoMipmapping(FALSE)` `0x3ac148`.

`RwEngineStart` reaches the sky2 video init `0x3bb168`: `sceGsResetGraph(…, 1)` (interlaced
field/frame per mode), `sceGsSetDefDBuffDc(0x444850, …)`, `sceDmaReset`, GIF-channel
`sceDmaSendN` of the initial GS register packet, then `0x3b1ff8`/`0x3b1e48` install the
sky2 interrupt handlers (§4.3). `CGame::InitialiseRenderWare` `0x2708d0` then validates the
disc (`ValidateVersion` `0x270d80`, reads `models\coll\peds.col` and checks for
`grandtheftauto3`), creates the texture-dictionary store (`TexDictionary`), the camera, etc.

### 1.4 FMV player (`0x3819b0`, `0x3812e0`, H)

Derived from Sony's MPEG sample (libmpeg + libipu), built with the game's compiler:

- `0x381b60` (init): `sceSifInitRpc`, `sceSifInitIopHeap` `0x11a468`, `sceGsResetPath`,
  `sceDmaReset`, `sceGsResetGraph(0, SCE_GS_INTERLACE, SCE_GS_NTSC, SCE_GS_FRAME)`,
  `sceGsSetDefDBuff(…, 640, 240)`, `sceSdRemoteInit` + `sceSdRemote(1, 0x8000 rSdInit)`.
- `0x382220` (open): for `cdrom0:` paths: `sceCdDiskReady(0)` once,
  `sceSifAllocIopHeap(0x28010)`, `sceCdStInit(0x50, 5, iopbuf)`, `sceCdSearchFile`,
  `sceCdStStart(lsn, {0,0,0})`; otherwise `sceOpen`/`sceLseek`.
- `0x3812e0` creates two priority-1 threads (§4.1), adds `INTC_VBLANK_S` handler `0x3809f0`
  and DMAC ch2 (GIF) handler `0x3809a0`; `0x3819b0` tears them down (`RemoveIntcHandler`,
  `RemoveDmacHandler`). IPU DMA uses ch3/ch4 (`0x373d68`, `0x374020`).
- Audio is played through `sdrdrv` (§3.2) with `rSdSetParam` (master volume, params
  `0xf80/0x1080`) and block transfers.

## 2. IOP modules

All loads happen in `main` (`0x273abc–0x273e7c`) through `sceSifLoadModule(path, 0, argv)`
(`0x11b168` → `0x11af40`, loadfile RPC `0x80000006` fno 0, send `0x200`, recv 8).
`argc` is **0** for every module (the `argv` pointer `0x4ad20c` points at a zero word), so no
module receives arguments. Paths are built as `"cdrom0:\" + "SYSTEM\" + name` — **without**
the `;1` version suffix. Failure prints `Can't Load Module %s` and retries forever.
`sceSifLoadModuleBuffer` (`0x11b2b0`) is linked but never called; there are no other loads.

Before the loads the IOP is rebooted with `cdrom0:\SYSTEM\IOPRP241.IMG;1` (an IOPRP replacement
image; such images carry newer ROM modules such as `cdvdman`/`cdvdfsv` — contents not yet
examined), sent as `rom0:UDNL <path>`.

| # | Path | Origin | EE-side client / RPC server |
|---|---|---|---|
| 0 | `cdrom0:\SYSTEM\IOPRP241.IMG;1` | Sony IOP replacement image (reboot) | `cdvdfsv` 0x80000592/3/5/7/a, fileio 0x80000001, heap 0x80000003, loadfile 0x80000006 come from ROM + this image |
| 1 | `cdrom0:\SYSTEM\SIO2MAN.IRX` | Sony standard | none (used by padman/mcman) |
| 2 | `cdrom0:\SYSTEM\PADMAN.IRX` | Sony standard | libpad: 0x80000100, 0x80000101 |
| 3 | `cdrom0:\SYSTEM\LIBSD.IRX` | Sony standard (SPU2 driver) | none directly |
| 4 | `cdrom0:\SYSTEM\SDRDRV.IRX` | Sony standard (libsd RPC server) | libsdr: 0x80000701 (FMV audio only) |
| 5 | `cdrom0:\SYSTEM\MCMAN.IRX` | Sony standard | none directly |
| 6 | `cdrom0:\SYSTEM\MCSERV.IRX` | Sony standard | libmc: 0x80000400 |
| 7 | `cdrom0:\SYSTEM\CDSTREAM.IRX` | **Game** (Rockstar CD streaming) | 0x65686577 (§3.3) |
| 8 | `cdrom0:\SYSTEM\SAMPMAN2.IRX` | **Game** (Rockstar sound/music driver, SPU2 via libsd) | 0x65686579 (SFX), 0x65686578 (music/streams) (§3.4); calls back into the EE server 0x12345678 (§3.5) |

## 3. SIF RPC

### 3.1 Table of servers and calls

Buffers are given as EE address / size. "recv" sizes are what the EE asks the IOP to
copy back into the receive buffer; return values are read from its first word/byte.

| Server ID | IOP module | EE client (struct) | fno | mode | send | recv | EE caller(s) | Meaning |
|---|---|---|---|---|---|---|---|---|
| `0x80000001` | fileio (ROM) | `0x4b6380` | `0xff` | 0 | 4 | 4 | `_sceFsInit` `0x117438` | init: tells IOP the EE buffer for unaligned read tails |
| | | | 0 | 0 | `0x418` | 4 | `sceOpen` `0x1176c8` | open (flags + 1 KB path) |
| | | | 1 | 0 | `0x14` | 4 | `sceClose` `0x117950` | close |
| | | | 2 | 0 | `0x20` | 4 | `sceRead` `0x117d08` | read |
| | | | 3 | 0 | `0x30` | 4 | *`sceWrite`* `0x117f78` | write |
| | | | 4 | 0 | `0x1c` | 4 | `sceLseek` `0x117ad0` | lseek |
| | | | 5,7,0xa–0xf,0x11,0x13,0x14,0x16–0x1a | 0 | various | 4 | `0x118238`–`0x11a2a0` | ioctl/remove/mkdir/dopen/getstat/… (linked, not used at boot) |
| `0x80000003` | IOP heap (ROM) | `0x4b6440` | 1 | 0 | 4 | 4 | `sceSifAllocIopHeap` `0x11a4f0` | alloc (FMV: `0x28010` bytes) |
| | | | 2 | 0 | 4 | 4 | `0x11a5e0` | free |
| | | | 3 | 0 | ? | 4 | `0x11a678` | load file into IOP heap |
| | | | 4 | 0 | `0xc` | 4 | `0x11a560` | (heap query) |
| `0x80000006` | loadfile (ROM) | `0x4b6800` | `0xff` | 0 | 0 | 4 | `0x11a768` | init / version |
| | | | 0 | 0 | `0x200` | 8 | `0x11af40` | **LoadModule** (used 8×) |
| | | | 2,3,6–0xa | 0 | | | `0x11b3e8`, `0x11b2f8`, `0x11a930`, … | buffer load, set/get addr, … (not used) |
| `0x80000100` | padman | `0x4b7d00` | 1 | 0 | `0x80` | `0x80` | `scePadInit`, `scePadPortOpen`, `0x121d48`, `0x122750`, … | all libpad commands go through fno 1 with the command code in the send buffer |
| `0x80000101` | padman | | | | | | `scePadInit` | bound only (second pad server) |
| `0x80000400` | mcserv | `0x6bb840` area | `0xfe` | 0 | `0x30` | `0xc` | `sceMcInit` `0x339a78` | version check (mcserv and mcman) |
| | | | 1 | 1 | `0x30` | 4 | `sceMcGetInfo` | |
| | | | 2 | 1 | `0x414` | 4 | `sceMcOpen` | |
| | | | 3,4,5,6 | 1 | `0x30` | 4 | `sceMcClose/Seek/Read/Write` | |
| | | | 0xa,0xc,0xd,0xe,0xf,0x10,0x11,0x12,0x14,0x15 | 1 (0x15: 0) | `0x30`/`0x414` | 4 | Flush, Chdir, GetDir, SetFileInfo, Delete, Format, Unformat, GetEntSpace, ChangeThreadPriority, fno 0x15 | standard mcserv numbering |
| `0x80000592` | cdvdfsv | | 0 | 0 | 4 | `0x10` | `sceCdInit` `0x33bfa8` | init (mode 0) |
| `0x80000593` | cdvdfsv (S cmds) | `0x43eb08` | `0x22` | 0 | 4 | 4 | `sceCdMmode` | media mode (2 = DVD) |
| | | | `0x16` | 0 | 0 | 4 | `sceCdBreak` | |
| | | | `0x01` | 0 | 0 | `0x10` | `sceCdReadClock` | RTC (used once to seed `srand`) |
| `0x80000595` | cdvdfsv (N cmds) | `0x43e290` | 6 | 1 | 0 | 0 | `sceCdStandby` `0x33c548` | spin up |
| | | | 9 | 0 | `0x14` | 4 | `_sceCdStreamCmd` `0x33cb20` | `sceCdSt*` stream commands (FMV) |
| | | | `0xe` | 0 | 0 | 4 | `0x33bc90` | N command 0x0E (status query on the standby path) |
| `0x80000597` | cdvdfsv | | 0 | 0 | `0x128` | 4 | `sceCdSearchFile` `0x33b820` | path → `sceCdlFILE` (LSN, size); ~120 calls at boot |
| `0x8000059a` | cdvdfsv | | 0 | 0 | 4 | 4 | `sceCdDiskReady` `0x33c288` | blocking ready (mode 0) |
| `0x80000701` | sdrdrv | | per call | 0/1 | `0x40` | `0x40` | `sceSdRemote` `0x11c5d8` | libsd remote calls; FMV only |
| `0x65686577` | **CDSTREAM.IRX** | `0x6ba0a0` | 0–4 | 0 | `0x10` @`0x6ba100` | `0x10` @`0x6ba140` | `0x336670`–`0x336930` | §3.3 |
| `0x65686579` | **SAMPMAN2.IRX** (SFX) | `0x5aef00` | `0x0aCCxx`/`0x0bCCxx` | 1 + poll | `0x10` @`0x5aeec0` (one call `0x490` @`0x5ae980`) | `0x40` @`0x5aeec0` | `0x239300–0x23b21c` | §3.4 |
| `0x65686578` | **SAMPMAN2.IRX** (music) | `0x5aee80` | `0x08CCxx`/`0x09CCxx` | 0 | `0x10` @`0x5aee40` | `0x40` @`0x5aee40` | `0x239720–0x23a1b8` | §3.4 |

The three game clients bind in a loop until the client's server pointer (`client+0x24`) is
non-zero (CDSTREAM spins 10,000 iterations between attempts), so the IOP HLE must answer
`sceSifBindRpc` for these IDs as soon as the modules are "loaded".

### 3.2 sdrdrv (`0x80000701`, FMV only, M)

`sceSdRemote(1, cmd, …)` command codes seen: `0x8000` (rSdInit, at FMV init), `0x8010`
(rSdSetParam; params `0x0f80`/`0x1080` = core 0/1 master volume L/R, values 0, `0x3fff`,
`0x7fff`), `0x80d0`, `0x80e0` (block-transfer start/stop, modes 2 and `0x13`), `0x8100`
(transfer status). Numbering follows libsdr's `rSd*` command IDs.

### 3.3 CDSTREAM.IRX — `0x65686577` (H for numbering/layout, M for the error codes)

EE side is re3's `cdstream.cpp` API. Every call is blocking (mode 0), sends up to four
32-bit words at `0x6ba100`, and returns one 32-bit word at `0x6ba140`.

| fno | EE function | Send words | Return | Notes |
|---|---|---|---|---|
| 0 | `CdStreamInit(nch)` `0x336870` | `nch` (=5) | — (rsize 0) | called once from `CGame::InitialiseOnceBeforeRW` |
| 1 | `CdStreamRead(ch, eebuf, lsn, nsect)` `0x3367f0` | channel, EE destination address, **absolute disc LSN**, sector count (2048-byte sectors) | ≠0 if the request was queued | IOP reads the sectors and DMAs them to `eebuf` (the EE does `SyncDCache` on the range before the call) |
| 2 | `CdStreamGetStatus(ch)` `0x336790` | channel | 0 = idle/done, `0xff` = reading, `0xfa` = waiting, anything else = error code | `CStreaming` polls this once per frame; ≥100 consecutive `0xff` is treated as error `0xfe` |
| 3 | `CdStreamSync(ch)` `0x3366e0` | channel | 0 = success, ≠0 = error | **blocks** until the channel's read completes |
| 4 | `CdStreamGetLastPosn()` `0x336740` | — | LSN after the last read (re3: `lastPosnRead`) | used by `CStreaming` to order requests (M) |

Channels: 0 and 1 are `CStreaming` (`0x2e0bc0`, `0x2e0cd0`, `0x2e1090`, `0x2e19a0`,
`0x2e5f90`, `0x2e7940`); **4 is the file system** (all `CFileMgr`/RW file reads, §5.2). 2/3
are not referenced directly. On a read error the file system waits one vblank (`0x273ff0`)
and re-issues the read forever.

Image names: `CdStreamAddImage` `0x3366a0` (64-byte entries at `0x6b9ea0`, count
`$gp−0x7f0`), `CdStreamGetNumImages` `0x336660`, `CdStreamGetImageName` `0x336670`;
`CStreaming` resolves each image's LSN/size with `sceCdSearchFile` (`0x25f6c0`), so the IOP
module never sees file names — only absolute LSNs.

### 3.4 SAMPMAN2.IRX — `0x65686579` (SFX) and `0x65686578` (music) (M)

Command encoding: the RPC *function number* carries the command,
`fno = (group << 16) | (cmd << 8) | arg8`, where `arg8` is a small immediate used by a few
commands (`0x0a0a00 | n`, music `0x09xx00 | n`). The 16-byte send buffer's first word holds
the main argument, typically packed bytes such as `(value << 8) | slot` or `0xff` for "all";
the reply overwrites the same buffer and the result is its first byte or word.

SFX server (client `0x5aef00`, buffer `0x5aeec0`, NOWAIT + `sceSifCheckStatRpc` spin):

| fno | EE caller | Send word 0 | Reply | Meaning (guess where marked) |
|---|---|---|---|---|
| `0x0a0000` | `0x23ab80` | 0 | word > 0 = OK | init (`SAMPMAN INIT failed (rpc)` otherwise) |
| `0x0a0b00` | `0x23ab80` | sample index | word | per-sample bank offset (*guess*); called for **every sample 0…0x2b45 (11,078)** at boot |
| `0x0a0c00` | `0x23ab80` | sample index | word | per-sample sample rate; EE rejects > 48000 with `Sample Rate of Sample %d … is invalid` |
| `0x0a0d00` | `0x23ab80`, `0x23a9f0` | `0xff` or slot | byte | (*guess*: load/select SFX bank) |
| `0x0a0200` | `0x23ab10` | 0 | — | |
| `0x0a0a00 + n` | `0x23a310` | 0 | byte | status query `n` |
| `0x0b0000` | `0x2395e0` (per frame, from `0x237510`/`0x237cf0`) | **`0x490`-byte block at `0x5ae980`** (mode 0) | `0x40` bytes | per-frame channel/voice update list |
| `0x0b0100` | `0x23ab80`, `0x23a950` | `(n << 8) \| 0xff` | byte | boot waits until the reply byte is **1** (re-sending `0x0a0d00`, then 8 vblanks between tries) |
| `0x0b0200` | `0x23a790`, `0x23a810` | `(v << 8) \| slot` (slot < 5) | byte | (stream slot control, *guess*) |
| `0x0b0300` | `0x23a6c0`, `0x23a8b0` | `(v << 8) \| 0xff` / slot | byte | |
| `0x0b0400` | `0x2393a0` | 0 | — | sent right after the EE DTS service thread starts (§3.5) |
| `0x0b0500` | `0x239720` | slot | byte | |

Music server (client `0x5aee80`, buffer `0x5aee40`, blocking):

| fno | EE caller | Meaning |
|---|---|---|
| `0x080000` | `0x23ab80` | init; reply word must be ≠0 (`MUSIC INIT failed (rpc)`) |
| `0x080e00` | `0x239720`, `0x239b70`, `0x239ec0` | status query (reply word, 0 = idle) |
| `0x080700` | `0x239720`, `0x239ec0` | stop (*guess*) |
| `0x090000` | `0x239be0` | arg = byte; reply `-1` = failure |
| `0x090100` | `0x239720`, `0x239e60` | query (reply word) |
| `0x090300` | `0x23a0d0` | |
| `0x090500` | `0x239720`, `0x23a030` | |
| `0x0b00 \| …` | `0x239c50` | composed from arguments (`0x2051`, `0x811` patterns), music stream start (*guess*) |
| variable | `0x239a8c`, `0x239acc`, `0x239f60`, `0x23a120` | `fno = base \| arg8` |

Boot order inside `0x23ab80` (`cSampleManager` init, reached from `Initialising audio`):
bind SFX (retry) → `0x0a0000` → 11,078 × (`0x0a0b00`, `0x0a0c00`) → `DTS_querymemsize` /
`DTS_init` (EE-side, VU0) → start the DTS service thread (§3.5, sends `0x0b0400`) →
loop { if `$gp−0x1004`==0: `0x0a0d00(0xff)`; wait 8 vblanks; `0x0b0100(0xff)` } until reply
== 1 → wait 3 vblanks → bind music (retry) → `0x080000`. **An IOP HLE that does not return
1 for `0x0b0100` hangs the boot here.**

### 3.5 EE-side RPC server `0x12345678` (DTS encoder service, H)

The EE registers its own SIF RPC server, so SAMPMAN2 can call the EE:
thread `0x239330` does `sceSifSetRpcQueue(0x5aef30, tid)` (`0x1168e0`),
`sceSifRegisterRpc(0x5af7c0, 0x12345678, 0x239480, buf 0x5aef50, 0, 0, 0x5aef30)`
(`0x116978`), `sceSifRpcLoop(0x5aef30)` (`0x116d90`). Handler `0x239480` runs the DTS
Interactive encoder on four 1 KB blocks (bracketed by `sceDevVu0Pause/GetCnd` …
`PutCnd/Continue`). It is only needed when the IOP driver produces DTS output; an HLE that
outputs PCM can leave it idle, but the thread and registration still happen at boot.

## 4. Threads, synchronisation, interrupts

### 4.1 Threads

| Entry | Created by | Priority | Stack / size | What |
|---|---|---|---|---|
| `0x114350` | `InitThread` `0x114428` (from `_InitSys`) | 0 | `0x4b2920` / `0x400` | libkernl top thread (performs queued i-context thread ops) |
| main | crt0 | 1 after `InitThread`; **10** after audio init (`0x2393a0` calls `ChangeThreadPriority(self, 10)`) | top of RAM / `0x4000` | game |
| `0x273270` | `main` `0x273eb8` | 127 | `0x5bf8a0` / `0x800` | idle: `b .` |
| `0x3812c0` | FMV `0x3812e0` | 1 | FMV work + `0x58e680` / `0x800` | loops `RotateThreadReadyQueue(1)` (round-robin helper) |
| `0x383920` | FMV `0x3812e0` | 1 | FMV work + `0x66aec0` / `0x4000` | MPEG read/demux/decode thread (arg = work + `0x671f00`) |
| `0x239330` | `0x2393a0` (audio init) | 9 | heap `0x2000` (64-byte aligned) | EE RPC server `0x12345678` (§3.5) |

The FMV work area pointer is `$gp−0x6ac`. Thread stacks are not freed when the FMV ends;
the FMV threads are terminated/deleted by `0x3819b0`.

### 4.2 Semaphores and alarms

- Per-call temporary semaphores: `sceSifBindRpc`/`sceSifCallRpc` (blocking calls wait on a
  semaphore signalled from the SIF DMA end callback) and every fileio wrapper (`sceOpen` …
  create, wait, delete). An HLE that completes RPCs synchronously can skip them.
- libkernl: two locks in `0x11c040` (init 1, max 1); top-thread queue semaphore (max `0xff`).
- `sceMcInit` (`0x339a78`): one lock (1/1). libcdvd `0x33b258`/`0x33b5f0`: command
  completion semaphores. FMV `0x383590`: one (1/1).
- Alarms (`SetAlarm`, syscall 252 via the `_InitSys` patch): libmc `0x33a430` (handler
  `0x33a408`) and libcdvd `0x33b258` (handler `0x33b230`) — RPC timeouts.

### 4.3 Interrupt handlers

| Cause | Handler | Installed by | What |
|---|---|---|---|
| DMAC ch5 (SIF0) | libkernl | `sceSifInitCmd` `0x1157a0` | SIF command dispatch |
| INTC 2 (VBLANK start) | `0x274020` | game `0x273280` (boot step 18) | `++*(int*)($gp−0x12ec)`; the game's frame pacing and `0x273ff0` (wait one vblank) spin on this counter |
| INTC 2 | `0x3b1c08` | RW sky2 `0x3b1e48` | sky2 vsync/flip |
| DMAC ch1 (VIF1) and ch2 (GIF) | `0x3b1118` (same handler for both) | RW sky2 `0x3b1e48` | DMA chain completion |
| INTC 2 | `0x3809f0` | FMV `0x3812e0` | FMV display |
| DMAC ch2 (GIF) | `0x3809a0` | FMV `0x3812e0` | FMV image upload |
| DMAC ch3/ch4 | — | libipu (`EnableDmac` only) | IPU from/to |

`sceGsResetGraph` writes `GsPutIMR(0xff00)` and `SetGsCrt`; `sceGsSyncV` uses
`SetVSyncFlag`/the VSync syscalls.

**Busy-waits a runtime must make progress through** (no syscalls inside the loop):
`main` waiting for 100 vblanks (`0x273fa4`), `0x273ff0` (wait for the next vblank; used by
file-read retries and audio init), all `sceSifCheckStatRpc` spins (`0x23acd8`, …,
`0x239448`), the CDSTREAM bind spin (`0x3368c0`). Interrupt handlers therefore have to run
asynchronously (another host thread or polling at loop back-edges).

## 5. File access

### 5.1 Layers

1. **CD file cache + CDSTREAM** (everything the game itself reads, H): path → LSN via
   `sceCdSearchFile`, data via CDSTREAM channel 4 in 2048-byte sectors.
2. **libcdvd streaming** (`sceCdSt*`, N command 9): FMV only.
3. **fileio** (`sceOpen`/`sceRead`/`sceLseek`, `0x80000001`): FMV for non-`cdrom0:` paths,
   libscf reading `rom0:ROMVER` (`0x389e28`), and the RW file-system `fflush`/`fclose` paths
   (`sceWrite` when a write buffer is pending — not used at boot). `sceCdRead` is not linked.
4. **Memory card** (libmc / mcserv).

### 5.2 The game's RW file system (installed by `0x131980`, H)

| RW slot | Function | Behaviour |
|---|---|---|
| fexist | `0x132150` | open + close through the same table |
| fopen | `0x132250` → `0x132280` | name → `<root> + name`, upper-cased, `/` → `\`, `;1` appended (`0x1323d0`); looked up in the file-position cache (`0x4bd0e0`, 200 × 48-byte entries: name, LSN at +0x28, size at +0x2c; count `$gp−0x1ca8`); on a miss loops `sceCdDiskReady(0)` + `sceCdSearchFile` (prints `Cannot find %s on CD`). Allocates a 16 KB-buffered file object (`+0x4000` LSN, `+0x4004` position, `+0x4008` size, `+0x400c` buffer offset) |
| fread | `0x131ea0` | sector-aligned bulk reads straight into the destination (`CdStreamRead(4, dst, lsn+pos/2048, n)`), otherwise refills the 16 KB buffer with 8 sectors; `CdStreamSync(4)` after each read |
| fgets / feof / fseek / ftell / fflush / fclose / fwrite | `0x131be0` / `0x131b20` / `0x131d20` / `0x131b70` / `0x131ae0` / `0x1321b0` / `0x131e90` (no-op) | |

`CFileMgr` (`0x25f780`–`0x25fb10`) calls these through `RwOsGetFileInterface()` (`0x39b9a0`);
its root directory string lives at `0x4bd0a0` (`CFileMgr::SetDir` `0x132490`).

### 5.3 Paths used before the first frame

- IOP: `cdrom0:\SYSTEM\IOPRP241.IMG;1` and the 8 IRX paths of §2 (via loadfile, i.e. the
  IOP's own `cdrom0:` device).
- File-position cache (boot step 18): 117 paths, listed at `0x437510–0x438580` (the
  `\ANIM`, `\MODELS`, `\TEXT`, `\DATA`, `\DATA\MAPS\…` and `\TXD` files named in §1.2).
- `ValidateVersion`: `models\coll\peds.col`. Text: `TEXT\<LANG>.GXT`. Fonts:
  `MODELS/FONTS.TXD`. `DATA\GTA_VC.DAT` and `DATA\SURFACE.DAT` (through `CFileMgr`).
- CD images for streaming: `CdStreamAddImage("MODELS\GTA3.IMG")` from `CGame::Initialise`
  `0x26fff0` (inside step 21); `CStreaming` (`0x2e7810`, from `0x2e7bd0`) resolves every
  image's LSN/size with `sceCdSearchFile` (`0x25f6c0`).
- Movies: `cdrom0:\MOVIES\VCNTSC.PSS;1`, `cdrom0:\MOVIES\VICE.PSS;1` (libcdvd streaming).
- `rom0:ROMVER` (libscf, fileio).

### 5.4 Memory card at boot

`sceMcInit` (step 13) binds mcserv and checks the mcserv/mcman versions (fno `0xfe`). The
boot check `0x409570` (step 15) then uses port 0: `sceMcGetInfo` + `sceMcSync` (presence,
`Memory card %i present/not present`), `sceMcChdir` + `sceMcGetDir` (save directory listing,
`PopulateFileTables`), and may `sceMcOpen`/`sceMcRead`/`sceMcClose` save files and
`sceMcDelete` stale ones. Save icons are `icon.sys`, `vcicon.icn`, `vcicon2.icn`
(`0x409e00`). `sceMcSync` polls `sceSifCheckStatRpc`, so all mcserv calls are NOWAIT.

## 6. Other runtime requirements before the first frame

- **Memory**: the game assumes 32 MB. The main heap ends at `0x1ffa800`; the main thread's
  stack is the top `0x4000` bytes of RAM (`SetupThread` with stack `-1`).
- **Kernel patches**: `_InitSys` installs kernel-mode code from `.data` (`0x42de78+`) with
  `SetSyscall`; the runtime must instead provide the alarm syscalls (`SetAlarm` 252 and the
  `0xFC/0xFE` handlers), `iWakeupThread`-style requests and the TLB helpers (`MODULES.md`).
- **Function-pointer-only entry points**: 210 code entries are reached only through
  pointers and were not functions in the original Ghidra list (so they are absent from
  `out/functions.csv` and the first `recomp_out/`). Boot-critical examples: the idle thread
  `0x273270`, the vblank handler `0x274020`, the DTS RPC thread `0x239330`, the FMV thread
  `0x3812c0`, libmc RPC end callbacks `0x33a0d8`/`0x33a558`, crt0's thread root `0x1000c0`
  (`ExitThread`), RenderWare engine/plugin constructors and destructors registered with
  `RwEngineRegisterPlugin` and the `Rp*RegisterPlugin` family (e.g. `0x39b7a0`/`0x39b7b8`,
  called during `RwEngineOpen`/`CGame::InitialiseRenderWare`), and destructors registered by
  the static constructors. They are now functions in the Ghidra project
  (`tools/ghidra/CreatePointerFunctions.java`, restricted to the recompiler's `code` ranges)
  and listed in `out/pointer_entries.csv`. A further 358 orphan addresses are referenced only
  from data; they are switch jump-table targets of functions Ghidra did not bound and were
  deliberately not turned into functions.
- **GS**: two video inits — FMV (`sceGsResetGraph(0,1,2,1)`, 640×240 double buffer with
  `sceGsSetDefDBuff`) and RW sky2 (`sceGsResetGraph` + `sceGsSetDefDBuffDc`, GIF DMA of the
  initial GS state). Field/interlace mode is NTSC interlaced.
- **DMA channels**: 1 (VIF1, RW pipelines and VU1 microcode), 2 (GIF, RW and FMV),
  3/4 (IPU, FMV), 5/6 (SIF), plus scratchpad DMA by the DTS encoder (audio init). The sky2
  driver uploads VU1 microcode through VIF1 `MPG` packets at pipeline setup; VU0 is used in
  macro mode by game code and in micro mode by the DTS encoder (`sceDevVu0*`).
- **Pads**: `scePadInit(0)` + `scePadPortOpen` ports 0 and 1 (slot 0) with 256-byte DMA
  buffers at `0x5c0180`/`0x5c0280`.
- **Clock**: `sceCdReadClock` once (seconds/minutes/hours feed `srand`).
