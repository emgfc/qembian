# Build log: problems, causes, fixes

Chronological. Each entry gives the symptom, the root cause, the fix, and how the fix was
verified. All fixes live in scripts or the container definition, so nothing is applied by hand.

## Environment facts

- Host: openSUSE Tumbleweed (kernel 7.2.11), x86_64, 16 cores, podman 6.1.2. The Docker
  daemon was not running, so podman (rootless, `--userns=keep-id`) is used.
- Container: `docker.io/debian/eol:squeeze` **i386** (Debian 6.0.10). Packages come from
  archive.debian.org with `AllowUnauthenticated`, because the archive keys have expired.
  **Risk**: apt cannot verify package signatures. The image does not run network services
  and only the project directory is bind-mounted. Ubuntu 10.04 i386 is not on Docker Hub.
- Tools: gcc 4.4.5, Python 2.6.6, Perl 5.10.1, CSL `arm-none-symbianelf-gcc` 4.4.1
  (2009q3-63), GNU ld 2.19.51, sbs 2.15.2, QEMU 0.9.1-symbian.

## 1. Sourcery G++ 2010q1 unavailable
- **Symptom**: `sourcery.sw.siemens.com` does not resolve, and the CodeSourcery/Mentor hosts are dead.
- **Fix**: a Wayback CDX search found `arm-2009q3-63-arm-none-symbianelf-i686-pc-linux-gnu.tar.bz2`
  (also GCC 4.4.1), sha256-pinned in `sources.lock`. A 2012.03 (GCC 4.6.3) build from archive.org is kept as a fallback.

## 2. QEMU build
- Built unpatched on the first try with GCC 4.4.5 + Python 2.6 + SDL 1.2.
- `fdt support: no` in the configure summary is **harmless**. configure only probes for a system
  `-lfdt` (used by the PPC `device_tree.c`). The bundled `libfdt/` is always linked, and that is
  what `devtree.c` (the `.dtb` machine loader) uses.
- The Python plugins are looked up in `<bios_dir>/plugins` (`python-plugin.c`), so they are installed to
  `share/qemu/plugins` and `-L` points there. No `PYTHONPATH` is needed.
- The `nvmemorydevice` plugin calls `sys.exit()` if `libnvmemmory.so` cannot be loaded, so it is built
  from `model-libraries/` and found via `LD_LIBRARY_PATH`.

## 3. Headless running
- **Symptom**: `Could not initialize SDL - exiting` (DirectFB probe) even with `-nographic`.
  The fork always initialises SDL. Fix: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.
- **Symptom**: the shell is invisible on serial. ESHELL renders on the framebuffer (EWSRV/ECONS), and
  `screendump` is unusable because the Python framebuffer passes `screen_dump=NULL`. VNC is `#if 0`'d out.
  Fix: `boot-test.sh` runs SDL against **Xvfb** and grabs screenshots with `xwd | convert`.
- **Symptom**: typed `\` arrives as `#`. The Syborg keymap is UK (`KConvertCode[0x2b]=EStdKeyHash`,
  `[0x56]=EStdKeyBackSlash`). `xdotool type` did not honour an xmodmap remap, so `\` is
  sent as keysym `less` (X keycode 94 → scancode 0x56). [verified: `dir z:\sys\bin\e*.exe`]

## 4. Raptor's bundled Python reports `sys.platform == 'linux7'`
- **Symptom**: `raptor_data module loaded on an unrecognised platform 'linux7'`.
- **Cause**: Raptor builds its own Python 2.7 from source. Its configure derives `MACHDEP` from
  `uname -r`, and the host kernel is 7.x.
- **Rejected fix**: presetting `MACHDEP=linux2`. That skips the configure block that sets
  `ac_sys_system`, so `LDSHARED` becomes bare `ld` and every extension module fails to link
  (`undefined reference to memset`, `No module named _struct`).
- **Fix**: `/usr/local/bin/uname` in the container reports release `2.6.32`, the same effect as
  `setarch --uname-2.6`, which squeeze's util-linux lacks. All 2010-era configure scripts
  now see a plausible kernel.

## 5. libgcc lacks the ARM EABI integer helpers
- **Symptom**: 26 kernel-side link failures: `undefined reference to __aeabi_idiv / __aeabi_uidiv /
  __aeabi_ldivmod / __aeabi_uldivmod / __aeabi_idivmod` (in EKERN, MMC, paging and sampler drivers).
- **Cause**: GCC's `config/arm/t-symbian` deliberately omits these from libgcc, because on Symbian
  the RVCT runtime normally supplies them. CSL 2009q3's members `_divdi3.o`/`_udivdi3.o` are
  **empty**. GCC_SURGE links kernel-side code with `-lgcc` and relies on the toolchain providing
  them; CSL 2012.03's libgcc does.
- **Fix** (`scripts/fix-toolchain-libgcc.sh`, run for both multilibs and idempotent from `libgcc.a.orig`):
  build the helpers from the **GCC 4.4.1 sources matching the compiler** (sha256-pinned):
  - `lib1funcs.asm` with `L_udivsi3 L_divsi3 L_umodsi3 L_modsi3`
  - `lib1funcs.asm` with `-U__symbian__` for `L_aeabi_ldivmod L_aeabi_uldivmod L_aeabi_lcmp
    L_aeabi_ulcmp L_lshrdi3 L_ashrdi3 L_ashldi3`, because these are compiled out for `__symbian__`
  - `bpabi.c` (`__gnu_[u]ldivmod_helper`)
  - `libgcc2.c` with `L_udivmoddi4 L_divdi3 L_udivdi3 L_muldi3`, compiled through a minimal target
    shim (`scripts/libgcc-shim/tm.h`, ILP32/LE plus the `bpabi.h` `__aeabi_lmul` rename).
    The shim's type model was checked by building the same routines for i386 and comparing them
    against native division: **5,000,000 random cases, 0 mismatches**
    (`scripts/libgcc-shim/test/divtest.c`).
- **Follow-on 1**: `multiple definition of __aeabi_idiv0/__aeabi_ldiv0`. The kernel defines both
  (`common/arm/gccehlp.cpp` calls `FAULT()`), and user side gets them from `drtaeabi.dll`. So
  `_dvmd_tls.o` is **not** added. But GCC 4.4.1's division code calls the legacy `__div0`, so a
  2-instruction `__div0 → __aeabi_idiv0` tail-call forwarder is added (`libgcc-shim/div0.S`).
- **Follow-on 2**: `elf2e32 : Error: E1035: Undefined Symbol __aeabi_llsr`. EKERN's frozen
  `ekernu.def` **exports** `__aeabi_{lmul,llsl,llsr,lasr,lcmp,ulcmp}` for kernel-side DLLs. Under
  RVCT, `armcchlp.cia` forces them in. Providing them in libgcc (above) fixes it.
- **Verified**: `kernelhwsrv` package builds with `sbs : errors: 0`. The kernel boots and runs
  64-bit maths paths (NTimer, kernel heap setup) without faults.

## 6. Missing `libboost_thread-mt`
- `imgtools/imgcheck` links `-lboost_thread-mt`. Fixed by adding `libboost-thread-dev` (1.42) to the image.

## 7. Small script portability issues inside squeeze
- `/.containerenv` is absent in this setup, so the scripts detect the container with `SYBORG_IN_CONTAINER=1`.
- bash 4.1: no negative array subscripts. coreutils 8.5 `timeout` has no `--foreground`.

## 8. Verified outcome

- Clean rebuild from scratch (`rm -rf build cache`, container image deleted) via `scripts/build-all.sh`:
  **3 min 35 s**, exit 0 ([log](evidence/clean-build.log)). This excludes the one-time
  downloads (about 2 GB, which were cached).
- `kernelhwsrv`: `sbs : errors: 0`. Syborg BSP + ROM: `sbs : errors: 0`, 124 warnings
  (mostly unfrozen exports and `-Wno-*` noise).
- ROM `syborg_tshell_ARMV5_udeb.img`: 4,890,624 bytes, 68 files. The RVCT-built PDK ROM is 2.1 MB;
  GCC udeb code is larger.
- Boot: ESHELL `0.01(3049) CFG=UDEB` on the framebuffer. The PDK fixture shows `(4004)`, which
  confirms our image is a different build. Commands work (`dir`), and a second process
  (`btrace.exe`) starts in its own console.
- Serial with `kerneltrace 0x80000003`: about 12k lines covering variant init, MMU (multiple memory
  model), nanokernel and timers, kernel heap, then loader → ESTART → domainSrv → EWSRV → ESHELL.
  No faults or panics.
- gdb (host, multi-arch 16.3) → QEMU gdbstub: stops at the reset vector, and a hardware breakpoint
  on `_E32Startup` (0x80003130) hits after the MMU is on.

---

# Option 1 (E1): our kernel + 9.2 exec personality + N95 user side

The plan is in [roadmap.md](roadmap.md) and the firmware facts in
[n95-firmware-inventory.md](n95-firmware-inventory.md). To reproduce:
`scripts/n95-inventory.sh` once, then `scripts/hybrid-boot.sh [--no-build] [--secs N] [--trace MASK]`.

## E1.1 ROM composer (`tools/romcompose.py`) [verified]
- **Layout:** in the N95 core ROM, all 84 kernel-side images live below `0x8038f9a0` and all
  2,635 user-side files above it. Kernel side means a primary/variant/extension/device flag, or
  imports from EKERN. The composite takes our ROM's bytes for `[0x80000000, 0x8038f9a0)` (header,
  Syborg bootstrap, our 23 kernel-side files ending at `0x802d5ff4` in udeb) and the N95 flat image
  verbatim above that.
- **New structures:** a new root directory (with `TRomDirSortInfo` tables, case-insensitive UTF-8
  order as in rombuild) and a merged exception search table (2,624 entries, used by the RVCT
  unwinder through `UserSvr::RomHeaderAddress`) go after the N95 image.
- **Header patches:** size, root, `iSecondaryFile` = N95 `efile.exe`, and paging/compression off.
  Our primary/variant/extension `TRomEntry` chain still points into our original directory, which
  is intact inside our window.
- **Verified:** all 2,635 N95 files are byte-identical at their original addresses. The Syborg
  bootstrap takes the ROM size from `TRomHeader`, so a 60 MB image needs no BSP change.
- **Data overrides** (`--file ROMPATH=LOCAL`) are for board configuration only. Currently that is
  `rom/hybrid/ESTARTCOMP.TXT`.

## E1.2 9.2 exec tables in the kernel (`patches/kernelhwsrv/0001-exec92-personality.patch`) [verified]
- **Mechanism:** each `NThread` carries its own fast/slow exec table pointers (`sthread.cpp`). When
  `TRomHeader.iKernelConfigFlags` bit 29 is set (`romcompose.py --exec92`), `Exec92Init()` builds
  `EpocFastExecTable92`/`EpocSlowExecTable92` at boot. It copies entries (handler and flags) out of
  the native tables per `exec92_map.h`, which `tools/exec92/gen_kernel_map.py` generates from the
  reviewed `tools/exec92/exec92.map`. Every user thread then gets the 9.2 tables. The native ROM is
  unaffected: the regression boot to ESHELL passes.
- **Map:** 28 fast + 238 slow 9.2 calls. 6 map to per-number logging stubs (calls with no caller
  in the N95 EUSER). Shims so far: `SafeInc32`/`SafeDec32` (9.2-only fast calls),
  `MutexWait92` (no timeout argument in 9.2), `HandleDuplicate92`.

## E1.3 boot runs
| Run | Result | Cause | Fix |
|---|---|---|---|
| 1 | kernel fault in `umemget32`, DFAR `0x4bd20001` | `S6d HandleDuplicate`: 9.2 passes the source handle **by value** in r2 and returns the new handle. Ours takes `TInt&`, so the kernel dereferenced the handle. Found from the user LR → N95 `RHandleBase::Duplicate`. | shim `HandleDuplicate92` |
| 2 | `efile.exe` "Fserv fault 162" → KERN 4 | N95 `SetSystemDrive` check: C: must be local/internal NAND/flash/disk. N95 `Z:\sys\data\ESTARTCOMP.TXT` maps C: to local drive 6 (N95 NAND); Syborg has drives 0 (IRAM) and 1 (NVMemory). | override ESTARTCOMP.TXT: D:=0, C:=1 |
| 3 | silent hang, all threads idle in the trace | gdb PC sampling (`run.sh --gdb-nowait`) showed EStart spinning in `TLex16::NextToken`/`SkipSpace`: the override had LF line endings, and EStart only splits on CR LF | CRLF (`rom/hybrid/.gitattributes`) |
| 4 | EStart panic "NokiaISCApi 1" → KERN 4 | `IscApi.dll` loads `xbus.ldd`, `iscisimultiplexer.ldd`, `nokiaiscdriver.ldd` and `nokiaiscdriverextension.ldd` (all N95 kernel side, dropped) | stand-in LDDs (`src/n95compat`); `NokiaISCDriver` completes async init (control 0), the rest is silent |
| 5 | EStart panic `custom.ldd -1` | EStart's boot flow: `Custom` channel controls 32/2/1 (boot mode) | `custom.ldd` stand-in answers 0 (normal start); `sosadlserver.ldd` load-only |
| 6 | WSERV-INTERNAL 13 (wsini.ini not found, `KErrPathNotFound`) | `wsini.ini` and much more live in ROFS (Z: composite). ROFS1 (`SOS+ROFS`) + ROFx (`.v13` → `SOS+ROFX`) form one partition; ROFx sits at ROFS1's `iMaxImageSize` (0x1700000, the same distance as on the N95 flash) | `romcompose.py --rofs` appends the blob and a ROM trailer; `MEDROFS.PDD` serves it as NAND/ROFS local drive 8; Z: line from the N95 `ESTARTCOMP.TXT` restored |
| 7 | efile LoaderThread panic E32USER-CBase 46 (stray signal) | New `REQC` trace: the kernel completed a loader `TRequestStatus` when MC_info died. Our `ProcessCreate`/`ProcessLoaded` read and write `TProcessCreateInfo` with 3 extra Symbian^3 fields (`iDebugAttributes`, `iDestructStat`, `iFlags`); 9.2's struct is 12 bytes shorter, so loader stack garbage became a destruct notifier (and paging flags) | handlers use the 9.2 size for 9.2 threads (`Exec92ProcessCreateInfoShrink`) |
| 8 | kernel fault: undefined instruction in user mode (`DFPAEABI.dll`) | N95 floating-point runtime uses VFP; Syborg's `variant.mmh` enables `__CPU_HAS_VFP` only `#ifndef GCCE` | VFP enabled for GCCE (`patches/qemu-baseport/0002`); compiles clean; text-shell regression passes |
| 9 | WSERV KERN-EXEC 3 | New `IPCRD` trace (server reads of client descriptors): `r_display.dll` → `scdv.dll` (screen driver) needs `d_display.ldd` (`DisplayDriver`) | `d_display.ldd` stand-in: select screen (10), 104-byte capabilities (11) |
| 10 | WSERV KERN-EXEC 3 (array index −1) | gdb: `NewScreenDeviceL` leaves −5 for every mode. Caps offsets: +56/+60/+64 = EColor16M/16MU/16MA (9.2 numbering); scdv maps HAL mode index 0/1 = 64K/16MU; Syborg LCD reported mode 2 of 3 at 24 bpp | caps +60; `lcd_n95` (Syborg LCD rebuilt with 32 bpp, 2 modes, current 1 — `patches/qemu-baseport/0001`); 240×320 DTB (`tools/dtb_set.py`, `SYBORG_DTB`) |
| 11 | WSERV-INTERNAL 13 | wsini `SCR_ROTATION1 270` needs rotation 270 in the orientation mask (`r_display` caps +36/+40/+44 = 90/180/270) | rotations declared |
| 12 | silent stall after WSERV init | WSERV waits on display request 0x8003 (`DoRequest` + `WaitForRequest`) that the stand-in left pending | display stand-in completes requests at once |
| 13 | no fault; `Starter.exe` and `DosServer.exe` run; screen is a uniform 240×320 framebuffer | DosServer loads `CommonDSY.dll`/`NokiaDSY.dll` and opens ISC unit 24; Starter presumably waits for the DOS/modem state (MTC) that the silent ISC peer never sends | fake modem peer (below) |
| 14 | `C32-fault 0` in `c32exe` CCommServer → C32Start reboots | `DATAPORT.CSY` (ROFS, RAM-loaded at 0x70014000) opens ISC channel 46, registers a name (`db 05`, answered `db 06`), then leaves inside its port open; the unwinder finds its exception descriptor, then the comms server faults | `tools/rofs.py` + `tools/e32load.py` (Symbian deflate, relocation, import resolution → ELF) + Ghidra: DATAPORT sizes reads with IscApi export 10 = ISC **ctl 22, the channel's maximum data size returned as the result**; our peer returned 0 → zero-length read → `KErrArgument` → `User::Leave` (confirmed with a gdb hardware breakpoint) | ctl 22 returns 4096 |
| 15 | no fault in 300 s (3.4 M trace lines); Starter's critical list runs: RfsServer, tzserver, DBRECOVERY/EDbsrv, HWRMServer, AccServer, MediatorServer, **SplashScreen**, randsvr, AknIconSrv, ApsExe, **akncapserver, EikSrvs, aknskinsrv**, cdlserver, FepSwitch, RightsServer; the frame buffer is filled dark (first S60 drawing) | RightsServer keeps retrying `secldd` (N95 security LDD, dropped kernel side) | next: SecLdd stand-in; further modem replies (second research agent) |
| 16 | SecEnvInit spins (1.7 M `SecEnv` requests, the boot crawls) | `secldd.ldd` stand-in (channel `SecEnv`) completed every request; SecEnvInit keeps request 2 outstanding as "wait for next job" | request 2 stays pending (`EPendRequest`) |
| 17 | RightsServer `USER 130` right after SecEnv request 6 | secure-environment crypto calls completed with success but no output | requests 3/4/5/6/7/16 complete `KErrNotSupported` |
| 18 | NOKIATSY `IsiMessage: OFFS -9` | modem reply additions from the second research agent ([part 2](n95-isc-isi-protocol-2.md)): the proposed `NET_MODEM_REG_STATUS` reply is shorter than NOKIATSY parses | that reply dropped (nothing waits for it); other additions kept (product profile for FeatMgr/SAServer, network/GSS reads, SIM/permanent-data reads that cost LanguageSelection ~170 s of timeouts); ISC peer now enters a critical section around its mutex |
| 19 | no fault in 420 s, 53 k trace lines; all of run 15's processes plus `DRMHELPERSERVER`; RightsServer restarted twice (crypto unavailable) | | next: find what Starter waits on after the UI servers (SysAp not started yet) |
| 20 | RightsServer `USER 130` after SecEnv request 6 | DrmBb5KeyStorage mode 3 with empty input reads the device certificate chain (`[len][DER]` records); our identity copy returned 0 bytes and `FUN_0000823a` indexed the record array at −1 | dummy self-signed cert chain (H-secenv-cert) |
| 21 | RightsServer: "Great Success!! Crypto PA is used for DRM", then waits on ETel for the IMEI: ISI `1B 00` unanswered | | fake IMEI reply (H-imei) |
| 22 | "DRM BB5 data is OK in this HW"; then `09 1D` (READ_IMSI) unanswered; boot continues: `DcfRepSrv`, `DRMHELPERSERVER`, `MmfAudioServer` start; then **`FAULT: KERN 0x50`** (1400 s; found by `tools/pcsample.py`: guest PC parked in FIQ-mode fault loop) | KERN 80 = `EPermanentThreadExit`: `MmfAudioServer::MmRmServerThread` (system-permanent) exits −1 because `Bal.dll` cannot load `EHWAOMAP.pdd`/`EHWA.ldd` (DSP bridge, dropped kernel side) | IMSI "not available"; H-critical-exit (patch 0003) |
| 23 | Starter now completes its list (`sae`, `SysAp`, `Phone`, `Idle`, `screensaver`, `startup` …) but only because Avkon keeps crashing: MmfAudioServer dies (no DSP) → EikSrvs' key-sound server gets −1 → EikSrvs exits → akncapserver `USER 0` → retried 4× | H-critical-exit lets it continue | DSP bridge stand-ins (H-dspbridge): bridge attach still fails; MmRm also needs `dvfs_interface.ldd` |
| 24 | same loop | `eiksrvui.dll` creates `CMdaAudioToneUtility` per tone | H-keysound-nommf code patch (`romcompose.py --patches`) |
| 25 | EikSrvs and akncapserver stay up; splash dismissed; then idle: Starter waits for akncapserver's rendezvous; akncapserver waits for `!HWRMServer`, which blocks in construction on SAServer request `0x2000000A` → light adaptation ISI `3A 22` unanswered | | H-light reply |
| 26 | one run went silent at 37 k lines (not reproduced); the rerun reached Starter's whole list: `sae`, `SysAp`, `Phone`, `Idle`, `screensaver`, `startup`, `alarmserver`, `aknnfysrv`, `PhoneServer`, …; SplashScreen relaunched ~11×; **screen still the uniform dark fill** | SAEmService leaves −2 on our GSS-style `17 0B` (needs bars 1..7) | H-em replies; next: why nothing reaches the frame buffer |
| 27 | no display off / secure switch / mode change ever requested (one power-up, fb phys 0x06012000, lin 0xcac02000); gdb cannot read that kernel address | | in-guest frame buffer probe |
| 28 | probe (hash every 60 s): the visible 240×320 frame buffer is **all `0x03030303` for the whole run** — nothing draws into it after the first fill. WSERV's screen driver pushes dirty rects with `DisplayDriver` ctl 9 (`[minX \| w<<16][minY \| h<<16]`, landscape 320×240 panel coordinates), never with pixel data | | find where ScDv's 16MU device draws (gdb on its constructor `0x8087b2ac`) |
| 29 | gdb (`SYBORG_GDB=wait`, hardware breakpoints on ScDv): WSERV probes modes 1–12 through `CFbsDrawDevice::NewScreenDeviceL`; the 16MU device draws at `0xcac02000` (our frame buffer, offset 0). ScDv asks the display driver for **hardware rotation** (ctl 31 = set orientation 3, slot 15 SetOrientation(Rotated270)) and draws in landscape 320×240 coordinates. During the boot WSERV fills the screen **white** (`WriteRgbMulti 0xffffffff`) a few times and blits ~3 full screens of lines (`WriteLine`, 720 calls); the frame buffer still ends up uniform `0x03030303` → the blitted source is dark | | check the blitted source pixels (splash bitmap / AknIcon SVG rendering) |
| 30 | gdb, draw calls classified by device vtable: **screen device** (`vt 0x8087e984`, 16MU at `0xcac02000`) gets only 6 white full-screen `WriteRgbMulti` fills and 480 `WriteLine`s whose source pixels are all 0 (SplashScreen blitting its 240×240 icon, which is empty); FBS 64K bitmaps (`vt 0x8087d83c`) get a few real-colour fills (0x848484, 0x00abea: icons/skin). **Conclusion: the display path works**; the screen is dark because (a) the splash logo (SVG in `splashscreen.mif`, rendered by AknIconServer) comes out empty, and (b) no other window is drawn: the startup sequence never finishes (SplashScreen is relaunched ~11×, Startup/SysAp keep the UI hidden) | | next: Startup.exe/SysAp state (what they wait for), then SVG rendering |
| 31 (current) | | | |

What already works in run 13, all N95 binaries on our kernel ([boot summary](evidence/e1/hybrid-boot-summary.txt)) ([trace excerpt](evidence/e1/hybrid-serial-excerpt.txt),
[loaded code segments](evidence/e1/hybrid-codesegs-loaded.txt), [exec call counts](evidence/e1/hybrid-exec-call-counts.txt)):
- `efile.exe` and its loader thread: code-seg create, load, dependencies, library attach.
- C++ leave/trap through `LeaveStart`/`LeaveEnd`, client/server IPC, drive threads.
- `erofs.fsy`, `ecomp.fsy` and `ELocal.fsy`, with C: mounted as FAT on Syborg NVMemory through our
  `ELOCD`/`MEDNVMEMORY`.
- `EStart.exe`, `halsettings.exe`, `domainSrv.exe` + `domainPolicy.dll`, `SecEnvInit`, `IntegrityCheck(Server)`,
  `centralrepositorysrv`, `EInfoServer`, `LanguageSelection`, `sysagt2svr`, **WSERV** (`EwSrv`) with
  Nokia's own `scdv.dll`/`r_display.dll` on the Syborg frame buffer, **FBSERV**, **ECOM**, the crash
  monitor processes, **`Starter.exe`**, **`DosServer.exe`** (+ `CommonDSY`, `NokiaDSY`), `baksrvs.exe`.
- Code loaded from ROFS into RAM (non-XIP) by the N95 loader.

**Fake modem (run 14):** `src/n95compat/stubs/iscpeer.inl`, built into `nokiaiscdriver.ldd`
(channels 0..31) and `nokiaiscdriverextension.ldd` (32..63), implements the IscApi channel interface
and answers like a modem in NORMAL state with no SIM, per
[n95-isc-isi-protocol.md](n95-isc-isi-protocol.md) (researched by a Fable subagent with headless
Ghidra, `tools/ghidra/decompile.sh`; key claims re-checked against the decompiled IscApi). With it,
DosServer finishes, Starter runs its start list (c32start/c32exe with ETel and NOKIATSY,
FeatMgrServer, mpmserver, SAServer), and LanguageSelection no longer panics. All ISC traffic is
logged ([evidence](evidence/e1/hybrid-isc-traffic.txt)): unanswered so far are NOKIATSY's
resource 0x1b/0x0a/0x17/0xdb requests, SAServer's 0x1b, and LanguageSelection's SIM 0x1f/0x19 reads.

Remaining non-fatal panics: `LanguageSelection` (`StartupLangPlugI -5`), `mc_useragent` (`KERN-EXEC 0`).

Diagnostic tools added on the way: kernel traces `REQC` (request completions, trace bit 24) and
`IPCRD` (what servers read from clients, bit 11) in `patches/kernelhwsrv/0002`; `tools/xipdis.py`
(XIP disassembly with import names); gdb recipes in `build/n95/gdb-*.cmd` patterns (attach to
`run.sh --gdb`, `break` after calls, `set logging`).

Open risks noted along the way:
- **`TCodeSegCreateInfo`:** the 9.2 SDK struct ends at `iClientProcessHandle`, ours appends 32 bytes.
  The N95's process-create fields line up with ours (heap/stack values are right), so the N95 8GB
  struct evidently has the same 32 bytes (paging backport); only `TProcessCreateInfo`'s 12-byte tail
  differed (fixed in run 7).
- **ARMv6 exclusives:** the N95 user side uses `ldrex`/`strex`. Our ARMv5 kernel doesn't clear the
  exclusive monitor on context switch.
- **VFP:** enabled (run 8). The N95 user side is ARMv6 code with VFP; the Syborg CPU model is a Cortex-A8.
