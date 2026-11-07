# Adding a new S60 device

The flow for a human with an LLM agent (Claude Code with the sydb MCP server). It assumes the
refactor in [refactor-plan.md](refactor-plan.md) is done at least through step 5; where a step is
not done yet, the N95 equivalent is named in brackets. The device model is in
[devices.md](devices.md). Budget your agent: each phase below is a separate brief with its own
tool-call budget, as `CLAUDE.md` asks.

## 0. Before you start

- A Linux host with the project built once (`scripts/build-all.sh`, `scripts/build-qemu-modern.sh`,
  `src/apps/devagent/build.sh`) and the N95 booting to idle (`sydb start --seed
  build/runs/mtc-drives`), so you know the platform works before you touch it.
- **Your own copy of the phone's firmware** (the flash files for its type code and a software
  version), e.g. what a Nokia Software Updater or Phoenix install left on disk. Put them in
  `firmware/<id>/` (git-ignored). Nothing from them is ever committed: not bytes, not screenshots
  of the firmware's UI (`build/evidence` for those), only checksums, names, addresses and sizes.
- Know the phone: type code (RM-xxx), Symbian version and S60 edition, SoC family, screen size,
  keys and slides, radios. Nokia's own spec sheet and the firmware's `hal.dll`/`wsini.ini` give
  the rest.

## 1. Scaffold the profile

```
tools/device.py new nokia-e72-rm530 --from nokia-n95-8gb-rm320
```
[Before the tool exists: copy `devices/nokia-n95-8gb-rm320/`, set `[device]`, `[platform]`, empty
`[status]`, keep `extends` *unset* unless it is a true variant of an existing device.]

Fill what you know without the firmware: `[device]`, `[platform]` (`personality` = the nearest:
`sym92` for 9.2, `sym93` for 9.3, …), `[hardware]`, `board.display.touch`, `board.cpu`
(`arm1136-r2` for OMAP2420/2430 phones, `cortex-a8` for OMAP3), `ram_mb_real`.

## 2. Inventory the firmware

```
FW_DIR=firmware/nokia-e72-rm530 tools/fw/inventory.py --device nokia-e72-rm530
```
Outputs in `build/devices/<id>/`: `core-flat.img` (unpaged XIP core), `core.json`/`core-files.txt`
(directory), `rofs*.img`, `symindex.json`, `abi/` (EUSER SWI map, vs ours; kabi report), and a
**kernel-side driver list** (every LDD/PDD/extension in the firmware's ROM) which is the raw
material for `[[standins]]`. It writes `devices/<id>/firmware.sha256` and the `[firmware.rom]`
numbers (base, kernel data, paging, user window start) into the profile.

If the container is not FIASCO/BB5 the tool stops here: write a short research brief for the
container format (mock-level: partition table layout only) before going on. Supported inside
FIASCO: plain `SOS+CORE`/`SOS+ROFS` (N95), compressed `SOS*CORE` with ROFS1 after it
(`compression = "sos-deflate"`, partitions `core` + `rofs-after-core`: N73), partition-less files
(`partitions = { rofs = "_pre" }`).

**Then read the ROM header before anything else** (`tools/fw/probe.py --device ID header`):
`romBase` and `kernData` decide the kernel build. 0x80000000/0xC8000000 = multiple memory model
(`kernel.baseport = "syborg"`); 0xF8000000/0x64000000 = moving model, ARMv5 (`syborgmv`, generated
by `tools/baseport_variant.py`). Also check whether the firmware's kernel-side and user-side files
are one block each or interleaved (`kernel.place = "after-firmware"` for the latter) and whether
user EXEs have data in the kernel section (moving-model fixed processes). `probe.py header` prints
the memory model, the last kernel-side file, the lowest user-side file (`user_window_start`) and the
fixed processes.

## 3. Check the personality

```
tools/personality/check.py --device nokia-e72-rm530
```
- Zero unknown SWIs and zero vtable deltas: keep `platform.personality` as set.
- A few: add them under `[personality.overrides]` in the profile (exec lines in `exec.map`
  syntax; class deltas in `kabi.toml` syntax) — the generator merges them.
- Many, or a different memory model: fork `personalities/<nearest>/` into a new one named after the
  OS version, note the derivation in its README (which EUSER, which cross-check), and set
  `platform.personality` to it. This is a kernel rebuild (`scripts/build-kernel.sh`).

Beyond the numbering, check (method in `personalities/sym91/README.md`; the first two and the
copy-in sizes are one command each: `tools/fw/probe.py --device ID stubs --vs <known device>` and
`probe.py --device ID exec-sizes --vs <known device>`):
- the **ordered stub sequence** of both EUser.dll (decisive; export pairing is noisy);
- the **stub shape**: `swi; bx lr` (9.2+) or `mov ip, lr; swi` (9.1: the kernel returns via ip);
- exports that **branch straight to a stub** without setting r1..r3 (fewer arguments than ours);
- **loader structures**: copy-in sizes of CodeSegCreate/ProcessCreate in the phone's own ekern
  (find its slow exec table: a count word, two handler words, then {flags, handler} pairs);
- how the loader indexes **ReadExportDir**'s buffer (ordinal 0 included or not).

## 4. Describe the hardware from the firmware

Where to look, with the tools in `docs/tools.md` (`syb_sym.py` works on any device's symindex):
- **Display**: `Z:\system\data\wsini.ini` in the ROFS (`tools/fw/probe.py --device ID display`) gives the UI size and
  rotation; the display driver's caps (decompile the firmware's `d_display`-like LDD once; the
  N95 analysis is `docs/devices/nokia-n95-8gb-rm320/nokia-d_display.md`) give native orientation, pitch, modes. Fill
  `[board.display]` and `[kernel.lcd]`.
- **Keys**: the firmware's keypad driver matrix table (N95: `keypad.dll` at 0x8014f6d4) and
  `EKData.dll` list the EStdKey codes; the spec sheet names the keys. Write `keymap.toml`
  (`tools/device.py check` verifies the EStdKey names). Slides and the camera half-press are
  just more codes at this stage.
- **Timing**: the firmware's `hal.dll` reports `EFastCounterFrequency` and `ENanoTickPeriod`;
  the board must match (`[board.timing]`). The N95's are 32768 Hz / 1000 µs.
- **Drives**: the firmware's `Z:\sys\data\ESTARTCOMP.TXT` (or `ESTART.TXT`; `probe.py estart`) maps letters to local
  drives; write `[[board.drives]]` and the device's `ESTARTCOMP.TXT` with our local drive
  numbers (0 IRAM, 1 NVMemory, 2 second NVMemory, 8 composite ROFS). CR LF line endings.
- **Stand-ins**: for each driver in the inventory's kernel-side list, look for an existing
  stand-in of the same name in `src/standins/` (families: `common`, `nokia-bb5` for BB5-era
  modem/security/light drivers, `nokia-omap2` for display/DSP/DVFS). Same name is **not** enough:
  compare the client DLL's side first (the control/request numbers it issues, buffer sizes it
  reserves, the interface version it opens with). The N73 differed in all three places (display
  caps 23 vs 26 words, Custom 1.0 vs 2.0, DSP bridge). A wrong mock is worse than a missing driver:
  unknown → leave it out and let the boot loop tell you whether anything misses it
  (`[standins.missing]`).
- **Modem**: a BB5 phone takes `family = "bb5-s60"` (`[modem]` knobs and the shared replies in
  `platform/isi/replies-bb5.txt`: MTC, security, self-test, RTC, light, energy management); its own
  `isi-replies.txt` starts empty. Answers this phone needs differently go into its file (the later
  layer wins); if one of the shared replies is wrong for it, move that reply out of the shared file
  into the other members' files (families rules, `docs/devices.md`). Unsolicited messages it waits
  for go into `[modem].indications`; every channel subscribed to that message gets it, so check that
  each subscriber (SAServer and the TSY may parse it differently) accepts the layout.
- **Family values in general**: start from the family, but whatever this phone does differently
  leaves the family for every member's profile. Never keep a false family value and override it.
- **GL**: if the phone has a PowerVR/SGX GPU and a `libgles_cm.dll`, set `[gl]`: ordinals from the
  matching SDK `.dso` (names only), `imports` = the N95 list to start with (the tool resolves the
  addresses from this device's symindex and fails on missing exports), identity strings from
  the firmware's driver DLLs (`syb_sym.py str`), `blob_addr` = a free gap below the user window
  (the composer refuses overlaps). Otherwise `gl.impl = "none"`.

## 5. Build and boot loop

```
tools/device.py build nokia-e72-rm530 --kernel   # kernel side (the profile's baseport), stand-ins, compose, DTB
export SYBORG_DEVICE=nokia-e72-rm530; S="--session build/sydb-e72"
sydb $S start --fresh --catch-panics --timeout 120   # paused start + freeze/snapshot catch + resume + wait
sydb $S catch hits                                # thread, PC, caller, stack scan of the first panic
```
The long form, for more catches or breakpoints before the first instruction:
`sydb $S start --fresh --paused; sydb $S catch add --kind panic --action freeze --snapshot;
sydb $S dbg break ...; sydb $S resume; sydb $S wait 'FAULT|<idle milestone>' --timeout 120`.
**Start every early boot paused with catches armed.** Early faults happen in the first second,
before a catch can be added to a running session; the catch file (and `sydb dbg break` on the
suspect exec stub or kernel function, with the device's own kernel symbols) replaces reading
register dumps. On the N73 this turned each fault into one boot.
Then iterate with what the platform already has (all in `docs/tools.md` / `docs/sydb.md`):

| Symptom | First tool | Typical fix |
|---|---|---|
| Serial stops early, no user processes | `sydb log --grep 'FAULT|EXEC92'` | unmapped SWI (personality override), loader struct size, missing extension |
| A process restarts forever / Starter waits | `tools/syb_report.py <session>/serial.log` → "IPC requests still pending" names the server; `sydb strace NAME` | a server blocked on a driver: add/extend a stand-in |
| `ISCPEER` lines with no reply | `tools/isi.py log <serial.log>` lists unanswered requests | one `reply` line in `isi-replies.txt` (same resource as the N95: copy its reply; new: research brief with the message layout only) |
| A thread dies | `sydb catch add --kind panic --thread X --action freeze --snapshot`, then `sydb dbg bt`, `sydb fork` to replay | stand-in return value, code patch (last resort, documented), kernel shim |
| The UI never shows | milestones (`wserv`, `startup_ui`) tell which phase; `sydb screenshot`, `sydb wg` | display caps, light/HWRM replies, DRM certificate, startup property |
| Keys do nothing | `sydb key`, serial `EKEYB`/WSERV traces | keymap scan codes |
| UI up but timers wrong, apps too fast | `[board.timing]` vs the firmware's HAL | match the fast counter and tick |

Rules while looping:
- Every shortcut goes into the device's `README.md` hacks table (or `docs/hacks.md` when it is
  platform-wide), with the file and the "real implementation" column, the day it is taken.
- Byte patches on the firmware are the last resort; each one is a hack id, with original bytes
  verified at compose time.
- Prefer `sydb catch`/`snap`/`fork` over breakpoints on hot paths (timing).
- Update `[milestones]` as the device's log lines differ from the N95's (`idle` is what
  `sydb start` waits for; `expected_idle_s` bounds the smoke test).
- Record the missing tool in `docs/tooling-ideas.md` after a long loop.

## 5a. What to expect (patterns seen so far, N95 and N73)

These recur from phone to phone; check them before chasing symptoms.

- **Personality gaps show up as odd behaviour, not clean errors.** A call your map marks `!invalid`
  but the firmware uses returns 0 silently. Run the "used but invalid" check (every SWI the
  device's EUSER issues must map to something) and `tools/personality/exec_sizes.py` (copy sizes of
  every exec handler, the phone's own kernel vs a known one) before the first boot.
- **CPU generation changes the exec set.** ARMv5 phones (no LDREX) do atomic operations
  (`User::LockedInc/LockedDec`, used by RFastLock/RCriticalSection) through fast execs that ARMv6
  EUSERs never call. Missing ones deadlock servers: symptom = white or frozen screen, the CPU idle
  in the null thread, FbServ/Wserv blocked.
- **A blank screen with the frame buffer filled is not a display problem.** Check the fb registers
  and memory (`sydb hmp 'xp /20wx 0xc0005000'`): if the window server cleared it, the UI side is
  blocked; find who waits on whom.
- **"Who waits on whom" needs the IPC trace.** Compose a second ROM with `--trace 0x80000800`
  (KIPC) and read `tools/syb_report.py`'s "likely blocked" list. A server blocked in a *synchronous*
  call stops reading everything else, so a missing modem reply on one channel can freeze a
  different subsystem. Line up the blocked request's time with the `ISCPEER: ... send` lines around
  it to find the unanswered message.
- **The fake modem is per firmware generation.** Same resources, different message numbers (older
  phones use older numbering), extra start-up questions (e.g. a SIM-changed check), different
  first-boot sequences. Unanswered requests (`tools/isi.py log`) that appear *before* a stall are the
  ones to research; many others are harmless (the N95 leaves 15 kinds unanswered).
- **Driver protocols drift too.** The same stand-in name can serve a client that renumbers one
  control (DSP bridge shared buffer) or changes one call's contract (image load). The stand-in log's
  `ignored` lines for a known driver are the hint; declare the variant in the profile
  (`[[standins]] ... config = { ... }`, read by the stand-in from the board config) instead of
  guessing it from the client's arguments.
- **Kernel "Exc 1" dumps from `K::USafeReadAndParseDesHeader` with tiny addresses are noise**:
  integers flagged as descriptors in IPC; the kernel handles them.
- **First boot asks questions** (city, time, date, tutorial): the idle milestone comes only after
  they are answered (LSK/RSK). The smoke test needs drives from an answered first boot.
- **The UI language is the firmware's language pack**: an archive's ROFS2/ROFx may hold only some
  languages (check `rofs.py rofs2.img | grep -oE '\.r[0-9]+$' | sort | uniq -c`).
- **Devices that share a kernel baseport share its guest keyboard fallback table**
  (`syborg_keyboard_map.h`, generated at `--kernel` build time): the last device built wins. With
  `translation = "qemu"` it only covers keys the QEMU keymap does not map, but rebuild a device's
  kernel side (`--kernel`) after building another one on the same baseport
  (`tests/device_py_test.py` checks the default device's).
- **Research in parallel.** A mock-level research agent (CLAUDE.md rules) on one protocol question
  while you keep booting keeps the main loop short; give it the exact request bytes and log lines.

## 6. Verify: the "done" levels

Each level is a checklist that an agent can run through sydb; `tools/device.py status ID` prints
the level from `[status]`. Levels apply only where `[hardware]` says the phone has the part.

| Level | Means | How to check |
|---|---|---|
| **L0 kernel-side** | our kernel + the firmware's file server and EStart run; user processes start | serial: `efile`, `EStart`, `ewsrv` created; no `KERN` fault in the first 60 s |
| **L1 boots to idle** | the home screen is up and stays up; sydb agent answers | `sydb start` reaches `milestones.idle` within `expected_idle_s` × 1.5; `sydb screenshot` is not black; 10 minutes without a restart loop; a second boot on the same drives (settings persisted) |
| **L2 UI usable** | menu, navigation, text entry, keyguard, an app round-trip | `sydb key menu`, launch Calculator/Notes/Clock/File manager, type text, `sydb unlock`, settings survive reboot, **Switch off** ends QEMU (fake-modem power path) |
| **L3 telephony offline** | the phone is consistent about having no SIM/network | Offline profile selected without errors, no periodic "operation time-out" notes (Autolock, HWRM), battery/charger indicators sane, clock set from the RTC and kept across reboots, emergency-number checks answered |
| **L4 multimedia** | audio out, images, video, camera where modelled | startup tone and key tones, music player plays an MP3 from E:, Gallery shows an image, video playback (DSP stand-in nodes), camera app opens on a stand-in (test pattern) |
| **L5 GL** | GLES apps render through sygl | the phone's own GL UI (Carousel on the N95) and a known GL test app render on both backends (`--gl host` / `--gl soft`) |
| **L6 connectivity** | WLAN/BT/packet data against fake peers | not built for any device yet; needs the networking research in `docs/research/tls-modern.md` |

"Done for the platform" = L1 with the smoke test green (`tests/device-smoke.sh ID`), the profile
passing `device.py check`, and the device README listing its level, hacks and open items. Higher
levels are logged in `[status].level` as they are reached.

## 7. Document and hand over

- `devices/<id>/README.md`: identity, firmware version used, level, boot time, the hacks table,
  open items, the stand-ins reused/added, where the research notes are (`docs/devices/<id>/`).
- A line in `docs/roadmap.md` under the device list; the smoke test added to the CI list.
- Local commits only (project rule). The commit must not contain anything under `firmware/` or
  `build/`; `git status` before committing.

## 8. Reference: build environment, ROM anatomy, code patches, drives

Facts an agent needs while doing steps 2-6, in one place.

**Build environment (containers, podman or docker; `ENGINE=docker` switches).**
- Legacy image (`container/Containerfile`, Debian squeeze i386): Symbian host tools, our EKA2 kernel
  (GCCE 4.4.1), the Syborg baseport and the stand-ins. Every script that needs it re-runs itself in
  it (`scripts/build-kernel.sh`, `build-syborg.sh`, `build-standins.sh`, `tools/device.py build
  --kernel`); first use builds the image. Inputs: `scripts/fetch.sh` (pinned in `sources.lock`).
- Modern image (`container/Containerfile.modern`, Debian trixie): modern QEMU with the Syborg board,
  GCC 14.2 `arm-none-symbianelf` + binutils 2.29.1 (`/opt/symbianelf`, `$GCC14`), elf2e32_next
  with `patches/elf2e32_next/` (`$ELF2E32`), Python tools, mtools, gdb-multiarch.
  `scripts/build-qemu-modern.sh --container` builds QEMU into `build/qemu-modern-ct/`;
  `scripts/run-modern.sh` (and so sydb) runs it through `scripts/qemu-container.sh` when there is no
  host build (or `SYBORG_QEMU=container`). Guest code with GCC 14:
  `S60SDK=/path/to/S60_3rd_FP1/Epoc32 CONTAINER=modern scripts/in-container.sh src/sygl/build.sh`
  (the SDK is proprietary: mounted read-only, never in an image).
- Host builds still work when the host has the packages listed in `Containerfile.modern`.

**ROM anatomy** (`tools/romcompose.py`, driven by `tools/device.py build`). One XIP image at the
firmware's ROM base:
1. our ROM header, bootstrap and kernel side (`build/devices/<id>/kernel-side.img`: EKA2 kernel,
   Syborg baseport, the profile's `[[standins]]`), below the firmware's user side
   (`kernel.place = "below-user-side"`) or after the firmware image (`"after-firmware"`);
2. the firmware's own core image bytes, verbatim at their original addresses (every import thunk
   and DLL reference stays valid);
3. a new root directory and exception search table; header fields patched (secondary = the
   firmware's `efile.exe`, the personality number, trace mask);
4. `[rom] files` replace or add data files (`Sys/Data/ESTARTCOMP.TXT` = our drive map);
5. `[rom] code_patches` byte patches, then the GL blob if `gl.impl = "sygl"`.
The ROFS images do not go into the ROM: they go into `<rom>.rofs`, loaded at the board's flash
window and served by `medrofs` (`docs/devices.md` §2.3). `hybrid-manifest.txt` lists every file
with its source (ours / firmware).

**Code patches** (`devices/<id>/code-patches.txt`, last resort after stand-ins and kernel fixes):
```
# H-<hack-id>: why, what the original code does, what the patch makes it do
82f0e366  01f0a2eb  0020c046   # module and function  [H-hack-id]
```
Columns: ROM address, the original bytes (memory order; Thumb halfwords little-endian), the new
bytes. The composer refuses an entry whose original bytes do not match, so a patch never applies to
the wrong firmware. Every entry is a hack in `devices/<id>/hacks.md` (device) or `docs/hacks.md`
(platform). `[rom.variants.<name>] omit_patches = ["H-id"]` builds a ROM without some of them
(`tools/device.py build ID --variant NAME`). Find the address with `tools/syb_sym.py`, the bytes
with `tools/xipdis.py`.

**Drives.** `[[board.drives]]` + the device's `ESTARTCOMP.TXT`: C: and E: are FAT images on the
Syborg NVMemory devices (`nvmem`, `nvmem2`), D: is RAM, Z: is the ROM + ROFS composite.
- Images are plain FAT files, created sparse when missing; the phone formats them on its first
  boot. `scripts/run-modern.sh` uses throw-away images unless given `--nvmem/--nvmem2`; a sydb
  session keeps its own in `<session>/drives/nvmemory.img` (C:) and `nvmemory2.img` (E:);
  `sydb start --seed DIR` copies a prepared set (e.g. a phone past its first-boot questions).
- Copy files with the phone running: `sydb push LOCAL 'C:\data\x'`, `sydb pull`, `sydb ls`
  (through the agent on the phone, `docs/sydb.md`).
- With the phone off: mtools on the image. `DRIVE_C=<image> scripts/n95-drive.sh put c FILE DIR`
  (also `ls`, `get`, `mkdir`, `rm`; `e` with `DRIVE_E=`), or directly
  `MTOOLS_SKIP_CHECK=1 mcopy -i nvmemory.img FILE ::/data/`. Never while a QEMU has the image open
  (two writers corrupt the FAT; the scripts refuse).
- `sydb install` puts the sydb agent and its start-up record on C: (mtools, phone off).
- Never use the user's personal phone images for tests: make a session (`sydb --session
  build/sydb-<name> start --fresh`).

## Checklist (copy into the device README and tick)

- [ ] firmware obtained, `firmware/<id>/` populated, `firmware.sha256` written, nothing proprietary staged
- [ ] `tools/fw/inventory.py` ran: `core-flat.img`, `core.json`, `rofs*.img`, `symindex.json`, `abi/`
- [ ] personality checked: unknown SWIs = 0 or overridden; kabi report clean or overridden
- [ ] `device.toml` filled: identity, platform, firmware, board (display, timing, drives), hardware, kernel, standins, modem, rom, gl, milestones
- [ ] `keymap.toml` written; `device.py check` green
- [ ] `device.py build` composes without overlap errors
- [ ] L0: user processes start
- [ ] L1: idle screen, agent up, persists across reboot, smoke test green
- [ ] L2: UI round trip, keyguard, switch off
- [ ] L3: offline telephony consistent
- [ ] L4 / L5 / L6 as the hardware allows
- [ ] hacks documented with file paths; `docs/tooling-ideas.md` updated if a tool was missing
- [ ] `devices/<id>/README.md` and the roadmap line written

## Firmware identity (implemented)

After extracting the firmware into `build/devices/<id>/`, run `tools/device.py identify` (or
`identify DIR`): it prints the firmware's own `sw.txt` / `langsw.txt` (version, date, type code), its
WLAN drivers, and which profile matches. Copy the values into `[firmware.identity]` (sw, langsw,
drivers / no_drivers, sha256 of the extracted images). `device.py build` refuses to compose a ROM
when they differ, because every code patch address belongs to one exact build (romcompose also checks
each patch's original bytes).

