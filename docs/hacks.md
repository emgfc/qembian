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
| **H-snd-device (2026-11-12):** sound is one QEMU device, `syborg-snd` (`syborg_audio.c`, id `c51d0011`, 0xc000e000, IRQ 13): 16 slots, each a guest ring the host reads by physical address, resamples (linear), decodes (FFmpeg MP3/AAC) and mixes into one 48 kHz stereo voice; the backend's consumption is the clock (`docs/research/sound-device-architecture.md`). The stand-ins only bind rings to slots: `ehwa` (N95/N73) one slot and 64 KB ring per DSP node, completion from the slot's watermark/EOS interrupts; `aamdaifdriver` (RAPIDO) the DSP window is physically contiguous and stream type t's ring is slot t | `src/qemu-syborg/hw/arm/syborg_audio.c`, `src/standins/_shared/sybsnd.h`, `ehwa.cpp`, `aamdaifdriver.cpp` | a DSP model is not needed for playback; capture (slot 15) not done |
| **H-rapido-pacing:** the fake modem answers RAPIDO F3 0x34 ("data written", u32 @12 = write position, **big-endian**) with 0x35 when syborg-snd has played the block, less `audio.lead_pct` (25 %) of the ring, instead of the old fixed half-ring timer. The engine writes one 19,200-byte block per reply into a 30,720-byte ring [V N8, 808]. 0x35's u32 is still the echo of the request (meaning of a real DSP's value not known) | `syborg_modem.c` (F3 0x34), `modem_reply_snd` | a DSP read position in 0x35/0x37 if a firmware needs it |
| **H-rapido-format:** RAPIDO stream type 0 is 48 kHz **stereo, 24 bits in 32-bit LE words, planar in chunks of 240 left then 240 right samples** (1,920 bytes = 240 frames = 5 ms; 384 KB/s). Measured 2026-11-12 on the N8 with a ring dump (`qmp pmemsave` of the DSP window) while `playfile.exe` played a left-only 1 kHz WAV; 16-bit decoding gave white noise, interleaved decoding doubled every clip's length. Types 1 and 2 are guesses (`48000/1/24`, `16000/1/24`); declared per profile `[board.audio] streams = "RATE/CH/BITS/PLANAR,..."` -> `audio.streams`; the device's slot register 14 (PLANAR) de-interleaves | `aamdaifdriver.cpp` StreamFormat, `syborg_audio.c` snd_pcm_next, `tools/device.py` | measure types 1/2 when a phone uses them (calls, voice) |
| **H-modem-keytone:** BB5 key tones are made by the cellular modem: the audio server sets PN_AUDIO 0B 14 device 5 parameter 5 = 0x80 and parameter 8 (output on) = 1, and sends no PCM to the DSP (a WAV sets 0x10 and plays through the DSP) [V N95 and N8 logs]. The fake modem plays a host tone (`syborg_snd_beep`, 1100 Hz, 70 ms, gain 80/256; `[board.audio] keytone = "HZ/MS/GAIN"`) when the output goes on for source 0x80. Frequency/length are ours, not Nokia's | `syborg_modem.c` `modem_audio_params`, `syborg_audio.c` | the real modem's tone set (key tone per profile, DTMF) if ever known |

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
| **Timer stop/start (2026-11-12, fix, not a hack):** a mode write 0 keeps the remaining count, a running mode written while stopped resumes from +10 (timer 1 froze otherwise: every one-shot kernel timer, the keypad scan included, stopped) | `ngage.c` ngage_timer_write | n/a |
| **H-ngage-mmc (2026-11-12):** MMC host at 0x0C120600 + the DMA engine at 0x0C120300 (`ngage_mmc.c`, `-M ngage,mmc=CARD.img`): every command completes at once (busy bits never set, CmdIdle/DataIdle always asserted, CmdRdy = a response word waits); a fixed CID and a CSD v1.x sized from the image; R1 always 0x900 (tran, ready). DMA: only transfers to/from the MMC FIFO are modelled (whole count at once, channel interrupt = source 6 + ch [I]); other channels (the LCD's) keep the catch-all behaviour (control reads back what was written). Card detect = the stack's own CMD0/1/2 polling | `src/qemu-ngage/hw/arm/ngage_mmc.c`, docs/research/ngage-mmc-controller.md | busy timing, write-protect/lock (CMD42), the GD1 game-card driver, a DMA model for the LCD |
| **H-ngage-bt (2026-11-12):** the Bluetooth module (LPRF, CBus device 5, CTSI status 2 bit 4 on source 25) is a minimal Bluetooth 1.1 HCI controller (`ngage_lprf.c`): it answers btldd's register protocol at once (reset indication on control 1, buffer size 520, every write acknowledged), every HCI command succeeds with standard return fields, inquiries find nobody, connections time out. BD_ADDR 00:02:EE:12:34:56, name "N-Gage", manufacturer 1, features and buffer sizes are guesses nothing checks [I]. Out-of-range timeouts get status 0x12 as on a real controller. The module never sleeps (no 0x20/0x100 status, no wake-up GPIO) | `ngage_lprf.c`, `ngage.c` (CBus dev 5), `ngage_keys.c` (CTSI 2); docs/research/ngage-bluetooth.md; `tests/ngage_lprf_test.c` | the real module's timing, sleep and its vendor commands; a radio link (host Bluetooth or a second board) for multiplayer |
| **H-ngage-selftest-stop (2026-11-12):** the ISI table sends SelfTestServer `PN_EPOC_TEST` (0x59) msg 1 sub-id 8 when it opens unit 13. That case answers and stops SelfTestServer; it then closes its BTTestServer session, BTTestServer exits and frees btldd's only channel. Without it BTTestServer (connected by SelfTestServer at boot) keeps the channel for ever, bt.prt's HCI open fails -11, and BTServer, emon ("App. closed EmonServer") and RHostResolver::GetHostName (Snakes) fail. What really ends the boot test session on a phone is not known [I] | `src/qemu-ngage/isi-replies.txt` (`ind 13 ...`) | the CMT's own end of the test session (real NOS), or whatever the phone really sends |

## Hosted EKA2 (src/hosted, tools/hosted/htest.py; docs/hosted-eka2/)

The EPL kernel running as a 32-bit Linux process, the phone's ROM code native (design:
`src/hosted/README.md`, future designs: `docs/hosted-eka2/future.md`). Test machine: QEMU virt aarch64.
[V] = checked against the ARM board, the EPL source or a test; [G] = a guess or a choice that only had to
work for now. Each row says where the shortcut lives and what the real implementation would be.

### Architecture-level shortcuts (deliberate, see src/hosted/README.md "Choices")

| Hack | Where | Real implementation |
|---|---|---|
| The whole Symbian machine is ONE Linux process: every Symbian process shares one address space; no memory protection between Symbian processes, nor between user code and kernel data (as the Win32 emulator) [G] | design | one Linux process per Symbian process, kernel data in a shared memfd (future.md section 4) |
| The host CPU must execute AArch32 (old phones, Cortex-A53..A76 class, the QEMU test VM); arm64-only CPUs and x86-64 cannot run it [V] | design | JIT / ahead-of-time translation (future.md section 6) |
| One host thread per nanothread, exactly one running at a time; a switch is a futex hand-over (Win32 model). The unicore `nkern`, no SMP [V] | `nkern/linux/ncsched.cpp` | one host thread for all nanothreads, or nkernsmp with per-CPU host threads (future.md 3, 5) |
| Futexes are process-shared (no FUTEX_PRIVATE_FLAG) although there is one process: kept for the multi-process and SMP designs, where the scheduler lock, fast mutexes and thread events in shared kernel data must be woken from another process. Costs the slower shared-key path on every futex call [inferred 10-30%] | `rt/hrt.h` | private futexes while single-process if a measurement says it matters; shared again with the split |
| Kernel-side ROM images (the phone's EKERN-prelinked LDDs/PDDs/extensions) are replaced by hosted builds of the same sources by file name (htest KMODULES, LCD.DLL from the profile); ROM extensions without a build are skipped with a log line (BTRACEX, EXMON*, KDEBUG, svphostfs, svpsnap, rm_debug); sound and webcamera drivers not built [V for the built ones] | `kernel/linux/kmodules.cpp`, htest.py | the module list generated from the device profile like the ARM ROM |
| Each built-in module is one partial link with every symbol not used by another object localised (COMDAT groups dropped first, so modules from one source keep their own inline copies) [V: kn_efile] | htest.py | real separately loaded kernel DLLs (a hosted E32 loader for kernel code) |
| A reloaded built-in module gets its .data restored, .bss zeroed and constructors rerun (as a fresh ARM DLL); no static destructors on unload [G] | `kmodules.cpp` | same, plus destructors if a driver needs them |
| Test variant (HostedAsic) instead of our Syborg variant: 1 ms tick from a nanokernel interrupt thread, host clock as RTC, debug output to fd 1. Its variant HAL copies Syborg's answers (`assp.cpp`: EMachineUid_OmapH4, LED caps) [V] | `variant/hvariant.cpp` | run the real Syborg variant (KASYBORG) on intc/timer/RTC device models |
| Built with `__DEBUGGER_SUPPORT__` as our ARM kernel [V] | htest.py | - |

### Fixed addresses and sizes (chosen, not computed as the phone's memory model does)

| Hack | Where | Real implementation |
|---|---|---|
| Layout: EXE data window 0x00400000; RAM code 0x70000000 (128 MB); DLL static data 0x78000000 (127 MB); exec trampolines 0x7ff00000; core ROM 0x80000000; I/O window 0xC6000000 (64 device pages); kernel data 0xC8000000; the hosted kernel image linked at 0xD0000000 (its .bss over 0x00400000 broke EXE data once) [G for the RAM regions; ROM, I/O, kernel data as the multiple model] | `memmodel/linux/mcodeseg.cpp`, `vectors.cpp`, `include/board/mmboot.h`, htest.py `-Ttext-segment` | the multiple model's computed regions (`iUserCodeBase` from the ROM header) |
| RAM code and DLL data regions bump-allocated and never freed; `Epoc::AllocPhysicalRam` a bump allocator over a 32 MB "physical" window at 0x20000000, `FreePhysicalRam` does nothing [G] | `hboard.cpp`, `mcodeseg.cpp` | real allocators (TBitMapAllocator) |
| Physical windows: I/O at phys 0xC0000000 (linear 0xC6000000), ROFS flash at phys 0x60000000, driver RAM at 0x20000000; `Epoc::LinearToPhysical` answers only for these [G] | `hboard.cpp`, `memmodel/linux/mphys.cpp` | a page-table-like map of every physical allocation |
| RAM size reported as 128 MB, RAM drive max 32 MB; the kernel heap is a 64 MB anonymous mapping (MAP_NORESERVE) [G] | `memmodel/linux/minit.cpp`, `nkern/linux/nkhost.cpp` | the phone's RAM size from the profile; heap growth as on ARM |
| Host thread stacks 128 KB; thread records in never-freed pages [G] | `nkern/linux/lxhost.cpp` | per-thread sizes from the Symbian thread's supervisor stack size |
| The EKERN process's UIDs/SID/VID set by hand (on Win32 they came from the PE image) [V: values of the ARM EKERN] | `minit.cpp` | from the ROM's EKERN image header |
| EUSER's exec stub range (N95: 0x803a0348..0x803a0d28) is found by `tools/hosted/romscan.py` on the composed ROM and passed in by htest; `svc`s outside it reach Linux as syscalls [V for the N95] | htest.py, `vectors.cpp` | scan every module's stubs at ROM composition; seccomp on all Symbian code ranges |

### Memory model

| Hack | Where | Real implementation |
|---|---|---|
| EXE .data/.bss of the running process in one live window at 0x00400000, saved and loaded by copy on process switch (a memfd per process before, remapped: 2 mmaps per switch); IPC to a process that is not live goes through its alias, IPC to the live window with the kernel locked | `memmodel/linux/mprocess.cpp`, `mutils.cpp` | per-process address spaces (future.md 4) |
| DLL static data (ROM and RAM DLLs) uses the Win32 model's copy-on-switch (`iDataCopy`) [V] | `mutils.cpp` | per-process address spaces; until then copy only dirty data |
| `RawRead`/`RawWrite` check only for a NULL non-zero-length address; any other bad user pointer from the kernel faults and is caught by the exception path (no IPC alias, no permission check) [G] | `mprocess.cpp` | per-process mappings with real protection |
| `PrepareMemoryForDMA`/`ReleaseMemoryFromDMA` return KErrNotSupported [G: no driver needed them yet] | `mprocess.cpp` | pin and translate pages |
| Code modifier (debugger breakpoints written into code) not ported: `CodeModifier::CodeSegRemoved` no-op | `kernel/linux/kplat.cpp` | port memmodel/epoc putils.cpp CodeModifier |
| `kernel/scodeseg.cpp` built at -O0: its comparator tests bit 0 of a reference (UB), clang -O2 folds it away [V] | htest.py | patch the comparator |
| EPL sources clang refuses compile from generated copies with the same meaning: ECOMM `if (a1<0)` on a pointer -> `(TUint)a1<0u` [V: GCC/RVCT compare pointers unsigned] | htest.py `SOURCE_FIXES` | upstream-style source fix |

### Nanokernel, CPU and exec calls

| Hack | Where | Real implementation |
|---|---|---|
| Exec calls: EUSER's `svc #n; bx lr` stubs (and lone `svc`s in the block) patched in the loaded ROM to jump to per-stub trampolines (direct calls into the dispatcher on the thread's stack, realigned to 8 bytes; only r0 changes, as the trap); unmatched ones still trap (SIGSYS). `htest --env EXEC_TRAP_ONLY=1` disables the patching [V] | `nkern/linux/vectors.cpp` | stub patching at ROM composition, or a separate kernel stack per thread as ARM |
| The host thread's record is in TPIDRURO (Linux `set_tls`); we assume the phone's user code never reads TPIDRURO (it uses TPIDRURW, which we leave alone) [G for ARMv6K+ firmware] | `nkern/linux/lxhost.cpp` | check every firmware's EUSER for TPIDRURO reads |
| Raw host threads (`hthread`: display, sound pump, modem timers) start with TPIDRURO 0 so `LxSelf()` is NULL on them; nanothreads set their record themselves [V: t_tls] | `rt/hrt.c` | - |
| Interrupts suspend the running nanothread with SIGUSR2 and park it (Win32 SuspendThread); disabling interrupts = taking the global interrupt lock [V] | `ncsched.cpp`, `lxhost.cpp` | user-level switches on one host thread (future.md 3) |
| Tick suppression: the 1 kHz tick thread takes the interrupt lock and advances the timer queue (NTimerQ::IdleTime/Advance) when nothing expires, a real interrupt only when due and at least every 8 ticks (timeslice ticks caught up then). Timeslices therefore expire in 8 ms steps; idle sleeps in 1 ms steps [G] | `nkern/linux/nkhost.cpp` | a one-shot host timer programmed to the next expiry (future.md 2) |
| Idle = WFI: the hosted `NKIdle` calls the power model/handler and then parks the null thread until an interrupt reschedules (`NThread::Idle`); no hosted power controller does the wait itself. Bug 2026-11-12: the null thread spun at 100% CPU after boot [V: kn_boot] | `kernel/linux/kplat.cpp` | a hosted power controller (DPowerController::CpuIdle) doing the wait, as the Win32 emulator's |
| ARM11-only encodings emulated on SIGILL: CLZ with zero should-be-one fields (N95 AknIconServer) [V] | `nkern/linux/lxhost.cpp` | same on a real ARMv8 phone; add encodings as they appear |
| SWP/SWPB and CP15 barriers rely on the host kernel's emulation (`CONFIG_SWP_EMULATION`, `CP15_BARRIER_EMULATION`, an undef trap each); the N95's EUSER has none, some apps do [V: romscan] | `src/hosted/linux/hosted.config` | patch hot sites to LDREX/STREX if they show up in profiles |
| Unaligned accesses rely on the host kernel's compat alignment fixups [G] | `hosted.config` | - |

### The hosted board (device models)

| Hack | Where | Real implementation |
|---|---|---|
| I/O bus: a device page without a model is plain memory (reads 0 or what was written). Models: NVMemory (0x10 IRQ 12, 0x0d IRQ 14), framebuffer (0x05), keyboard (0x03 IRQ 2), sound (0x0e IRQ 13), modem (0x19/0x1b, IRQ 15/16). No model for serial, pointer, timer, intc, RTC, GL, sensors, ethernet, platform device [G] | `variant/hboard.cpp` and the device files | one model per QEMU syborg device, shared code where possible (as the modem core) |
| Interrupts: edge delivery (the raised flag clears when the ISR runs, a device raises again), no priorities, `Interrupt::SetPriority` KErrNotSupported, `Clear` no-op; one IRQ thread runs every ISR [G] | `hboard.cpp` | level semantics as syborg_intc.c |
| Device models (hsnd, hmodem, hkeyb) serialise register access with a spinlock that spins with `sched_yield`; a nanothread takes it with interrupts disabled (an ISR on the interrupt thread reads the same registers), ISRs and host helper threads take only the spinlock. Bug 2026-11-12: with only the kernel locked, the sound ISR spun on the lock its interrupted thread held (start-up tone hang) [V: kn_devlock] | `variant/hsnd.cpp`, `hmodem.cpp`, `hkeyb.cpp` | per-device locks as QEMU's, ISRs run without the global lock |
| NVMemory drives map the seed images private: every write is lost when the run ends [G, deliberate for tests; the phone window too] | `hboard.cpp` | a writable copy per instance, as sydb sessions |
| Fake modem on the hosted board: offline only (booting first); no packet data (no network), no sound pacing (`snd_*` hooks answer "nothing playing": RAPIDO replies come at once), power off/reset ends the process [G] | `variant/hmodem.cpp` | host network and the sound model's ring state |
| syborg-snd as a sink: rings consumed at the playback rate in 5 ms steps (encoded MP3/AAC at a nominal 16 KB/s), CAPS claims MP3/AAC decoders so ehwa accepts codec nodes; nothing is played [G] | `variant/hsnd.cpp` | an audio output (host sound), the modem's RAPIDO pacing hooks |
| Framebuffer: registers (size, base, pitch, refresh), no vsync interrupt, orientation/partial mode stored but not applied [G] | `hboard.cpp` | vsync and damage from the LCD driver |
| Live view: an ivshmem device (PCI 00:05.0, 1 MB, a host file build/hosted/htest/phone.shm) holds a header, a 64-entry key ring and XRGB pixels; a host thread in the VM copies changed rows every 30 ms and bumps a frame counter; `tools/hosted/phone.py` polls the file every 20 ms. The VM finds the BAR through its sysfs `resource2` file (env `PHONE`) [G] | `variant/hdisplay.cpp`, htest `--phone`, `tools/hosted/phone.py` | DRM/KMS on a phone; a doorbell interrupt instead of polling |
| Keypad: the phone window maps Qt key -> qcode (the sydb phone window's table) -> scan code (the device keymap through `tools/device.py qemu_keymap`) and writes it to the key ring; the VM's display thread drains the ring every 10 ms into the syborg-keyboard model as bit-30 codes (already scan codes, no set-1 table); a full ring or FIFO (64) drops keys; key auto-repeat comes from the phone's window server, not the host [G] | `variant/hkeyb.cpp`, `hdisplay.cpp`, `tools/hosted/phone.py` | evdev on the phone (the keymap's host keys) |
| Linux console (Ctrl+T in the phone window): the VM's init forks `lxtop` in phone mode, which prints a `top`-like block (processes, the "symbian" process's threads by CPU) to the serial log every 2 s, lines prefixed `LXTOP`; the window parses the log. Each Symbian thread names its Linux thread at start (`prctl(PR_SET_NAME)`, 15 chars: "Proc:Thread"); kernel helper threads that never pass `EpocThreadFunction` keep their creator's name ("Supervisor") [G, a show-and-tell feature] | `src/hosted/init/init.c`, `kernel/linux/kthreadname.cpp`, `tools/hosted/phone.py` | a virtio console or a shell in the image |

### Test tooling

| Hack | Where | Real implementation |
|---|---|---|
| The test VM's Linux is arm64 defconfig + `hosted.config` (6.18 LTS), not stripped down; QEMU TCG `cortex-a55` (also `max`), 1-4 CPUs, 1 GB; ROM, seeds and output over 9p [V] | `tools/hosted/htest.py`, `src/hosted/linux/` | a minimal kernel config for the Pixel 3a |
| htest `--env` values are read as hex by the tests (`hgetenv_hex`): `PROBE_MS=4e20` is 20 s, `20000` is 131 s [V: bit me 2026-11-12] | `src/hosted/tests/kn_efile.cpp` | decimal values, or a unit suffix |
| `kn_efile` judges the boot at a fixed probe time (default 15 s): what has started, who is blocked, then pass/fail; a slower machine needs a later `PROBE_MS` [G] | `kn_efile.cpp` | wait on events (a start-up-done property) instead of a time |

### Known remaining non-fatal failures (hosted)

| Hack | Where | Real implementation |
|---|---|---|
| Boot proceeds with known failures: DcfRepSrv panics CDCFREPSRV 1000 (not on ARM, open); HALSettings -12 and baksrvs/mc_useragent exits also happen on ARM [V] | - | investigate DcfRepSrv |

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
  the audio work. **Done 2026-11-12 for the audio patches:** N95 8GB default ROM omits H-keysound-nommf and
  H-startuptone-nommf (`[rom] omit_patches`); N8 (011.012 and other non-Delight firmwares), 701 and 808 no longer
  carry H-startuptone-none (the tone plays and ends). 5800, N97, E71, N81 keep H-startuptone-none (stack B,
  eapdsp, not tested).

- `RightsServer.EXE` restarted when SecEnv request 6 failed (identity hack), then panicked `USER 130` on the empty cert chain (dummy cert hack, run 20).
- SIM `READ_IMSI` (`09 1D`) answered "data not available" (`[service, 0x24]`, same as the other SIM reads): the phone behaves as having no usable SIM.
- `SecEnvInit` polls `SecEnv` request 1 every few minutes (timer driven; harmless).
