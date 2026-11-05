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
| **H-lcd-alwayson:** `lcd_n95` (macro `SYBORG_FB_N95`) logs `N95LCD:` power/secure/mode changes and the frame buffer addresses, and ignores display-off (panel never blanked). The QEMU device itself now implements blanking properly (blank/disabled panel shows black, review 2026-11-05), so dropping this hack is a one-line change once power policy is understood | `fb/syborg_fb.cpp` (patch qemu-baseport 0003), `src/qemu-syborg/hw/arm/syborg_fb.c` | honour display off (screen saver / backlight policy; partial-display mode for the screensaver) |
| **H-lcd-native-landscape + stride:** the N95 panel is natively landscape and wsini.ini gets the 240×320 UI through rotation 270 done by the display controller (`DisplayDriver` claims hardware rotation). `lcd_n95` reports the native size 320×240 through HAL while the frame buffer memory stays portrait 240×320 with 960-byte lines; `DisplayDriver` ctl 31 (scan-line bytes) = 960. Reporting 240×320 natively made ScDv draw a 320×240 picture into 240-pixel rows (bottom quarter never drawn, content shifted) | `fb/syborg_fb.cpp` (patch qemu-baseport 0003), `d_display.cpp` | real display-controller model if anything reads the rotation state |
| Board is 240×320 via a patched DTB | `tools/dtb_set.py`, `SYBORG_DTB` | fine |
| Rotation caps (+36/+40/+44) claim hardware rotation; part of H-lcd-native-landscape (ScDv draws portrait into memory, the "controller" would rotate) | `d_display.cpp` | check against Nokia's own `d_display.ldd` (still in the firmware image) |
| ARMv7 `WFI` added to `SyborgWFIIdle` after the ARMv6 CP15 wait-for-interrupt | `specific/interrupts.cia` (patch qemu-baseport 0004) | fine as is (not really a hack) |

## N95 hardware stand-ins (src/n95compat/stubs)

| Device / file | Hack | Real implementation |
|---|---|---|
| `nokiaiscdriver.ldd`, `nokiaiscdriverextension.ldd` (modem link) | fake modem `iscpeer.inl`: answers a fixed reply table (MTC NORMAL, no SIM, SIM/SEC "no SIM", product profile all zero, network/GSS reads); unknown messages get no reply; connection and flow-control notifications parked forever | a proper fake CMT with state; NET reg status reply removed because the proposed layout crashed NOKIATSY (`IsiMessage: OFFS -9`) |
| ISC ctl 21 channel info | returns `6C <channel>` (guessed) | real value unknown |
| ISC ctl 22 max data size | returns 4096 (guessed; 0 made DATAPORT leave) | real value unknown |
| ISC indications | `MTC_STATE_INFO_IND(NORMAL, START)` only to the subscribing channel itself, on subscribe and after a state query; no fan-out to other channels | proper per-subscription fan-out |
| `xbus.ldd`, `iscisimultiplexer.ldd` | load-only | n/a |
| `custom.ldd` (`Custom`) | controls 32/2/1 return 0 (normal startup mode); everything else returns 0 untouched | real startup reasons, power/reset controls |
| `sosadlserver.ldd` | channel accepts everything | unknown purpose |
| `d_display.ldd` (`DisplayDriver`) | caps block: only +0/+16/+20 = 1 (caller defaults), +60 = 1 (16MU), rotations; all requests completed immediately; controls other than 10/11 ignored | real display driver semantics (update/vsync requests, brightness, power) |
| fake modem `PN_LIGHT` (0x3A) (hack H-light) | `3A 22` (target info, from `SALightsService.01.dll` on behalf of HWRMServer) → `3A 23` `[status 0][count][request's 0x16 subblocks echoed]`; any other `3A xx` → `xx+1` echoing the request data with byte 0 = 0 | Without it HWRMServer blocks in construction on its SAServer request `0x2000000A`, never registers `!HWRMServer`, akncapserver spawns a duplicate and never rendezvouses → Starter waits forever. Real: a light model (brightness per target, on/off) |
| fake modem energy management `17 0A`/`17 08` (hack H-em) | `17 0B` = `[01][2 sb][16 04 07 07][15 04 01 00]` (battery 7/7 bars, charger not connected; NOKIATSY and SAEmService both find their subblock by id); `17 09` = `[01][1][13 04 01 00]` | real battery/charger model |
| fake modem `INFO_SERIAL_NUMBER_READ` (`1B 00`→`1B 01`) | replies `[INFO_OK][1 sb][0x41 IMEI_PLAIN len 20: 0, 15, "490154203237518", 0]`; the IMEI is the well-known documentation example, the layout follows the reference ISI headers, **not** verified against NOKIATSY's parser (hack H-imei) | RightsServer asks ETel for the phone id after DRM init; unanswered it waits forever. If NOKIATSY panics `IsiMessage: OFFS`, check subblock layout |
| `secldd.ldd` (`SecEnv`) | request 2 ("wait for next job") stays pending; request 6 (SecEnv.dll export 17, "process data in a secure-environment app", used by DrmBb5KeyStorage to wrap/unwrap DRM keys) is an **identity transform**: input copied to output, out length = in length. **Exception (hack H-secenv-cert):** mode 3 with empty input is DrmBb5KeyStorage reading the device certificate chain (output = `[TUint32 LE len][DER cert]` records, the last one X509-parsed and SHA-1 hashed); the stub returns a self-signed RSA-1024 `CN=N95-stub-device` cert generated with openssl (`src/n95compat/stubs/stubcert.h`, no firmware bytes). An empty output made `FUN_0000823a` index the record array at −1 → RightsServer `USER 130`. Requests 3/4/5/7/16 complete `KErrNotSupported`; others complete `KErrNone` | no secure environment exists; DRM-protected content will not decrypt. Failing request 6 made RightsServer exit −5 → DRMHelperServer panicked → AknSkinSrv → EikSrvs → AknCapServer → Starter all waited forever |
| `ehwa.ldd` (`Hwa`) + `ehwaomap.pdd` (`Hwa.Omap`) — TI DSP/BIOS bridge (hack H-dspbridge) | PDD load-only; LDD accepts every control, writes nothing, completes requests at once. `bridgeapi.dll` attach (ctl `0x400001e`) then fails (expects a non-zero node handle back) and MmfAudioServer's resource manager still dies. Kept only so the PDD/LDD loads succeed | a real DSP bridge model, or an MMF/DevSound replacement. MmRmServer also needs `dvfs_interface.ldd` (`LeaveIfError`) |
| `LightLdd.ldd` (`LightDrv`) + `LightPdd.pdd` (`LightDrv.pdd`) — backlight driver (hack H-lightdrv) | LDD accepts every control and completes requests at once; PDD load-only | without them SALightsService (SAServer) leaves −5 setting up a light target, HWRM never creates its light plugin and SysAp shows "Feature not supported". Real: backlight model (brightness/on-off per target), could drive the screensaver/power-save display |
| `medrofs.pdd` | ROFS1 + ROFx served read-only from memory appended to the ROM | fine |
| `lcd_n95` | see BSP | |

## ROM / configuration (rom/hybrid, tools/romcompose.py)

| Hack | Real implementation |
|---|---|
| `Z:\sys\data\ESTARTCOMP.TXT` replaced: D: = local drive 0 (IRAM), C: = 1 (NVMemory), Z: = composite with ROFS on 8; no E: (memory card), no Y: | map the N95 drive layout if needed (mass memory E:) |
| N95 kernel side dropped wholesale; our kernel files placed below the N95 user window | n/a |
| Paging off: the core ROM is flattened | n/a |

## User-side code patches (rom/hybrid/code-patches.txt, `romcompose.py --patches`)

Byte patches on N95 binaries in the composed ROM; original bytes are verified at compose time.

| Hack | Where | Real implementation |
|---|---|---|
| **H-keysound-nommf:** `blx CMdaAudioToneUtility::NewL` → `movs r0,#0; nop` (2 sites) so the key-sound server keeps a NULL tone player instead of leaving −1 when MMFAudioServer cannot start (no DSP) | `eiksrvui.dll` 0x82f0e366, 0x82f0e9c8 | working audio (MMF with a fake DSP/DevSound). Risk: the tone player's prepare-complete callback dereferences without a check, but it is only invoked by a live player; key tones are silent |
| **H-startuptone-nommf:** `blx CMdaAudioPlayerUtility::NewFilePlayerL` → `movs r0,#0; nop` in `CStartupTone::ConstructL`: no startup-tone player (opening it blocked Startup's main thread in a synchronous MMF call → `ViewSrv 11` after 10 s); `Play()` returns `KErrNotReady` and Startup falls back to a (silent) key sound | `startup.exe` 0x833a93c4; analysis in `docs/n95-startup-animation.md` | working audio |
| H-keysound-nommf (continued): the two tone-play helpers return at once (`bx lr`) | `eiksrvui.dll` 0x82f0ea94, 0x82f0eac8 | working audio |
| **H-splash-behind:** SplashScreen window-group priority literal 1001 → −1000 | `SplashScreen.exe` 0x833a6574 | none needed once startup completes (Startup.exe normally takes over the screen before the splash's 120 s timeout); the splash logo renders correctly since the CLZ fix (AknIconServer); re-check whether this patch is still needed once Startup completes |

## Known remaining non-fatal failures

- `mc_useragent.exe` panics `KERN-EXEC 0` early (crash-monitor helper, ignored).
- `RightsServer.EXE` restarted when SecEnv request 6 failed (identity hack), then panicked `USER 130` on the empty cert chain (dummy cert hack, run 20).
- SIM `READ_IMSI` (`09 1D`) answered "data not available" (`[service, 0x24]`, same as the other SIM reads): the phone behaves as having no usable SIM.
- `SecEnvInit` polls `SecEnv` request 1 every few minutes (timer driven; harmless).
