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
| ARMv6 exclusive monitor not cleared on context switch (ARMv5 kernel) | not handled | `clrex`/dummy `strex` in the scheduler for ARMv6+ CPUs |

## Board / BSP (patches/qemu-baseport, src/n95compat/lcd)

| Hack | Where | Real implementation |
|---|---|---|
| VFP enabled for GCCE builds (was RVCT-only); no `evfp.dll` | `variant.mmh` (patch 0002) | verify lazy VFP context switching under load; add VFP support code if exceptions appear |
| LCD reports 32 bpp, 2 modes, current mode 1 (N95 numbering: 0 = 64K, 1 = 16MU) | `lcd_n95` build of `fb/syborg_fb.cpp` (patch 0001 macros) | a real mode 0 (16 bpp) if anything switches modes |
| Board is 240×320 via a patched DTB | `tools/dtb_set.py`, `SYBORG_DTB` | fine |
| Screen rotation: caps claim rotations 90/180/270 supported but nothing rotates | `d_display.cpp` caps +36/+40/+44 | check how scdv rotates; the N95 runs mode 1 at rotation 270 (the picture may come out sideways) |

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
| `secldd.ldd` (`SecEnv`) | request 2 ("wait for next job") stays pending; request 6 (SecEnv.dll export 17, "process data in a secure-environment app", used by DrmBb5KeyStorage to wrap/unwrap DRM keys) is an **identity transform**: input copied to output, out length = in length; requests 3/4/5/7/16 complete `KErrNotSupported`; others complete `KErrNone` | no secure environment exists; DRM-protected content will not decrypt. Failing request 6 made RightsServer exit −5 → DRMHelperServer panicked → AknSkinSrv → EikSrvs → AknCapServer → Starter all waited forever |
| `medrofs.pdd` | ROFS1 + ROFx served read-only from memory appended to the ROM | fine |
| `lcd_n95` | see BSP | |

## ROM / configuration (rom/hybrid, tools/romcompose.py)

| Hack | Real implementation |
|---|---|
| `Z:\sys\data\ESTARTCOMP.TXT` replaced: D: = local drive 0 (IRAM), C: = 1 (NVMemory), Z: = composite with ROFS on 8; no E: (memory card), no Y: | map the N95 drive layout if needed (mass memory E:) |
| N95 kernel side dropped wholesale; our kernel files placed below the N95 user window | n/a |
| Paging off: the core ROM is flattened | n/a |

## Known remaining non-fatal failures

- `mc_useragent.exe` panics `KERN-EXEC 0` early (crash-monitor helper, ignored).
- `RightsServer.EXE` restarted when SecEnv request 6 failed (fixed by the identity hack; check it no longer happens).
