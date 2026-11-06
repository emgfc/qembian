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
| 9.2 exec table built from our handlers; 6 unmapped 9.2 calls are logging stubs returning `KErrNotSupported` (fast: return 0) | `kernel/sexec92.cpp`, `tools/exec92/exec92.map` | identify `F0a`, `F0b`, `S34`, `Se8`, `Sed` (secure time) and implement |
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
| **H-lcd-native-landscape:** the N95 panel is natively landscape and wsini.ini gets the 240×320 UI through rotation 270 done by the display controller (`DisplayDriver` claims hardware rotation). `lcd_n95` reports the native size 320×240 through HAL. The frame buffer holds the picture in the current WSERV rotation (240×320 at index 3, 320×240 at index 0/2, which the screensaver uses) with 1280-byte lines at every rotation (ctl 31 and HAL line offset, as on the device; patch qemu-baseport 0013). `d_display` writes the Syborg fb ORIENTATION register = (3 − index) quarter turns clockwise and QEMU rotates the picture onto the portrait screen. Direction at index 0 (landscape top at the screen's left) is [inferred] from the screensaver's band coordinates; flip it in `SetFbOrientation` if the real phone shows it the other way. Before 2026-11-06 the pitch was 960 and memory was always shown portrait, so the screensaver's landscape picture was clipped to 240 columns | `fb/syborg_fb.cpp` (patches qemu-baseport 0003, 0013), `d_display.cpp`, `src/qemu-syborg/hw/arm/syborg_fb.c` | VRFB/DMA copy model; DSB at rotation 0/2 |
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

## N95 hardware stand-ins (src/n95compat/stubs)

| Device / file | Hack | Real implementation |
|---|---|---|
| `nokiaiscdriver.ldd`, `nokiaiscdriverextension.ldd` (modem link) | fake modem `iscpeer.inl`: answers a fixed reply table (MTC NORMAL, no SIM: `SIM_STATUS_RESP` 0x06 SIM_SERV_SIM_DISCONNECTED and `SEC_STATE_RESP` 0x05 SEC_STARTUP_OK, `MTC_STATE_REQ` 0x01 (e.g. target 0x0A RF_INACTIVE = Offline profile) → `MTC_STATE_RESP` 0x64 status OK, the values NokiaDSY maps to "no card" and "security OK" (0x808bfb54, 0x808bfd20), product profile all zero, network/GSS reads); unknown messages get no reply; connection and flow-control notifications parked forever | a proper fake CMT with state; NET reg status reply removed because the proposed layout crashed NOKIATSY (`IsiMessage: OFFS -9`) |
| ISC ctl 21 channel info | returns `6C <channel>` (guessed) | real value unknown |
| ISC ctl 22 max data size | returns 4096 (guessed; 0 made DATAPORT leave) | real value unknown |
| ISC indications | `MTC_STATE_INFO_IND(NORMAL, START)` only to the subscribing channel itself, on subscribe and after a state query; no fan-out to other channels | proper per-subscription fan-out |
| `xbus.ldd`, `iscisimultiplexer.ldd` | load-only | n/a |
| `custom.ldd` (`Custom`) | controls 32/2/1 return 0 (normal startup mode); everything else returns 0 untouched | real startup reasons, power/reset controls |
| `sosadlserver.ldd` | channel accepts everything | unknown purpose |
| `d_display.ldd` (`DisplayDriver`) | follows the real driver (docs/nokia-d_display.md §4): caps as on the device except +8 (DSB, kept 0 so ScDv uses heap memory), +24/+28 (current size, left 0) and +52 (64K off: our fb is 32 bpp only); ctl 10 screen 0 only; requests 0x8001/0x8002/0x8003 validate their argument; req 0x23 stays pending; **partial display (ctl 3/4/5) drives the Syborg fb PARTIAL_* registers**: the band is in native panel rows, so it is a band of picture rows at rotation index 0/2 (reversed at 2) and of columns at 1/3 (rotation tracked from req 0x8003, re-applied when it changes; WSERV switches to index 0 before the screensaver); masked in picture coordinates before QEMU rotates the picture; outside the band black, 8 colours unless full colour; ctl 9 (screen update) a no-op because the fb is live | TV-out/posting/DSB controls still return 0 instead of KErrNotSupported; ctl 0x2b–0x2d (freeze) ignored |
| `d_display.ldd` DSB (hack H-dsb) | `CDirectScreenBitmap` buffers (caps word 2 = 1, ctl 23/24/25, req 0x2003/0x2004): one buffer pair in physical RAM mapped user-readable at a global address; EndUpdate copies the posted buffer into the frame buffer in the vsync DFC and completes then (60 fps pacing, verified with `src/n95apps/dsbtest`). One DSB at a time (device-wide), portrait coordinates only, no DSA region check | DMA/VRFB transfer, per-rotation coordinates, posting/TV-out |
| fake modem `PN_LIGHT` (0x3A) (hack H-light) | `3A 22` (target info, from `SALightsService.01.dll` on behalf of HWRMServer) → `3A 23` `[status 0][count][request's 0x16 subblocks echoed]`; any other `3A xx` → `xx+1` echoing the request data with byte 0 = 0 | Without it HWRMServer blocks in construction on its SAServer request `0x2000000A`, never registers `!HWRMServer`, akncapserver spawns a duplicate and never rendezvouses → Starter waits forever. Real: a light model (brightness per target, on/off) |
| fake modem energy management `17 0A`/`17 08` (hack H-em) | `17 0B` = `[01][2 sb][16 04 07 07][15 04 01 00]` (battery 7/7 bars, charger not connected; NOKIATSY and SAEmService both find their subblock by id); `17 09` = `[01][1][13 04 01 00]` | real battery/charger model |
| fake modem NokiaDSY start-up self-test: `35 02`→`35 03 [00 00]`, `35 00`→`35 01 [00 00]`, `59 42`→`59 43 [00 00]` (status passed; docs/n95-startup-animation.md §5.3). Without it Startup never gets DOS event 1 and parks in state 2 | `src/n95compat/isi/isi-db.txt` | modem self-test model |
| fake modem RTC status `19 0A` (from NokiaDSY for DosServer request 0x130, `SysStartup::RTCStatus()`) → `19 0B` `[01][1 sb][04 04 01 00]` = RTC valid (NokiaDSY 0x808bbaa8: `[10]` must be 1, subblock 0x04 byte 2: 1/3 valid, 2 invalid). Unanswered, CommonDSY timed out with -2, and Startup asked for the date and time on every boot (from its 01/01/2008 default) | `src/n95compat/isi/isi-db.txt` | report "invalid" when the RTC offset file is missing, if a first-boot-like time query is ever wanted |
| fake modem security `08 01` (phone-lock code state, from NOKIATSY for Autolock) → `08 02` `[state 0]` = lock disabled (NOKIATSY 0x808c8b7c: [10] 1/3 locked, 2 enabled, else unlocked/disabled). Unanswered, the query repeated and Autolock showed "operation time-out" every minute or two | `src/n95compat/isi/isi-db.txt` | a security model (lock code on/off) |
| fake modem light control `47 06` → `47 07` status OK (inferred resource for SALightsService's on/off control; unanswered it blocked SysAp via HWRM) | `src/n95compat/isi/isi-db.txt` | real light model |
| fake modem `INFO_SERIAL_NUMBER_READ` (`1B 00`→`1B 01`) | replies `[INFO_OK][1 sb][0x41 IMEI_PLAIN len 20: 0, 15, "490154203237518", 0]`; the IMEI is the well-known documentation example, the layout follows the reference ISI headers, **not** verified against NOKIATSY's parser (hack H-imei) | RightsServer asks ETel for the phone id after DRM init; unanswered it waits forever. If NOKIATSY panics `IsiMessage: OFFS`, check subblock layout |
| `secldd.ldd` (`SecEnv`) | request 2 ("wait for next job") stays pending; request 6 (SecEnv.dll export 17, "process data in a secure-environment app", used by DrmBb5KeyStorage to wrap/unwrap DRM keys) is an **identity transform**: input copied to output, out length = in length. **Exception (hack H-secenv-cert):** mode 3 with empty input is DrmBb5KeyStorage reading the device certificate chain (output = `[TUint32 LE len][DER cert]` records, the last one X509-parsed and SHA-1 hashed); the stub returns a self-signed RSA-1024 `CN=N95-stub-device` cert generated with openssl (`src/n95compat/stubs/stubcert.h`, no firmware bytes). An empty output made `FUN_0000823a` index the record array at −1 → RightsServer `USER 130`. Requests 3/4/5/7/16 complete `KErrNotSupported`; others complete `KErrNone` | no secure environment exists; DRM-protected content will not decrypt. Failing request 6 made RightsServer exit −5 → DRMHelperServer panicked → AknSkinSrv → EikSrvs → AknCapServer → Starter all waited forever |
| `ehwa.ldd` (`Hwa`) + `ehwaomap.pdd` (`Hwa.Omap`) — TI DSP/BIOS bridge (hack H-dspbridge) | no DSP: answers what MmfAudioServer needs (docs/nokia-dspbridge.md); per-node message exchange (EAP nodes acked `{0,0,0}`). **Playback** (docs/nokia-dsp-playback.md): nodes tagged by UUID (raw PCM, MP3, AAC, eAAC+); `0x800000e` blocks copied to a 128 KB physically contiguous ring and played by the QEMU `syborg-audio` device (PCM; MP3/AAC decoded by FFmpeg); 10 ms pump DFC paces the client; end of stream / stop → `{8}` (raw, eAAC+) or `{4}` (MP3, AAC). Raw PCM node = **44.1 kHz stereo** (measured). Status 2026-11-05: startup jingle plays correctly on the experimental ROM; key tones still refused before the DSP (audio route `77/01` now answered, not yet verified); MP3/AAC node path written but untested | a DSP-side model of more node messages if needed |
| `dvfs_interface.ldd` (hack H-dvfs) | every DoRequest (performance level) completes `KErrNone` at once; MmRmServer leaves without the driver | a power model, if ever needed |
| `LightLdd.ldd` (`LightDrv`) + `LightPdd.pdd` (`LightDrv.pdd`) — backlight driver (hack H-lightdrv) | LDD accepts every control and completes requests at once; PDD load-only | without them SALightsService (SAServer) leaves −5 setting up a light target, HWRM never creates its light plugin and SysAp shows "Feature not supported". Real: backlight model (brightness/on-off per target), could drive the screensaver/power-save display |
| `medrofs.pdd` | ROFS1 + ROFx served read-only from memory appended to the ROM | fine |
| `mednvmem2.pdd` (E:) | the baseport NVMemory media driver built a second time (patch qemu-baseport 0006 makes its instance parameters overridable): second QEMU NVMemory in the unused NAND slot `0xc000d000`, irq 14, `EFixedMedia3` (2 is MEDROFS), local drive 2, removable. ESTARTCOMP maps `E: 2 ... FS_FORMAT_CORRUPT` (the N95 maps its memory card E: to local drive 1, which is our C:) | a real MMC/SD model if card insert/remove matters |
| `lcd_n95` | see BSP | |

## ROM / configuration (rom/hybrid, tools/romcompose.py)

| Hack | Real implementation |
|---|---|
| `Z:\sys\data\ESTARTCOMP.TXT` replaced: D: = local drive 0 (IRAM), C: = 1 (NVMemory, 64 MB FAT16), E: = 2 (second NVMemory, memory card, 1 GB FAT32), Z: = composite with ROFS on 8; no Y: | fine; drive images are persistent with `scripts/run-gui.sh` (build/n95/drive-c.img, drive-e.img), host access with `scripts/n95-drive.sh` (mtools) |
| N95 kernel side dropped wholesale; our kernel files placed below the N95 user window | n/a |
| Paging off: the core ROM is flattened | n/a |

## sydb dev bridge (tools/sydb, src/n95apps/devagent)

| Hack | Real implementation |
|---|---|
| ~~H-sydb-autostart~~ **retired 2026-11-06**: was `dafs.exe` copied to `C:\sys\bin\HALSettings.exe` (EStart runs it by bare name). Now the agent is a regular record in Starter's dynamic startup list (`start.dat`), added/removed through StarterServer like a SIS install does (docs/sydb.md "Autostart"); `sydb install` deletes an old HALSettings.exe copy | done |
| devagent.exe is All-TCB, so writes to `\sys\` and `\resource\` go through `dafs.exe mv` (TCB); EKA2 refuses an ALL-caps EXE that links non-TCB DLLs (WSERV, apparc, avkon all lack TCB) | fine as is (a design constraint, not a shortcut) |

## User-side code patches (rom/hybrid/code-patches.txt, `romcompose.py --patches`)

Byte patches on N95 binaries in the composed ROM; original bytes are verified at compose time.

| Hack | Where | Real implementation |
|---|---|---|
| **H-keysound-nommf:** `blx CMdaAudioToneUtility::NewL` → `movs r0,#0; nop` (2 sites) so the key-sound server keeps a NULL tone player instead of leaving −1 when MMFAudioServer cannot start (no DSP) | `eiksrvui.dll` 0x82f0e366, 0x82f0e9c8 | working audio (MMF with a fake DSP/DevSound). Risk: the tone player's prepare-complete callback dereferences without a check, but it is only invoked by a live player; key tones are silent |
| **H-startuptone-nommf:** `blx CMdaAudioPlayerUtility::NewFilePlayerL` → `movs r0,#0; nop` in `CStartupTone::ConstructL`: no startup-tone player (opening it blocked Startup's main thread in a synchronous MMF call → `ViewSrv 11` after 10 s); `Play()` returns `KErrNotReady` and Startup falls back to a (silent) key sound | `startup.exe` 0x833a93c4; analysis in `docs/n95-startup-animation.md` | working audio |
| H-keysound-nommf (continued): the two tone-play helpers return at once (`bx lr`) | `eiksrvui.dll` 0x82f0ea94, 0x82f0eac8 | working audio |
| **H-splash-behind:** SplashScreen window-group priority literal 1001 → −1000 | `SplashScreen.exe` 0x833a6574 | none needed once startup completes (Startup.exe normally takes over the screen before the splash's 120 s timeout); the splash logo renders correctly since the CLZ fix (AknIconServer); re-check whether this patch is still needed once Startup completes |

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
