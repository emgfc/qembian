# Device coverage matrix

How we test qembian across S60 3rd Edition and later without booting every firmware of every phone:
the axes an emulator like ours is sensitive to, a rule for how many firmware versions a device needs,
the state of the devices done so far, the devices still needed, and how this file stays current.

Facts from the repository (profiles, bring-up notes, `roms/`) are stated as they are. Facts about
phones we have not touched come from general Symbian knowledge; anything uncertain is marked
[unverified]. Firmware version numbers of such phones are indicative; take the real list from the
flash-file archive when the device is added (`tools/device.py identify DIR` reads `sw.txt`).

Related: `docs/devices.md` (profile schema), `docs/adding-a-device.md` (flow and the L0..L6 levels),
`docs/roadmap.md` (queue), `models/README.md`.

## 1. Selection system

### 1.1 Axes

An axis is a property whose value changes which of our code runs. Each value must be covered by at
least one (device, firmware) pair in the test set. Values in the same row are ordered by era.

| Axis | Values | Why it matters to us |
|---|---|---|
| Platform release (user side) | S60 3rd (9.1), 3rd FP1 (9.2), 3rd FP2 (9.3), 5th (9.4), Symbian^3 PR1.0/1.1/1.2, Anna, Belle, Belle Refresh, Belle FP1, Belle FP2 | exec personality (`personalities/sym91`, `sym92`, `sym94`, `symbian3`), start-up machinery (Starter on 9.1-9.3, SSM from 9.4), window server render stages (ScreenPlay from ^3), Qt and the new home screen from Belle, which stand-ins and ISI replies are asked for |
| Kernel memory model | moving (ARMv5), multiple (ARMv6), flexible (^3 and later) | a separate kernel build each (`baseport = syborgmv` / `syborg` / `syborgfx`), ROM base and fixed-process layout, user-accessible chunk rules (the MDAIF window on the N8) |
| CPU core | ARM926EJ-S, ARM1136 r0, ARM1176 (ARMv6K), Cortex-A8 (non-Nokia only) | QEMU `-cpu`, cache-type registers (qemu-modern patches 0003/0004), TPIDRURW use by EUSER on ARMv6K, alignment checking, VFP |
| SoC / adaptation family | TI OMAP1710, TI OMAP2420, Nokia RAPIDO (RAP3G) ARM11, RAPIDO + Broadcom VideoCore (^3), [unverified] Freescale MXC300-30 (E71/6120c class) | the whole kernel-side driver set we replace: modem link (`iscdriver`+`ssi` on OMAP1710, `nokiaiscdriver`+`xbus` on OMAP2420, `nokiaiscdriver`+`sisadriver` on RAPIDO, `isaaccessldd` on ^3), DSP (OMAP `ehwa`/dspbridge vs RAPIDO `aamdaif`), display driver, NAND layout, DVFS |
| Modem protocol | BB5 ISI over ISC (all Nokia S60 3rd+ in this list); non-Nokia modems | `platform/isi/replies-bb5.txt` is shared; a non-BB5 device needs a new fake peer |
| Display | 240x320 portrait (QVGA), 320x240 native landscape (E-series), 352x416 (N80/E70, 9.1), 800x352 + 240x320 dual (E90), 360x640 nHD portrait, 640x360 nHD native landscape (N97, E7), 640x480 VGA (E6) | `board.display` geometry, wsini rotation, LCD HAL build, screen modes, two screens for the E90 |
| Input | ITU-T keypad; keypad + QWERTY slide; QWERTY bar; NaviScroll ring / optical navi key; resistive single touch; capacitive multitouch; touch + QWERTY | keymap tables, `exyin` digitiser stand-in (resistive), Synaptics-behind-exyin (capacitive), slide-state keys, extra pointer devices |
| Graphics stack | software only; PowerVR MBX GLES 1.1 (OMAP2420); Broadcom VideoCore OpenVG/GLES (BCM2727 on ^3, BCM2763 on 603/700/701/808); ScreenPlay composition without a GPU [unverified: Nokia 500] | `gl.impl = sygl` entry-point patches (N95), `H-khr` libEGL/libOpenVG + `H-gcebe` composition on the host GPU (N8), nothing (RAPIDO 9.3/9.4) |
| Audio adaptation | OMAP DSP bridge (ehwa), RAPIDO shared-memory DSP (aamdaif ring), ^3 audio server over RAPIDO | the only axis where we still model two different real devices; 5800/N8 are silent by design today |
| Storage | NAND ROFS only; + mass memory (N95 8GB, N8 eMMC); + SD card; user-area FAT image (N73) | flash window size, local drive map, ESTARTCOMP, first-boot formatting |
| Nokia-specific servers | Starter + SysAp (9.1-9.3), SSM/sysstatemgr (9.4+), startup.exe state machine (^3), alf/hitchcock (N95 Carousel), alf + compositor (^3), haptics (hwrmhaptics on ^3), NFC (700/701/808), MobileCrash | each one has asked for a stand-in or an ISI reply so far |
| RAM size | 64 MB (N73, 6120c), 128 MB (N95, 5800, N97), 256 MB (N8), 512 MB (808) | `board.ram_mb`, low-memory behaviour of the N97/N73 class, unpaged core size |

Axes we deliberately do not test for: camera resolution, radio bands, colour variants, language
packs (ROFS2) and customer variants (ROFS3): one language/variant combination per firmware version.

### 1.2 Firmware versions per device

The user side changes with the platform release, not with every service pack, so:

| Rule | When | Versions to keep and boot |
|---|---|---|
| first + last | the device stayed on one platform release all its life (every S60 3rd and 5th phone) | the launch firmware and the last firmware. The first shows the launch-era servers (and is what most archived phones run); the last has every fix and is what a collector's phone usually has. Mid-life versions only when a bug depends on it |
| every platform release | the device got platform upgrades (Symbian^3 phones: PR1.0 -> PR1.1 -> PR1.2 -> Anna -> Belle -> Belle Refresh; 603/700/701/808: Belle -> FP1 -> FP2) | one firmware per release the phone shipped or received; the point release within a platform does not matter (PR1.1 vs PR1.2 counts as one when time is short) |
| main only | the device adds one axis value already covered elsewhere at the other versions, or a planned device whose lineage overlaps a reference device (701, C5, 808) | the last firmware of its series; add the launch firmware only if the device was the launch device of that platform release (808 for Belle FP1) |
| variant: optional | a type-code variant with the same user side (RM-321 China N95 8GB; N73 ME) | boot once, lowest priority; a variant counts only if it drops or adds hardware (WLAN) |

Reference devices (N95 8GB, N8) get the full treatment; everything else gets the cheapest rule that
still covers an uncovered axis value. The aim is about 30 ROM boots in total for the whole platform
range, roughly one afternoon of `tests/device-smoke.sh` runs when every profile is at L1.

### 1.3 What "tested" means per ROM

A ROM counts as covered when `tests/device-smoke.sh ID` is green (L1: idle reached within
the smoke budget, screenshot not black, no restart loop, second boot on the same drives). L2
(menu, text entry, an app round trip) is the target for the reference devices and for one device per
platform release. Higher levels (L3 telephony offline, L4 audio, L5 GL) are tracked per device in
`[status].level`, not per ROM.

## 2. Devices worked on

Values from `models/<id>/device.toml` unless noted ([bring-up] = the device's notes under
`docs/devices/<id>/`, [roms] = directory names under `roms/`). "-" = not known or not applicable.

### 2.1 Identity and platform

| Model | Type | Profile id | Platform | OS | Personality | CPU / SoC | Memory model, kernel build | RAM |
|---|---|---|---|---|---|---|---|---|
| N73 | RM-132 | nokia-n73-rm132 | S60 3rd | 9.1 | sym91 | ARM926EJ-S / TI OMAP1710 | moving, unpaged, `syborgmv` | 64 MB [unverified] |
| N95 8GB | RM-320 | nokia-n95-8gb-rm320 | S60 3rd FP1 | 9.2 | sym92 | ARM1136 r0p2 / TI OMAP2420 | multiple, demand-paged core flattened, `syborg` | 128 MB (board 168 MB) |
| N95 8GB (China) | RM-321 | nokia-n95-8gb-rm321 | S60 3rd FP1 | 9.2 | sym92 | as RM-320 | as RM-320 | 128 MB |
| 5730 XpressMusic | RM-465 | nokia-5730-rm465 | S60 3rd FP2 | 9.3 | sym92 (9.3 = 9.2 tables) | ARM1136 / RAPIDO (RAP3NAND) | multiple, paged, `syborg` | 128 MB (board 172 MB) |
| N79 | RM-348 | nokia-n79-rm348 | S60 3rd FP2 | 9.3 | sym92 | ARM1136 / RAPIDO | multiple, paged, `syborg` | 128 MB |
| 5800 XpressMusic | RM-356 | nokia-5800-rm356 | S60 5th | 9.4 | sym94 | ARM1136 / RAPIDO | multiple, paged, `syborg` | 128 MB |
| N8 | RM-596 | nokia-n8-rm596 | Symbian^3 PR1.0 | ^3 | symbian3 (our numbering + MutexWait92) | ARM1176 (ARMv6K) / RAPIDO + Broadcom BCM2727 | flexible, paged, `syborgfx` (profile key `memory_model` still says "multiple": see 4.3) | 256 MB (board 172 MB) |
| N8 (013.016 Russia) | RM-596 | nokia-n8-rm596-v13 | Symbian^3 PR1.1 | ^3 | symbian3 | as above | as above | 256 MB |

### 2.2 Display, input, graphics

| Profile id | Display (UI size, native panel) | Input | Graphics path in the emulator | Audio path |
|---|---|---|---|---|
| nokia-n73-rm132 | 240x320, portrait | ITU-T keypad | `gl.impl = none` (no GPU) | OMAP DSP bridge stand-in (H-dspbridge) |
| nokia-n95-8gb-rm320 | 240x320 UI on a native-landscape panel (wsini rotation 270, H-lcd-native-landscape) | keypad, dual slide (media keys), landscape switch | PowerVR MBX GLES 1.1 replaced: `gl.impl = sygl`, 175 entry-point patches on the ROM's `libgles_cm.dll`, syborg-gl device, host GPU backend (`--gl soft` fallback) | OMAP DSP bridge stand-in, syborg-audio (PCM/tones/MP3/AAC) |
| nokia-n95-8gb-rm321 | as RM-320 | as RM-320 | as RM-320 (patch addresses to be regenerated) | as RM-320 |
| nokia-5730-rm465 | 240x320, portrait; 320x240 on the slide states | keypad + QWERTY slide | none | RAPIDO aamdaif stand-in, silent |
| nokia-n79-rm348 | 240x320, portrait | keypad + NaviScroll ring (no stand-in) | none | RAPIDO, silent |
| nokia-5800-rm356 | 360x640 UI on a native-landscape panel (rotation 270) | resistive single touch (`exyin` stand-in on the Syborg pointer, `rotate = 270`), few keys | none | RAPIDO, silent; start-up tone patched out (H-startuptone-none) |
| nokia-n8-rm596 (+v13) | 360x640, portrait panel, Color16MAP | capacitive multitouch (Synaptics behind exyin; single touch in the stand-in), 3 keys | VideoCore replaced: our libEGL/libOpenVG (H-khr) + gcebe composition (H-gcebe), frames composed and presented on the host GPU (`[gl.host]`: msaa 8, compose host); `gl.impl = none` (no sygl patches) | RAPIDO ring model (virtual DSP clock), silent; tone patched out |

### 2.3 Firmware, level, boot time, status

| Profile id | Firmware held [roms] | Tested | Level (`[status]`) | Boot to idle | Key hacks / stand-ins | Open / next |
|---|---|---|---|---|---|---|
| nokia-n73-rm132 | 3.0717.1.4.1 (RM-132); 4.0839.42.2.1 (RM-133 N73 ME: nokia-n73-rm133, L1, English) | 3.0717.1.4.1 | L1: home screen, menu, Calendar | ~48 s first boot after the city/time/date queries  | moving-model ARM926 kernel (`syborgmv`, kernelhwsrv patch 0011, qemu-modern 0003 cache type), sym91 personality, SOS*CORE deflate reader, `iscdriver`+`ssi`+`xbus` modem link, `ehwa`/`ehwaomap` DSP, H-lightdrv | seeded drives for the smoke test; FaxModem.exe KERN-EXEC 3; RES_94 and PN_PHONE_INFO 0x15 unanswered. Rule: first + last -> obtain the launch firmware (2.06xx/3.06xx [unverified]) or treat 3.0717 as "first" and boot the N73 ME 4.0839 as "last" |
| nokia-n95-8gb-rm320 | 35.0.001 | 35.0.001 | L5-gl: idle, UI usable, telephony offline, GL on host; audio partial; no connectivity | ~26 s to Active Standby (first boot asks region/date/time) | H-cpu-arm1136, H-ram-168, TimerAt92 shim, H-dspbridge, H-dvfs, H-gles-* (blob, trampolines, present, window-fixed, identity, host), H-lcd-native-landscape, H-keysound-nommf, H-startuptone-nommf, H-secenv-cert, H-splash-behind, H-mednvmem2, H-light | keyguard emergency-number check (CALL 0x28), game audio rate, Carousel landscape under strict EGL, camera, slide/accelerometer stand-ins, networking. Rule: first + last -> the launch firmware (10.0.021 [unverified]) is missing; 35.0.001 is the last or second-to-last (35.0.002 [unverified]) |
| nokia-n95-8gb-rm321 | none | - | L0-none | - | inherits RM-320; needs regenerated code/GL patches | obtain RM-321 flash files; optional (variant rule) |
| nokia-5730-rm465 | 101.48.128 | 101.48.128 | L1: home screen and menu | 20.3 s seeded  | `nokiaiscdriver`+`sisadriver` modem link, `aamdaifdriver`, SEC_STATE/SIM_IND/lock-info replies, three start-up waits in a row | sydb agent on 9.3 start-up (sysstart/SSM list), audio, apps not tried. Rule: first + last -> 101.48.128 may be the only public version (200.12.87 [unverified]) |
| nokia-n79-rm348 | 10.046, 11.049, 30.019 | 30.019 | L1: home screen and menu | 15.3 s seeded  | 5730 profile copied; SEC_STATE_RESP length (3 bytes); keypad matrix from keypad_exp.dll; NaviScroll declared missing | boot 10.046 (first) to complete first + last; sydb agent; audio; apps |
| nokia-5800-rm356 | 11.0.008, 20.0.012, 31.0.101 | 31.0.101 | L1: home screen, menu, touch | 8.6-9 s seeded  | sym94 personality (S45 inserted, SEE..SF2), `exyin` digitiser stand-in (10 ms sampling, rotate 270), H-startuptone-none ROFS2 CenRep patch, 1 MB MDAIF buffer, F3 0x34 "all taken" | touch latency in the dialler, home-screen shortcut row, flicker in `sydb phone`, proximity sensor, sydb agent on 9.4, audio. Rule: first + last -> boot 11.0.008; the last public version (60.0.003 [unverified]) is not held |
| nokia-n8-rm596 | 011.012 (Europe), 013.016 (Russia), 111.040.1511 (Belle Refresh, no profile yet) | 011.012 | L1 (2026-11-08): home screen, first-boot wizard, clock and Photos open, some apps hang | start-up done at ~23-30 s | `syborgfx` kernel (flexible + ARM1176), `isaaccessldd` modem link, H-khr, H-gcebe, surfacemanagerdriver, usbaudiodriver, RAPIDO ring with virtual DSP clock, H-fmradio-nosession, H-haptics-fail, H-bt-ondemand, H-startuptone-none, RI patches 0005/0006, kernel without alignment checking | haptics 1C 9E, FeedbackPlayerThread -33, radioadaptationserver, 09 13 SIM; audio to host. Next ROMs: Anna 022.014/025.007 (obtain), Belle 111.030 (obtain) and the held Belle Refresh 111.040.1511 |
| nokia-n8-rm596-v13 | 013.016 | 013.016 | as above (L0-pending stale) | start-up done at 23.2 s | only addresses changed (EDISP user window, fmradioaudiocontrol -0x4390, gcebe imports) | same as 011.012 |

Firmware held without a profile (`roms/`): E52 RM-469 022.009, E72 RM-530 091.003, E5-00 RM-632
101.003, 701 RM-774 113.010.1506, N8 111.040.1511, N73 ME RM-133 4.0839.42.2.1. These are the
cheapest next steps in section 3.

### 2.4 Axis coverage today

| Axis | Covered | Not covered |
|---|---|---|
| Platform release | 3rd (9.1), FP1 (9.2), FP2 (9.3), 5th (9.4), ^3 PR1.0, ^3 PR1.1 | ^3 PR1.2, Anna, Belle, Belle Refresh, Belle FP1, Belle FP2 |
| Memory model | moving, multiple, flexible | - |
| CPU | ARM926, ARM1136, ARM1176 | Cortex-A8 (non-Nokia S60 only; optional) |
| SoC family | OMAP1710, OMAP2420, RAPIDO, RAPIDO + BCM2727 | BCM2763 generation (603/700/701/808), [unverified] Freescale (E71/6120c class) |
| Display | 240x320 portrait, 360x640 portrait panel, 360x640 on a landscape panel | 320x240 native landscape, 352x416, 640x360 native landscape with QWERTY, 640x480, dual screen |
| Input | keypad, keypad + QWERTY slide, resistive touch, capacitive touch | QWERTY bar, optical navi key, NaviScroll (declared, no stand-in), touch + QWERTY, 5th-edition capacitive (X6) |
| Graphics | software, PowerVR MBX via sygl, VideoCore via khr/gcebe | BCM2763 (expected to take the N8 path), Belle's Qt/QML home screen on top of it |
| Audio | OMAP DSP bridge (plays), RAPIDO ring (silent) | RAPIDO audio to the host |
| RAM | 64 (N73), 128, 256 | 512 MB (701, 808) |

## 3. Recommended targets

Ordered by value per effort. "Easy" = an existing family profile copied and the addresses
regenerated (one or two boots of fixes, as the N79 and 5800 were); "medium" = a new display geometry,
input device or firmware generation on a known SoC; "hard" = a new kernel-side adaptation (modem link,
DSP, display driver) or a non-BB5 modem. Firmware versions marked [unverified] are from memory.

| # | Device (type) | Adds to the matrix | Firmware to use | Difficulty | Notes |
|---|---|---|---|---|---|
| 1 | N8 (RM-596), Anna | Anna release: new browser, split-screen text entry, portrait QWERTY, Qt 4.7 preinstalled; same kernel and adaptation | 022.014 (Anna PR2.0) or 025.007 (the last Anna) [unverified]; one of them suffices | easy: `nokia-n8-rm596-v13` showed that a new N8 firmware costs only addresses | obtain the flash files |
| 2 | N8 (RM-596), Belle and Belle Refresh | Belle: new home screen (widgets, Qt/QML), pull-down status menu, new system apps; Refresh: the last ^3 generation. Possibly the first ROM where composition load matters | 111.030.0609 (Belle) [unverified], 111.040.1511 (Belle Refresh, held) | easy to medium: if Belle Refresh boots, plain Belle may be skipped (every platform release rule, point release does not count) | the owner's intent: every ^3 generation on the N8 |
| 3 | 701 (RM-774) | Belle FP2 as a shipped generation; BCM2763 VideoCore (second generation); 1 GHz ARM11; 3.5" IPS 360x640 capacitive; NFC; 512 MB RAM [unverified] | 113.010.1506 (Belle FP2, held, last); optionally 111.030 (Belle launch) if the N8 Belle boot fails for a reason specific to the 701 | medium: N8 profile, new ROM addresses, new VideoCore driver names (vchi/dispman for BCM2763: the khr/gcebe path should not care), NFC stack to be told "no chip" | "main only" rule; the 603 and 700 add nothing |
| 4 | E72 (RM-530) | 320x240 native-landscape QVGA E-series; QWERTY bar; optical navi key; S60 3rd FP2 on a late 600 MHz RAPIDO; E-series start-up (Mail for Exchange, encryption, device lock) | 091.003 (held; the last or close to it [unverified]); the launch 021.024 [unverified] only if a landscape-specific bug needs the older window server | easy to medium: 5730 profile, display `native_orientation = landscape` at rotation 0, a keymap for the full keyboard, the optical key as a pointer-free navi device | prefer over E52: the E52 (022.009 held) is a 240x320 keypad phone, i.e. the N79 again with more MHz |
| 5 | 808 PureView (RM-807) | Belle FP1 as a launch platform; 1.3 GHz ARM11; 512 MB RAM; BCM2763; 4" nHD ClearBlack AMOLED; the last Symbian phone; largest unpaged core | the launch Belle FP1 (112.020.x [unverified]) and 113.010.1506 (Belle FP2, last) | medium: as the 701 plus `board.ram_mb` 512+, camera stand-in irrelevant for L1/L2 | obtain both; the camera app will fail without the 41 MP adaptation, acceptable |
| 6 | N97 (RM-505) | S60 5th on a 640x360 native-landscape panel with a QWERTY slide and a slide state; 128 MB low-memory behaviour (the N97 is the stress test for RAM); first-generation 5th firmware | 10.0.012 (launch) [unverified] and 22.2.110 (last, 2011) [unverified] | easy to medium: 5800 profile, landscape panel, slide keys, keyboard matrix | the N97 mini (RM-555) adds nothing |
| 7 | X6 (RM-559) | the only S60 5th phone with a capacitive screen (Synaptics path on 9.4) | 20.0.005 (launch) and 40.0.002 (last) [unverified] | easy: 5800 profile with the N8's touch stand-in configuration | if time is short, boot only the last |
| 8 | C5-00 (RM-645) | the last-generation S60 3rd FP2 candybar (2010-2011 firmware era on 9.3); 600 MHz RAPIDO; small 2.2" 240x320; cheap and very common | 071.005 (last) [unverified]; the launch 031.022 [unverified] optional | easy: N79 profile | Which C5: C5-00 (S60 3rd FP2, keypad) is the one that fits the 701/808 "main ROM" set, because 9.4 is already covered by the 5800 and will be by the N97/X6. C5-03 (RM-697, S60 5th, 3.2" resistive, 600 MHz) is "easy, adds only a later 5th-edition firmware generation (12.0 -> 23.0)" and can wait. The C5-00 5MP (RM-745) is a hardware refresh with its own 09x firmware; skip |
| 9 | E7 (RM-626) | Symbian^3 with a 4" 640x360 native-landscape panel, capacitive touch plus QWERTY slide (touch + keyboard on ^3), HDMI | the ^3 launch version (PR1.1 class, 013.x/014.x [unverified]) is optional since the N8 covers ^3; use Belle Refresh 111.040.1511 [unverified for the E7] as main | medium: N8 profile, landscape panel, slide and keyboard | the C7 and C6-01 (same chipset, portrait panels) add nothing |
| 10 | E6 (RM-609) | 640x480 VGA touch screen (the only one), portrait QWERTY bar + capacitive touch, Anna as the launch platform (021.x) | 021.014 [unverified] (Anna launch) and 111.140.0058 (Belle Refresh for the E6) [unverified] | medium: N8 profile, VGA geometry (new wsini modes, Avkon VGA layouts), keyboard | covers "Anna launch device" if the N8 Anna boot is enough for the release itself, the E6 can be main-only (last) |
| 11 | E71 (RM-346) | S60 3rd FP1 on a non-OMAP SoC [unverified: Freescale MXC300-30, 369 MHz ARM11]; 320x240 landscape QWERTY bar on 9.2; the E-series FP1 user side | 100.07.76 (launch) and 510.21.009 (last) [unverified] | unknown until `device.py identify` lists the kernel-side drivers: easy if it is RAPIDO-like (nokiaiscdriver + sisadriver names), hard if a third modem-link/DSP driver set appears | this is the one axis question only a firmware answers; do the identify step before committing to it |
| 12 | 6120 classic (RM-243) | the cheapest, most common FP1 phone: 2.0" 240x320, 64 MB RAM, same SoC question as the E71 | 3.70 (launch) and 7.10 (last) [unverified] | same as the E71; take only one of E71/6120c unless the drivers differ | 64 MB on 9.2 is a RAM stress point the N73 covers only on 9.1 |
| 13 | N93 (RM-153) or N80 (RM-92) | Symbian 9.1 on ARM1136/OMAP2420, i.e. the multiple memory model with the sym91 personality (today sym91 exists only for the moving model); N93 adds PowerVR MBX GLES on 9.1, N80 adds the 352x416 resolution | last firmware of each (N93 20.0.058, N80 5.0719.0.2 [unverified]) | medium: N95 profile + sym91; sygl patches for a 9.1 `libgles_cm.dll` | one of the two is enough; N93 if GL on 9.1 matters, N80 if the odd resolution does |
| 14 | E90 Communicator (RM-42) | dual display (800x352 internal, 240x320 cover), full QWERTY, S60 3rd FP1 on OMAP2420 | 400.34.93 (last) [unverified] | hard for the display: a second screen number in WSERV and the display stand-in; the rest is the N95 | optional; the only way to test two screens |
| 15 | E5-00 (RM-632) | late FP2 (2010) QWERTY bar, 256 MB RAM? [unverified], 101.003 held | 101.003 (held) | easy once the E72 runs | fill-in: boot because it is already on disk, not because it adds an axis value |
| 16 | Nokia 500 (RM-750) | Anna -> Belle on a 1 GHz ARM11 reportedly without a GPU [unverified]: ScreenPlay composition in software, i.e. the N8 path with gcebe only | 111.020 (Belle) [unverified] | medium if the GPU claim holds | optional; verify the graphics adaptation from the firmware (no vchi/vcdriver = software) before spending time |
| 17 | Samsung i8910 Omnia HD | S60 5th on TI OMAP3430 Cortex-A8 with PowerVR SGX530; non-Nokia modem; 3.7" 360x640 capacitive | last firmware [unverified] | hard: new container, new modem front end (no ISI), new kernel-side driver set, Cortex-A8 kernel build | the only way to cover Cortex-A8 and SGX under S60; out of scope until a non-Nokia modem is wanted |

Devices considered and dropped: N82 and N95 classic (OMAP2420 FP1: the N95 8GB again), N85/N86 (FP2
on an ARM11 [unverified SoC]: covered by N79/5730 unless the SoC differs), 5230/5530/5235 (5800
again), C6-00 (5th + QWERTY slide: the N97 covers it with less RAM), C7/C6-01/X7 (N8 chipset with
other screens), 603/700 (701 chipset with smaller screens), E52/E55 (N79-class FP2 keypad with more
MHz; E52 022.009 is on disk and can be booted as a fill-in like the E5-00), N96 and 6220 classic
(FP2 keypad phones; the N96's SoC [unverified] would be the only reason), E61/E61i/3250 (9.1
landscape QWERTY / keypad on OMAP1710: the N73 covers 9.1 moving model; the E61 would add 320x240 on 9.1 only).

### 3.1 Resulting test set

With the targets above the set is about 32 ROM boots:

| Platform release | ROMs |
|---|---|
| S60 3rd (9.1) | N73 3.0717 (held) + N73 launch or N73 ME 4.0839 (held); N93/N80 last |
| 3rd FP1 (9.2) | N95 8GB launch (obtain) + 35.0.001 (held); E71 first + last; 6120c last (if its SoC differs); E90 last (optional) |
| 3rd FP2 (9.3) | 5730 101.48.128 (held); N79 10.046 + 30.019 (held); E72 091.003 (held); C5-00 last; E52/E5-00 (held, fill-in) |
| 5th (9.4) | 5800 11.0.008 + 31.0.101 (held); N97 first + last; X6 last |
| Symbian^3 PR1.x | N8 011.012 + 013.016 (held); E7 main |
| Anna | N8 022.014 or 025.007; E6 launch or last |
| Belle, Belle Refresh | N8 111.030 (optional) + 111.040.1511 (held); E6 Refresh (optional) |
| Belle FP1 | 808 launch |
| Belle FP2 | 701 113.010.1506 (held); 808 113.010.1506 |

Order of work: 1-3 (N8 Anna/Belle, 701: the ^3 lineage the owner asked for first), then 4 and 15
(E72, E5-00: on disk, FP2 landscape QWERTY), 5 (808), 6-7 (N97, X6), 8 (C5-00), 9-10 (E7, E6),
11-12 (E71 or 6120c after the identify step), 13 (N93/N80), 14 (E90), 16-17 optional.

## 4. Maintenance

### 4.1 Source of each field

| Field | Source | How |
|---|---|---|
| model, type code, profile id, aliases, market | `device.toml` `[device]` | `tools/device.py get ID device.model` etc. |
| platform release, OS version, personality, SoC | `[platform]` `ui`, `os_version`, `personality`, `soc` | automatic |
| CPU, RAM, kernel build | `[board] cpu`, `ram_mb_real`, `[kernel] baseport` | automatic |
| memory model, paging | `[firmware.rom] memory_model`, `paged` | automatic (see 4.3 for the N8 drift) |
| display | `[board.display] width`, `height`, `native_orientation`, `touch`, `bpp` | automatic |
| input | `[board.display] touch`, `[hardware] slides`, `keymap.toml` (keyboard matrix size), stand-in names `exyin`/`naviscroll` | automatic, derived: `touch` + `slides` + the presence of `exyin` give the input class |
| graphics path | `[gl] impl`, `[board.gl] backend`, `[gl.host]`, stand-in names (`surfacemanagerdriver`), hack ids `H-khr`/`H-gcebe` in comments | automatic for `impl`/`backend`; the ^3 path (khr/gcebe) is recognisable from the stand-in list |
| audio path | `[board.audio] dsp`, stand-ins `ehwa`/`aamdaifdriver` | automatic |
| firmware version profiled | `[firmware] version` | automatic |
| firmware versions held | `roms/<TYPE>_<Model>/<version>/` directory names (and `roms/_archives/` file names) | automatic: match `<TYPE>` to `device.type_code`; the archive list shows versions not yet unpacked |
| firmware versions tested, level, open items, date | `[status] level`, `updated`, `open` | automatic, but `level` is hand-maintained and drifts (N8); the smoke test could write `[status] smoke = { version, date, result }` |
| boot time | measured only (`expected_idle_s` was removed from the profiles 2026-11-08: boot time depends on the host) | the budget is automatic; the measured value lives only in a comment and in the bring-up notes: make it a key (`measured_idle_s`, written by `tests/device-smoke.sh`) |
| key hacks and stand-ins | `[[standins]] name`, `H-` ids in profile comments, `docs/hacks.md` | stand-in names automatic; hack ids need a key (`[status] hacks = [...]`) or a scan of the comments |
| first/last firmware of the model, platform lineage, rule applied | nowhere yet | hand-written: proposed `[matrix]` table in the profile (4.2) |
| why the device is in the set (axis values) | nowhere | derived from the automatic fields against an axis list (4.2) |
| recommended targets (section 3) | this file | hand-written |

### 4.2 Proposed generator: `tools/device.py matrix`

Design only; nothing implemented.

- `tools/device.py matrix [ID ...] [--md | --json] [--roms DIR]` prints sections 2.1-2.3 from the
  profiles and the `roms/` tree. `--md` writes the tables between the markers
  `<!-- matrix:auto -->` and `<!-- matrix:end -->` of this file; everything outside them stays as
  written. Per-device rows use the automatic fields of 4.1; a variant (`extends`) is one row that
  names its base.
- `tools/device.py matrix --coverage` prints section 2.4: for each axis in a small static list
  (`platform/matrix-axes.toml`: axis name, the ordered value list, the profile key or derivation
  that yields a device's value), the values covered by at least one profile at L1 or better, and
  the values with no device, so a newly added profile shows what it bought.
- New optional profile table, hand-written:

      [matrix]
      role = "reference"              # reference | axis | fill-in | variant
      rule = "every-release"          # first-last | every-release | main | optional
      lineage = { first = "011.012", last = "111.040.1511" }    # the model's public firmware range
      releases = { "symbian3-pr1.0" = "011.012", "symbian3-pr1.1" = "013.016", anna = "025.007", belle = "111.030.0609", "belle-refresh" = "111.040.1511" }
      tested = ["011.012"]            # versions with a green smoke test; written by tests/device-smoke.sh

  From `lineage`/`releases` and the `roms/` tree the generator lists what the rule still requires
  ("missing for first-last: 10.0.021"), which is the to-do list of section 3.1 kept automatically.
- `tests/device-smoke.sh` appends the result to `[matrix] tested` and writes
  `[milestones] measured_idle_s` (or a sidecar `build/roms/<id>/<fw>/smoke.toml` if the profile should
  stay free of generated values; the generator reads either).
- `tools/device.py status ID` is named in `docs/adding-a-device.md` section 6 but is not in the
  current subcommand list (`list check show paths qemu-args get build identify gen`); `matrix` would
  subsume it.

### 4.3 Drift the generator would have caught today

- (fixed 2026-11-08) `nokia-n8-rm596` `[status]` and `expected_idle_s`: status set to L1, the key removed from all profiles.
- (fixed 2026-11-08: now "flexible") `nokia-n8-rm596` `[firmware.rom] memory_model = "multiple"` while `[kernel] baseport = "syborgfx"`
  and the bring-up notes say flexible (same ROM base and kernel data address, told apart by the DLL
  data at 0x7f000000). Either the key is stale or it does not mean what the matrix needs; the
  generator should take the memory model from the kernel build name until this is settled.
- Held firmware without a profile (E52, E72, E5-00, 701, N8 Belle Refresh, N73 ME) is invisible to
  `device.py list`; matching `roms/` type codes against profiles lists them.
- The N95 8GB and the 5800 are tested at one version only although the first + last rule applies and
  the 5800's launch-era 11.0.008 is on disk.
