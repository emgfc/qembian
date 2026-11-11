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
| 9.2 exec table built from our handlers; 6 unmapped 9.2 calls are logging stubs returning `KErrNotSupported` (fast: return 0) | `kernel/sexecpers.cpp`, `personalities/sym92/exec.map` | identify `F0a`, `F0b`, `S34`, `Se8`, `Sed` (secure time) and implement |
| Shims: `MutexWait92` (timeout 0), `HandleDuplicate92` (by-value handle), `SafeInc32`/`SafeDec32` | `sexecpers.cpp`, `arm/cexecpers.cia` | fine as is; audit the remaining 9.2 call argument semantics one by one |
| `ProcessCreate`/`ProcessLoaded` use the 9.2 `TProcessCreateInfo` size for 9.2 threads (newer fields zeroed) | `scodeseg.cpp` | same, plus audit other loader structures (`TCodeSegCreateInfo` assumed equal size) |
| `TimerAt92` shim: 9.2 `RTimer::At`/`AtUTC` pass the `TTime` by reference (pointer in r2); ours takes lo/hi by value. Before the shim every absolute timer completed at once (CentralRepository's cache-eviction timer spun at 100% CPU and starved the boot: 150 s → 9 s to the Startup UI phase) | `sexecpers.cpp` (patch 0005), `exec92.map` S37 | correct as is; audit the other 9.2 calls that take `TTime&`/`TInt64&` |
| Personality is global (ROM header flag), all user threads get 9.2 tables | `sthread.cpp`, `sinit.cpp` | per-process selection if our own user binaries are ever mixed in |
| Debug traces `REQC` (request completions) and `IPCRD` (server reads of client descriptors) | `sutils.cpp`, `sipc.cpp` (patch 0002) | keep for debugging; udeb only |
| **H-critical-exit:** with the 9.2 personality on, a system-critical/permanent *user* thread (not the file server) that exits is logged `SYBHACK: system-… thread … exited` and allowed to die instead of `KERN 80` (`EPermanentThreadExit`) / `ESystemThreadPanic` Exceptions in such threads are handled the same way (patch 0006: the thread is panicked, PC/LR/fault address logged as `SYBHACK: exception in system-critical thread …`) | `sthread.cpp` (patch 0003) | stand-ins for the missing drivers so those threads survive. First hit: `MmfAudioServer::MmRmServerThread` dies −1 because `Bal.dll` loads PDD `EHWAOMAP` + LDD `EHWA` (TI DSP/BIOS bridge, driven through `bridgeapi.dll`, ~5 k lines of client API): **no audio** until a bridge stand-in exists |
| ~~ARMv6 exclusive monitor not cleared on context switch~~ **resolved (review 2026-11-05):** the Cortex-A8 build defines `__CPU_ARM_HAS_WORKING_CLREX`; every exception/interrupt entry executes `CLREX` (`nkern/arm/vectors.cia`, `include/nkern/arm/entry.h`) | — | nothing to do |

## Modern QEMU (patches/qemu-modern, src/qemu-syborg)

| Hack | Where | Real implementation |
|---|---|---|
| A32 `CLZ` decoded with the should-be-one fields (bits 19:16, 11:8) ignored, as ARM11/Cortex-A8 hardware does. N95 code (AknIconServer's runtime-copied rasterizer) contains `0xe160201e` (`clz r2, lr` with those fields zero); strict QEMU raised UNDEF and AknIconServer died | `target/arm/tcg/a32.decode` (patch qemu-modern 0001) | fine as is (matches hardware) |
| TLB for Symbian on ARMv6: per-ASID TLB contexts (an ASID change parks/restores the softmmu TLB and jump cache instead of flushing), DACR writes drop only the entries of domains whose access falls, ARMv6 runs with 4K target pages (1K before, for ARMv5 tiny pages), jump cache 16K entries. QEMU flushed everything on each IPC (ASID change) and twice more around each cross-process copy (the flexible memory model's IPC alias domain). Measured, reviewed and open points in docs/perf.md. Off switches: `SYBORG_ASID_TLB=0`, `SYBORG_DACR_TRACK=0`, `SYBORG_V6_1K_PAGES=1` | `accel/tcg/cputlb.c`, `target/arm/{helper,ptw,cpu}.c` (patch qemu-modern 0005); LTO on, TCG plugins off (scripts/build-qemu-modern.sh) | fine as is if it stays correct (behaves like an ASID-tagged TLB); upstream has no equivalent |
| **H-gl2-highp:** GLES2 shader uploads get `precision mediump float;` defaults rewritten to `highp` (same length, comments skipped; int/sampler/lowp defaults and explicit qualifiers untouched) when the host supports highp floats in both stages (one stage only would split the precision of a shared uniform). GTA III builds world-space positions in mediump vertex shaders; a host that honours mediump (16-bit) snaps dynamic objects at world coordinates ~1000 (the jitter). `SYBORG_GL2_HIGHP=0` in QEMU's environment turns it off for an A/B; glGetShaderSource returns the promoted text | `src/qemu-syborg/hw/arm/syborg_gl2.h` (glShaderSource), `syborg_gl2_precision.h`, `tests/gl2_precision_test.c`, docs/devices/nokia-n8-rm596/gles2-host.md | none needed on a real phone (its GPU runs mediump at fp32 or wider); per-app opt-out if a shader ever depends on mediump wrap-around |

## Board / BSP (patches/qemu-baseport, src/standins/lcd)

| Hack | Where | Real implementation |
|---|---|---|
| VFP enabled for GCCE builds (was RVCT-only); no `evfp.dll` | `variant.mmh` (patch 0002) | verify lazy VFP context switching under load; add VFP support code if exceptions appear |
| LCD reports 32 bpp, 2 modes, current mode 1 (N95 numbering: 0 = 64K, 1 = 16MU) | `lcd_n95` build of `fb/syborg_fb.cpp` (patch 0001 macros) | a real mode 0 (16 bpp) if anything switches modes |
| **H-lcd-alwayson:** `lcd_n95` (macro `SYBORG_FB_N95`) logs `SYBLCD:` power/secure/mode changes and the frame buffer addresses, and ignores display-off (panel never blanked). The QEMU device itself now renders a blanked/disabled panel like one without backlight: the picture stays, dimmed to `blank-dim` percent (default 25; review 2026-11-05), so dropping this hack is a one-line change once power policy is understood | `fb/syborg_fb.cpp` (patch qemu-baseport 0003), `src/qemu-syborg/hw/arm/syborg_fb.c` | honour display off (screen saver / backlight policy; partial-display mode for the screensaver) |
| **H-lcd-native-landscape:** the N95 panel is natively landscape and wsini.ini gets the 240×320 UI through rotation 270 done by the display controller (`DisplayDriver` claims hardware rotation). `lcd_n95` reports the native size 320×240 through HAL. The frame buffer holds the picture in the current WSERV rotation (240×320 at index 3, 320×240 at index 0/2, which the screensaver uses) with 1280-byte lines at every rotation (ctl 31 and HAL line offset, as on the device; patch qemu-baseport 0013). `d_display` writes the Syborg fb ORIENTATION register = (3 − index) quarter turns clockwise and QEMU rotates the picture onto the portrait screen. Direction at index 0 (landscape top at the screen's left) was inferred from the screensaver's band coordinates and confirmed by the owner (2026-11-06). Before 2026-11-06 the pitch was 960 and memory was always shown portrait, so the screensaver's landscape picture was clipped to 240 columns | `fb/syborg_fb.cpp` (patches qemu-baseport 0003, 0013), `d_display.cpp`, `src/qemu-syborg/hw/arm/syborg_fb.c` | VRFB/DMA copy model; DSB at rotation 0/2 |
| Board is 240×320 via a patched DTB | `tools/dtb_set.py`, `SYBORG_DTB` | fine |
| Rotation caps (+36/+40/+44) = 0/1/1 as on the device (checked against Nokia's `d_display.ldd`, docs/devices/nokia-n95-8gb-rm320/nokia-d_display.md); part of H-lcd-native-landscape (ScDv draws portrait into memory, the "controller" would rotate) | `d_display.cpp` | – |
| ARMv7 `WFI` added to `SyborgWFIIdle` after the ARMv6 CP15 wait-for-interrupt (A8 builds only since H-cpu-arm1136) | `specific/interrupts.cia` (patch qemu-baseport 0004, 0011) | fine as is (not really a hack) |
| **H-rtc:** hardware RTC = QEMU `syborg-rtc` register 4 (UTC seconds since 2000-01-01, host time + an offset). A guest write moves the offset, and `-global syborg-rtc.offset-file=FILE` persists it (a sydb session or instance: `<session>/drives/rtc-offset`; delete it to go back to host time). The variant installs an initial-time hook because the default one ignores the RTC on a cold start, and every QEMU launch is one (the clock restarted at the ROM timestamp) | `specific/assp.cpp` (patch qemu-baseport 0007), `src/qemu-syborg/hw/arm/syborg_timer.c` | a real battery-backed RTC model; on the N95 the RTC is the modem's, read through DOS/ISI |
| **H-fastcounter-32k:** the kernel fast counter (`NKern::FastCounter`, `User::FastCounter`) reads a 32768 Hz counter (QEMU `syborg-rtc` register 5) instead of the 1 MHz latch. The N95's `hal.dll` reports `EFastCounterFrequency` = 32768 on its own, so with 1 MHz every app that paces itself with the fast counter measured time ~30x too long and never slept (Gravity Defied ran unthrottled, 3-4x too fast) | `specific/highrestimer.h` (patch qemu-baseport 0008), `src/qemu-syborg/hw/arm/syborg_timer.c` | fine as is (matches the N95's 32 kHz timer) |
| **H-tick-1ms:** nanokernel tick 1 ms (timer load 100 at 100 kHz, `KMsTickPeriod` 1000) instead of Syborg's 10 ms; the N95's `hal.dll` reports `ENanoTickPeriod` = 1000, so `User::NTickCount` ran 10x slow for apps. Boot ~3% slower | `specific/syborg.h`, `specific/syborg_priv.h` (patch qemu-baseport 0009) | fine as is (matches the N95) |
| **H-vsync:** Syborg fb raises VSYNC (INT_CAUSE bit 0) from a fixed-rate virtual-clock timer (`-global syborg-fb.refresh-rate=N`, default 60 Hz; the N95 panel rate is assumed, not measured), plus FRAME_COUNT (17) / REFRESH_RATE (18) registers; it used to come from the host display refresh | `src/qemu-syborg/hw/arm/syborg_fb.c` | measure the real N95 rate (a DSB game's frame time on the device) |
| **H-rofs-flash:** the N95 ROFS partition (ROFS1 + ROFx, 34 MB) lives in a 128 MB read-only flash window at physical 0x60000000 (QEMU ROM region filled by `-device loader` from `<rom>.rofs`, which `tools/romcompose.py` writes and `scripts/run-modern.sh` loads), not in the RAM image; MEDROFS.PDD maps it. Stands for the phone's NAND | `src/qemu-syborg/hw/arm/syborg.c`, `src/standins/medrofs/medrofs.cpp` | a NAND/OneNAND model with the real N95 partition layout |
| **H-ram-168:** 168 MB RAM (bootstrap `KHwRamSizeMb`, patch qemu-baseport 0010; QEMU board default) so that free RAM at idle matches the N95 8GB (Nokia: 128 MB, "~90 MB free executable RAM"; measured here 93.7 MB free via HAL `EMemoryRAMFree`, 2026-11-06). More than 128 MB because our kernel keeps the whole 58 MB core image in RAM, while the phone (firmware v20+) demand-pages it from NAND | `bootstrap/syborg.inc` (patch 0010), `syborg.c` | ROM demand paging (needs a paging-capable memory model) and 128 MB |
| **H-cpu-arm1136:** CPU = ARM1136 r0p2 like the N95's OMAP2420 (QEMU `arm1136-r2`, board default; MIDR 0x4107b362; r0p2 inferred from the N810 kernel's ARM_ERRATA_364296). Kernel/BSP built `__CPU_ARM1136__` / `CFG_CPU_ARM1136`, VFPv2 (patch qemu-baseport 0011); the ARMv7 WFI word in the idle loop is left out on ARM1136 (the CP15 WFI halts). QEMU's ARM1136 cache type register was invalid for v6 (ctype 0, kernel FAULT cache_maintenance.cpp 804): set to 0x1D992992 = 32 KB I/D, 4-way, 32-byte lines, VIPT aliasing as N800 kernels report for the OMAP2420 (patch qemu-modern 0002). Same boot times, exceptions and panics as the A8 build; A8 images kept in `build/n95/a8/` (run with `-cpu cortex-a8`) | `variant.mmh`, `config.inc`, `specific/interrupts.cia` (0011), `target/arm/tcg/cpu32.c` (qemu-modern 0002), `syborg.c` | fine as is |

## Device hacks

The N95 8GB's hardware stand-ins, ROM configuration and user-side code patches moved to
[models/nokia-n95-8gb-rm320/hacks.md](../models/nokia-n95-8gb-rm320/hacks.md) (same hack ids); the
N8's are in [models/nokia-n8-rm596/hacks.md](../models/nokia-n8-rm596/hacks.md), the 5800's in
[models/nokia-5800-rm356/hacks.md](../models/nokia-5800-rm356/hacks.md). The 701 and 808 (Belle FP2):
H-fmradio-none in each model's `code-patches.txt` header and
[personalities/sym102/README.md](../personalities/sym102/README.md); H-startuptone-none,
H-bt-ondemand and H-khr in the comments of `models/nokia-701-rm774/model.toml` and
`models/nokia-808-rm807/model.toml`.

## Stand-ins shared between devices (src/standins)

| Hack | Where | Real implementation |
|---|---|---|
| **H-modem-qemu:** the fake BB5 modem is a QEMU device (`syborg-modem`, 2026-11-10): the guest's ISC/ISA stand-ins (`iscpeer.inl`) only forward IscApi's sends, subscriptions and channel open/close to its port (0xc0019000 IRQ 15, port 1 0xc001b000 IRQ 16) and deliver what it queues. The modem talks to
channel *objects* (the guest gives each a handle and writes handle << 8 | channel number), not channel
numbers: the 701/808 audio server, DSP controller and audio engine all open channel 36, and keyed by
number the replies went to the last opener and every open wiped the others' subscriptions (start-up tone
never started, Startup waited on it; fixed 2026-11-10). Frames the modem produces during a guest send
(replies, indications, log lines) raise no interrupt: the guest reads them in that same call, as the
in-guest modem queued them; only later ones (delayed replies, power) interrupt. An interrupt per reply put
an ISR + DFC between a client's send and its next step, other threads ran first and N8 Belle start-up
reordered into a reboot loop (fixed 2026-11-10, docs/devices/nokia-n8-rm596/bring-up.md "Start-up
reboot loop"); every answer (canned table `gen/isi-replies.tab` written by `device.py qemu-args` at launch, request-dependent replies, `[modem].indications`, RAPIDO DSP timing, MTC power off/reset) is host code, so a modem change needs no ROM rebuild. Answers a modem in NORMAL state with no SIM | `src/qemu-syborg/hw/arm/syborg_modem.c`, `src/standins/_shared/iscpeer.inl`, patches/qemu-baseport/0023 | a modem model with a SIM, network registration and packet data (docs/research/connectivity-options.md) |
| **H-accel-standin:** the accelerometer is a QEMU device (`syborg-sensor`, 0xc001d000, no IRQ, 2026-11-10): the host holds the gravity vector (`qom-set /machine/sensor accel "x,y,z"`, milli-g; the window's Turn view sets (+1000,0,0), portrait (0,+1000,0); `sydb accel`). Nokia's own accelerometer.ldd/.pdd cannot run on our kernel: the PDD imports `i2c0.dll`, `egpio.dll`, `PowerResourceManager.dll`, `GAPE.DLL`, and i2c0/egpio import `karap.dll`, Nokia's RAPIDO platform layer that our Syborg kernel replaces [V N8 imports]. Running them would need the RAPIDO interrupt controller, clocks, I2C, GPIO and the sensor chip emulated in QEMU. So `src/standins/accelerometer` (accelerometer.ldd + an empty accelerometer.pdd, `[[standins]] file`) speaks Nokia's channel protocol to accssy.dll and forwards the host's vector, nothing else: it reads X/Y/Z every 40 ms and completes request 3 with one 24-byte record; controls 0/2 answer zeros, 1/3 are accepted [I], other requests stay pending (docs/devices/nokia-n8-rm596/accelerometer.md). Status (2026-11-11): Turn view rotates the N8 UI [V 011.012]. What it took, all host data in the profile's `[board.accelerometer] info` (the 248-byte driver block QEMU serves at INFO): the chip id accssy finds in its cenrep 20000585 (0x03023B1D, LIS302DL at 0x1D), the data-rate (40/100 Hz) and range (2000/8000 mg, 8 bit) tables its cenrep profiles must find, and all four channel bits (the sensor server rejects a registration that lacks any static channel of accssy's ECom resource: sizes 0, `CSensrvChannelBuffer` KErrArgument, open -> -1); request 1 is a synchronous start (completed at once). The context framework rule did fire all along (Device:Orientation.Active = true) | `src/qemu-syborg/hw/arm/syborg_sensor.c`, `src/standins/accelerometer`, patch qemu-baseport 0025, `tools/sydb/phone.py` turn | Nokia's drivers on an emulated RAPIDO SoC |

## sydb dev bridge (tools/sydb, src/n95apps/devagent)

| Hack | Real implementation |
|---|---|
| H-sydb-autostart: `sydb install` puts `boothook.exe` (SID = EStart's 0x10272C04) on C: under the name of a program EStart starts by bare name (`HALSettings.exe`, 9.1: `LanguageSelection.exe`; C: is searched before Z:); it starts the agent once and chains to the real z: program with the same command line. Starter's `start.dat` list (used 2026-11-06..08) never ran on the 9.4 boot and does not exist on Symbian^3 (docs/agent-autostart.md) | none needed: test images only, the phone's own start-up lists stay untouched |
| devagent.exe is All-TCB, so writes to `\sys\` and `\resource\` go through `dafs.exe mv` (TCB); EKA2 refuses an ALL-caps EXE that links non-TCB DLLs (WSERV, apparc, avkon all lack TCB) | fine as is (a design constraint, not a shortcut) |

## N-Gage QD (EKA1) bring-up board (src/qemu-ngage)

The original RH-29 ROM (bootstrap, EKA1 kernel, user side) on the `ngage` QEMU board.
Evidence and status for each entry: [docs/eka1-ngage-questions.md](eka1-ngage-questions.md).
The ROM file on disk is never changed; the board patches its own flash copy, and logs every
patch it applies at start-up.

| Hack | Where | Real implementation |
|---|---|---|
| **H-nos-standin:** the cellular CPU side (Nokia's NOS/CMT stack, on the same ARM) does not run. Its code is scrambled in the flash file (Q20). A stand-in stub (`nos_standin.S`, about 5.5 KB) is copied over the NOS entry point (virtual 0x5149E4C4), so `nosstarter.ext` calls it. It does what the NOS does for the Symbian side, as far as seen: registers variant slot 3 (product data, Q21) and slots 6/7 (objects medint.pdd needs, Q45), sets the variant's NOS status word to 3 (Q24), and installs the power model (a trap object built with `DPowerModel`'s own constructor, Q22). Every method call goes to the board's NOS device (0x0C1F0000, log `ngage-nos:`). That device answers 0, and writes product-data items 14 and 15 as 0 and item 12 as 105 on a factory-fresh flash (EStart formats a blank C:, Q44) or 100 (a normal start) once the flash was kept (Q55). Asynchronous reads (a TDfc in r2) complete through `Kern::Add`; item 5, a notification, is left pending (Q34). The NOS's two exports to `nosdriver.ldd` (0x514A260C, 0x514A1C90) jump into the stub, so NOSDRIVER channel requests reach the device (Q33, Q36). The null thread's per-pass call (slot 3 item 13, the NOS idle hook) returns 0, answered by the stub from RAM unless the board flags an ISI delivery (Q56). On the first NOS call after the variant has rtc.dll's object, the stub makes that object's request 1 (the NOS's RTC hand-over, which caches the NOS interface) with the RTC's own time (Q49). The phone protocol (PhoNet/ISI over `nosdriver.ldd`) is answered by the same device: see the ISI replies row | `src/qemu-ngage/nos-standin/`, `src/qemu-ngage/hw/arm/ngage.c` (`ngage_nos_*`) | the real NOS, descrambled (owner's decision, Q20), or a stand-in that also covers what the NOS answers over ISI |
| **H-ngage-fb:** the board shows the window server's screen chunk (`[[0x800066EC]+24]`, 32-byte header, then EColor4K pixels) instead of the panel's memory, until lcd.ldd writes a whole frame of pixel words to the panel itself. lcd.ldd most likely sends its pixels by DMA, which is not modelled (Q47) | `ngage.c` `ngage_lcd_from_screen_chunk` | model the LCD DMA channel so lcd.ldd's own transfers fill the panel |
| **H-ngage-rww:** QEMU's CFI01 flash gets read-while-write partitions (2 MB): only the partition the last command went to leaves read-array mode. Matches the part's design [I], not a workaround of firmware behaviour, but the partition size is a guess (Q48) | `patches/qemu-ngage/0001-pflash-cfi01-rww-partitions.patch`, `ngage.c` `NGAGE_FLASH_PARTITION` | per-partition modes and status, from the L18 datasheet |
| **H-ngage-startup:** after the NOS stand-in has run, the board reports a reset source in 0x0C150000 (bit 1) and start-up code 1 at 0x0C00420E. ecust then gives start-up mode 0, and Starter runs its normal list (Q51). On a real phone the NOS or the hardware provides these [I] | `ngage.c` `ngage_startup_handover` (`NGAGE_STARTCODE`, -1 = off) | find who writes them (the NOS, descrambled) |
| **ISI replies (part of H-nos-standin):** the board answers NOSDRIVER (PhoNet) requests from `src/qemu-ngage/isi-replies.txt`. Most entries are the N95's BB5 answers, unchanged [I]. The stub's DFC writes each reply into the client's receive buffer and completes the request (Q50) | `ngage_isi.c`, `nos_standin.S` `rx_dfc_fn` | the real NOS (Q20), or N-Gage replies checked message by message |
| **H-ngage-keys:** the 6x5 key matrix that ekeyb.dll scans is modelled (rows and columns, CTSI interrupt 25). Host keys map to matrix positions; the names come from the standard scan codes [I]. The power key (UEM) is not modelled (Q53) | `ngage_keys.c` | the UEM power key and its interrupt |
| **H-ngage-md5:** the bootstrap MD5-checks ROM 0x50009000-0x51164FFF and halts on a mismatch. The digest stored in the flash file is not the MD5 of its bytes (most likely because of the NOS scrambling, Q12/Q20). The board writes the digest of the image it serves into its flash copy | `ngage.c` `ngage_fix_md5_table` | drop it once a descrambled image matches the stored digest |
| Product-data values (items 14 and 15 = 0 for `product_info.dll`, item 12 = 105 or 100 for EStart/Starter; override with `NGAGE_ITEM12`) are guesses that pass the checks; the UEM ADC result (CBus dev 1 reg 0x1A) = 480 (Q35) | `ngage.c` `ngage_nos_items` (Q23) | the phone's real PMM values |
| Clock guesses: timers 0 and 3 at 34.67 MHz, the others at /15 [I: timer 0 verified; timer 3 from medint's flash-program period, Q54] | `ngage.c` `ngage_timer_hz` | the UPP clock tree |
| Bootstrap answers: Intel 0x8874 for both flash chips; power-on reason = power key (CBus dev 1 reg 0x15 bit 4, start-up mode 4); device 4 ASIC id 0x0400; PLL/ready bits forced on | `ngage.c` (Q4, Q9, Q10, Q14, Q15) | real chip models where they matter (the C: flash media, UEM ADC/RTC) |

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
