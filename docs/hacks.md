# Hacks registry (option 1 hybrid boot)

Every shortcut taken to get the N95 user side booting on our kernel, listed so it can be revisited
later. Ordered by subsystem. "Real" means what a faithful implementation would do instead.
Keep this file up to date with every new hack.

Note (review 2026-11-05): Nokia's own kernel-side drivers (`d_display.ldd`, `nokiaiscdriver.ldd`,
`custom.ldd`, `LightLdd.ldd`, `secldd.ldd`, ...) are still in the firmware image
(`build/n95/core-flat.img`, dropped only from the composed ROM). The guessed values in the
stand-ins below can be checked against their code (decompile with `tools/syb_sym.py fn`).

## Kernel (patches/kernelhwsrv)

| Hack | Where | Real implementation |
|---|---|---|
| 9.2 exec table built from our handlers; 6 unmapped 9.2 calls are logging stubs returning `KErrNotSupported` (fast: return 0) | `kernel/sexec92.cpp`, `personalities/sym92/exec.map` | identify `F0a`, `F0b`, `S34`, `Se8`, `Sed` (secure time) and implement |
| Shims: `MutexWait92` (timeout 0), `HandleDuplicate92` (by-value handle), `SafeInc32`/`SafeDec32` | `sexec92.cpp`, `arm/cexec92.cia` | fine as is; audit the remaining 9.2 call argument semantics one by one |
| `ProcessCreate`/`ProcessLoaded` use the 9.2 `TProcessCreateInfo` size for 9.2 threads (newer fields zeroed) | `scodeseg.cpp` | same, plus audit other loader structures (`TCodeSegCreateInfo` assumed equal size) |
| `TimerAt92` shim: 9.2 `RTimer::At`/`AtUTC` pass the `TTime` by reference (pointer in r2); ours takes lo/hi by value. Before the shim every absolute timer completed at once (CentralRepository's cache-eviction timer spun at 100% CPU and starved the boot: 150 s → 9 s to the Startup UI phase) | `sexec92.cpp` (patch 0005), `exec92.map` S37 | correct as is; audit the other 9.2 calls that take `TTime&`/`TInt64&` |
| Personality is global (ROM header flag), all user threads get 9.2 tables | `sthread.cpp`, `sinit.cpp` | per-process selection if our own user binaries are ever mixed in |
| Debug traces `REQC` (request completions) and `IPCRD` (server reads of client descriptors) | `sutils.cpp`, `sipc.cpp` (patch 0002) | keep for debugging; udeb only |
| **H-critical-exit:** with the 9.2 personality on, a system-critical/permanent *user* thread (not the file server) that exits is logged `N95HACK: system-… thread … exited` and allowed to die instead of `KERN 80` (`EPermanentThreadExit`) / `ESystemThreadPanic` Exceptions in such threads are handled the same way (patch 0006: the thread is panicked, PC/LR/fault address logged as `N95HACK: exception in system-critical thread …`) | `sthread.cpp` (patch 0003) | stand-ins for the missing drivers so those threads survive. First hit: `MmfAudioServer::MmRmServerThread` dies −1 because `Bal.dll` loads PDD `EHWAOMAP` + LDD `EHWA` (TI DSP/BIOS bridge, driven through `bridgeapi.dll`, ~5 k lines of client API): **no audio** until a bridge stand-in exists |
| ~~ARMv6 exclusive monitor not cleared on context switch~~ **resolved (review 2026-11-05):** the Cortex-A8 build defines `__CPU_ARM_HAS_WORKING_CLREX`; every exception/interrupt entry executes `CLREX` (`nkern/arm/vectors.cia`, `include/nkern/arm/entry.h`) | — | nothing to do |

## Modern QEMU (patches/qemu-modern, src/qemu-syborg)

| Hack | Where | Real implementation |
|---|---|---|
| A32 `CLZ` decoded with the should-be-one fields (bits 19:16, 11:8) ignored, as ARM11/Cortex-A8 hardware does. N95 code (AknIconServer's runtime-copied rasterizer) contains `0xe160201e` (`clz r2, lr` with those fields zero); strict QEMU raised UNDEF and AknIconServer died | `target/arm/tcg/a32.decode` (patch qemu-modern 0001) | fine as is (matches hardware) |

## Board / BSP (patches/qemu-baseport, src/n95compat/lcd)

| Hack | Where | Real implementation |
|---|---|---|
| VFP enabled for GCCE builds (was RVCT-only); no `evfp.dll` | `variant.mmh` (patch 0002) | verify lazy VFP context switching under load; add VFP support code if exceptions appear |
| LCD reports 32 bpp, 2 modes, current mode 1 (N95 numbering: 0 = 64K, 1 = 16MU) | `lcd_n95` build of `fb/syborg_fb.cpp` (patch 0001 macros) | a real mode 0 (16 bpp) if anything switches modes |
| **H-lcd-alwayson:** `lcd_n95` (macro `SYBORG_FB_N95`) logs `N95LCD:` power/secure/mode changes and the frame buffer addresses, and ignores display-off (panel never blanked). The QEMU device itself now renders a blanked/disabled panel like one without backlight: the picture stays, dimmed to `blank-dim` percent (default 25; review 2026-11-05), so dropping this hack is a one-line change once power policy is understood | `fb/syborg_fb.cpp` (patch qemu-baseport 0003), `src/qemu-syborg/hw/arm/syborg_fb.c` | honour display off (screen saver / backlight policy; partial-display mode for the screensaver) |
| **H-lcd-native-landscape:** the N95 panel is natively landscape and wsini.ini gets the 240×320 UI through rotation 270 done by the display controller (`DisplayDriver` claims hardware rotation). `lcd_n95` reports the native size 320×240 through HAL. The frame buffer holds the picture in the current WSERV rotation (240×320 at index 3, 320×240 at index 0/2, which the screensaver uses) with 1280-byte lines at every rotation (ctl 31 and HAL line offset, as on the device; patch qemu-baseport 0013). `d_display` writes the Syborg fb ORIENTATION register = (3 − index) quarter turns clockwise and QEMU rotates the picture onto the portrait screen. Direction at index 0 (landscape top at the screen's left) was inferred from the screensaver's band coordinates and confirmed by the owner (2026-11-06). Before 2026-11-06 the pitch was 960 and memory was always shown portrait, so the screensaver's landscape picture was clipped to 240 columns | `fb/syborg_fb.cpp` (patches qemu-baseport 0003, 0013), `d_display.cpp`, `src/qemu-syborg/hw/arm/syborg_fb.c` | VRFB/DMA copy model; DSB at rotation 0/2 |
| Board is 240×320 via a patched DTB | `tools/dtb_set.py`, `SYBORG_DTB` | fine |
| Rotation caps (+36/+40/+44) = 0/1/1 as on the device (checked against Nokia's `d_display.ldd`, docs/nokia-d_display.md); part of H-lcd-native-landscape (ScDv draws portrait into memory, the "controller" would rotate) | `d_display.cpp` | – |
| ARMv7 `WFI` added to `SyborgWFIIdle` after the ARMv6 CP15 wait-for-interrupt (A8 builds only since H-cpu-arm1136) | `specific/interrupts.cia` (patch qemu-baseport 0004, 0011) | fine as is (not really a hack) |
| **H-rtc:** hardware RTC = QEMU `syborg-rtc` register 4 (UTC seconds since 2000-01-01, host time + an offset). A guest write moves the offset, and `-global syborg-rtc.offset-file=FILE` persists it (`run-gui.sh`: `build/n95/rtc-offset`; delete it to go back to host time). The variant installs an initial-time hook because the default one ignores the RTC on a cold start, and every QEMU launch is one (the clock restarted at the ROM timestamp) | `specific/assp.cpp` (patch qemu-baseport 0007), `src/qemu-syborg/hw/arm/syborg_timer.c` | a real battery-backed RTC model; on the N95 the RTC is the modem's, read through DOS/ISI |
| **H-fastcounter-32k:** the kernel fast counter (`NKern::FastCounter`, `User::FastCounter`) reads a 32768 Hz counter (QEMU `syborg-rtc` register 5) instead of the 1 MHz latch. The N95's `hal.dll` reports `EFastCounterFrequency` = 32768 on its own, so with 1 MHz every app that paces itself with the fast counter measured time ~30x too long and never slept (Gravity Defied ran unthrottled, 3-4x too fast) | `specific/highrestimer.h` (patch qemu-baseport 0008), `src/qemu-syborg/hw/arm/syborg_timer.c` | fine as is (matches the N95's 32 kHz timer) |
| **H-tick-1ms:** nanokernel tick 1 ms (timer load 100 at 100 kHz, `KMsTickPeriod` 1000) instead of Syborg's 10 ms; the N95's `hal.dll` reports `ENanoTickPeriod` = 1000, so `User::NTickCount` ran 10x slow for apps. Boot ~3% slower | `specific/syborg.h`, `specific/syborg_priv.h` (patch qemu-baseport 0009) | fine as is (matches the N95) |
| **H-vsync:** Syborg fb raises VSYNC (INT_CAUSE bit 0) from a fixed-rate virtual-clock timer (`-global syborg-fb.refresh-rate=N`, default 60 Hz; the N95 panel rate is assumed, not measured), plus FRAME_COUNT (17) / REFRESH_RATE (18) registers; it used to come from the host display refresh | `src/qemu-syborg/hw/arm/syborg_fb.c` | measure the real N95 rate (a DSB game's frame time on the device) |
| **H-rofs-flash:** the N95 ROFS partition (ROFS1 + ROFx, 34 MB) lives in a 128 MB read-only flash window at physical 0x60000000 (QEMU ROM region filled by `-device loader` from `<rom>.rofs`, which `tools/romcompose.py` writes and `scripts/run-modern.sh` loads), not in the RAM image; MEDROFS.PDD maps it. Stands for the phone's NAND | `src/qemu-syborg/hw/arm/syborg.c`, `src/n95compat/medrofs/medrofs.cpp` | a NAND/OneNAND model with the real N95 partition layout |
| **H-ram-168:** 168 MB RAM (bootstrap `KHwRamSizeMb`, patch qemu-baseport 0010; QEMU board default) so that free RAM at idle matches the N95 8GB (Nokia: 128 MB, "~90 MB free executable RAM"; measured here 93.7 MB free via HAL `EMemoryRAMFree`, 2026-11-06). More than 128 MB because our kernel keeps the whole 58 MB core image in RAM, while the phone (firmware v20+) demand-pages it from NAND | `bootstrap/syborg.inc` (patch 0010), `syborg.c` | ROM demand paging (needs a paging-capable memory model) and 128 MB |
| **H-cpu-arm1136:** CPU = ARM1136 r0p2 like the N95's OMAP2420 (QEMU `arm1136-r2`, board default; MIDR 0x4107b362; r0p2 inferred from the N810 kernel's ARM_ERRATA_364296). Kernel/BSP built `__CPU_ARM1136__` / `CFG_CPU_ARM1136`, VFPv2 (patch qemu-baseport 0011); the ARMv7 WFI word in the idle loop is left out on ARM1136 (the CP15 WFI halts). QEMU's ARM1136 cache type register was invalid for v6 (ctype 0, kernel FAULT cache_maintenance.cpp 804): set to 0x1D992992 = 32 KB I/D, 4-way, 32-byte lines, VIPT aliasing as N800 kernels report for the OMAP2420 (patch qemu-modern 0002). Same boot times, exceptions and panics as the A8 build; A8 images kept in `build/n95/a8/` (run with `-cpu cortex-a8`) | `variant.mmh`, `config.inc`, `specific/interrupts.cia` (0011), `target/arm/tcg/cpu32.c` (qemu-modern 0002), `syborg.c` | fine as is |

## Device hacks

The N95 8GB's hardware stand-ins, ROM configuration and user-side code patches moved to
[devices/nokia-n95-8gb-rm320/hacks.md](../devices/nokia-n95-8gb-rm320/hacks.md) (same hack ids).

## Stand-ins shared between devices (src/standins)

| Hack | Where | Real implementation |
|---|---|---|
| **H-mdaif-probe** (2026-11-07): the 5730's RAPIDO audio driver `AAMdaifDriver.ldd` ("MdaifDriver", client AAMdaifApi.dll) is a stand-in that returns 0 for every control and completes every request with KErrNone, only to learn the client's protocol from the log; without any driver MmfAudioServer's DSP control thread panics ("MdaifApi Panic 2") and the start-up times out on the key-sound path | `src/standins/aamdaifdriver/aamdaifdriver.cpp` | a mock built from the logged protocol (mock-level research), feeding syborg-audio |
| **H-stub-protocol-guess** (2026-11-07): a stand-in serving several phones picks the phone's protocol variant from what the client happens to pass, not from a declared fact. `ehwa` ctl 0x4000002 (DSP image load): a2 != 0 is taken as the N73's older bridge (image already in the client's buffer, must return 0), a2 == 0 as the N95's (chunk handle as the return value). `custom` uses the interface version the client opens with (1.0 N73 / 2.0 N95): closer to what the real driver negotiates, still an inference. Works for exactly these two firmwares | `src/standins/ehwa/ehwa.cpp`, `src/standins/custom/custom.cpp`, `src/standins/_shared/nokiastub.h` | the device profile declares the variant (e.g. `[board.dsp] bridge = "omap1710"` read by the stand-in from the board config, or per-target stand-in builds) and the stand-in runs that one protocol, refusing the other |

## sydb dev bridge (tools/sydb, src/n95apps/devagent)

| Hack | Real implementation |
|---|---|
| ~~H-sydb-autostart~~ **retired 2026-11-06**: was `dafs.exe` copied to `C:\sys\bin\HALSettings.exe` (EStart runs it by bare name). Now the agent is a regular record in Starter's dynamic startup list (`start.dat`), added/removed through StarterServer like a SIS install does (docs/sydb.md "Autostart"); `sydb install` deletes an old HALSettings.exe copy | done |
| devagent.exe is All-TCB, so writes to `\sys\` and `\resource\` go through `dafs.exe mv` (TCB); EKA2 refuses an ALL-caps EXE that links non-TCB DLLs (WSERV, apparc, avkon all lack TCB) | fine as is (a design constraint, not a shortcut) |

## Known remaining non-fatal failures

- Caught kernel exceptions in `SessionSend`/`SessionSendSync` (`RMessageK::TMsgArgs::ReadDesHeaders`,
  ~500 per boot, mostly AppArc): some clients flag integer arguments as descriptors. Our kernel reads
  descriptor headers eagerly, traps the fault and marks the argument undefined (standard EKA2
  behaviour); the 9.2 kernel read them lazily. Integer access still works; harmless noise.
- `mc_useragent.exe` (Nokia MobileCrash) panics `KERN-EXEC 0`: it loads `mobilecrashdriver.ldd`
  (dropped with the N95 kernel side) and uses the unopened channel. A stand-in would need to capture
  thread deaths to be useful.
- Review 2026-11-05: removing the six user-side byte patches (key sound, startup tone, splash) and
  honouring display-off booted fine in scripted runs, but an interactive run hung after the region
  query (prime suspect: key tones through the fake DSP when scrolling). Reverted; to be redone with
  the audio work.

- `RightsServer.EXE` restarted when SecEnv request 6 failed (identity hack), then panicked `USER 130` on the empty cert chain (dummy cert hack, run 20).
- SIM `READ_IMSI` (`09 1D`) answered "data not available" (`[service, 0x24]`, same as the other SIM reads): the phone behaves as having no usable SIM.
- `SecEnvInit` polls `SecEnv` request 1 every few minutes (timer driven; harmless).
