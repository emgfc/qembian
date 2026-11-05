# Hacks registry (option 1 hybrid boot)

Every shortcut taken to get the N95 user side booting on our kernel, listed so it can be revisited
later. Ordered by subsystem. "Real" means what a faithful implementation would do instead.
Keep this file up to date with every new hack.

## Kernel (patches/kernelhwsrv)

| Hack | Where | Real implementation |
|---|---|---|
| 9.2 exec table built from our handlers; 6 unmapped 9.2 calls are logging stubs returning `KErrNotSupported` (fast: return 0) | `kernel/sexec92.cpp`, `tools/exec92/exec92.map` | identify `F0a`, `F0b`, `S34`, `Se8`, `Sed` (secure time) and implement |
| Shims: `MutexWait92` (timeout 0), `HandleDuplicate92` (by-value handle), `SafeInc32`/`SafeDec32` | `sexec92.cpp`, `arm/cexec92.cia` | fine as is; audit the remaining 9.2 call argument semantics one by one |
| `ProcessCreate`/`ProcessLoaded` use the 9.2 `TProcessCreateInfo` size for 9.2 threads (newer fields zeroed) | `scodeseg.cpp` | same, plus audit other loader structures (`TCodeSegCreateInfo` assumed equal size) |
| Personality is global (ROM header flag), all user threads get 9.2 tables | `sthread.cpp`, `sinit.cpp` | per-process selection if our own user binaries are ever mixed in |
| Debug traces `REQC` (request completions) and `IPCRD` (server reads of client descriptors) | `sutils.cpp`, `sipc.cpp` (patch 0002) | keep for debugging; udeb only |
| **H-critical-exit:** with the 9.2 personality on, a system-critical/permanent *user* thread (not the file server) that exits is logged `N95HACK: system-… thread … exited` and allowed to die instead of `KERN 80` (`EPermanentThreadExit`) / `ESystemThreadPanic` | `sthread.cpp` (patch 0003) | stand-ins for the missing drivers so those threads survive. First hit: `MmfAudioServer::MmRmServerThread` dies −1 because `Bal.dll` loads PDD `EHWAOMAP` + LDD `EHWA` (TI DSP/BIOS bridge, driven through `bridgeapi.dll`, ~5 k lines of client API): **no audio** until a bridge stand-in exists |
| ARMv6 exclusive monitor not cleared on context switch (ARMv5 kernel) | not handled | `clrex`/dummy `strex` in the scheduler for ARMv6+ CPUs |

## Board / BSP (patches/qemu-baseport, src/n95compat/lcd)

| Hack | Where | Real implementation |
|---|---|---|
| VFP enabled for GCCE builds (was RVCT-only); no `evfp.dll` | `variant.mmh` (patch 0002) | verify lazy VFP context switching under load; add VFP support code if exceptions appear |
| LCD reports 32 bpp, 2 modes, current mode 1 (N95 numbering: 0 = 64K, 1 = 16MU) | `lcd_n95` build of `fb/syborg_fb.cpp` (patch 0001 macros) | a real mode 0 (16 bpp) if anything switches modes |
| **H-lcd-alwayson:** `lcd_n95` (macro `SYBORG_FB_N95`) logs `N95LCD:` power/secure/mode changes and the frame buffer addresses, and ignores display-off (panel never blanked) | `fb/syborg_fb.cpp` (patch qemu-baseport 0003) | honour display off once the UI works (screen saver / backlight policy) |
| **H-lcd-stride:** `DisplayDriver` ctl 31 (scan-line bytes, r_display export 77) returns 960 = 240 px × 4; ScDv draws portrait into the 240×320 frame buffer (the display driver claims hardware rotation, and the Nokia ScDv does not rotate in memory). An earlier landscape attempt (stride 1280, 320×240 fb) showed the UI upright but cut off — screenshot `build/evidence/run33-first-ui-landscape-stride.png` (local only) | `d_display.cpp`; `fb/syborg_fb.cpp` swaps a landscape device to portrait if ever configured (patch qemu-baseport 0003) | real stride/rotation semantics of the N95 display driver |
| Board is 240×320 via a patched DTB | `tools/dtb_set.py`, `SYBORG_DTB` | fine |
| Screen rotation: caps claim rotations 90/180/270 supported but nothing rotates | `d_display.cpp` caps +36/+40/+44 | check how scdv rotates; the N95 runs mode 1 at rotation 270 (the picture may come out sideways) |
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
| **H-splash-behind:** SplashScreen window-group priority literal 1001 → −1000 | `SplashScreen.exe` 0x833a6574 | none needed once startup completes (Startup.exe normally takes over the screen before the splash's 120 s timeout); the splash logo (SVG) also renders empty — AknIconServer/SVG to investigate |

## Known remaining non-fatal failures

- `mc_useragent.exe` panics `KERN-EXEC 0` early (crash-monitor helper, ignored).
- `RightsServer.EXE` restarted when SecEnv request 6 failed (identity hack), then panicked `USER 130` on the empty cert chain (dummy cert hack, run 20).
- SIM `READ_IMSI` (`09 1D`) answered "data not available" (`[service, 0x24]`, same as the other SIM reads): the phone behaves as having no usable SIM.
- `SecEnvInit` polls `SecEnv` request 1 every few minutes (timer driven; harmless).
