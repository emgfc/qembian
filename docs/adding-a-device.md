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
[Today: `N95_FW_DIR=… scripts/n95-inventory.sh`, hard-wired to RM-320 globs and `build/n95`.]
Outputs in `build/devices/<id>/`: `core-flat.img` (unpaged XIP core), `core.json`/`core-files.txt`
(directory), `rofs*.img`, `symindex.json`, `abi/` (EUSER SWI map, vs ours; kabi report), and a
**kernel-side driver list** (every LDD/PDD/extension in the firmware's ROM) which is the raw
material for `[[standins]]`. It writes `devices/<id>/firmware.sha256` and the `[firmware.rom]`
numbers (base, kernel data, paging, user window start) into the profile.

If the container is not FIASCO/BB5 the tool stops here: write a short research brief for the
container format (mock-level: partition table layout only) before going on.

## 3. Check the personality

```
tools/personality/check.py --device nokia-e72-rm530
```
[Today: read `build/n95/abi/swi-n95-vs-ours.txt` and run `tools/personality/kabi.py`.]
- Zero unknown SWIs and zero vtable deltas: keep `platform.personality` as set.
- A few: add them under `[personality.overrides]` in the profile (exec lines in `exec.map`
  syntax; class deltas in `kabi.toml` syntax) — the generator merges them.
- Many, or a different memory model: fork `personalities/<nearest>/` into a new one named after the
  OS version, note the derivation in its README (which EUSER, which cross-check), and set
  `platform.personality` to it. This is a kernel rebuild (`scripts/build-kernel.sh`).

## 4. Describe the hardware from the firmware

Where to look, with the tools in `docs/tools.md` (`syb_sym.py` works on any device's symindex):
- **Display**: `Z:\system\data\wsini.ini` in the ROFS (`tools/rofs.py`) gives the UI size and
  rotation; the display driver's caps (decompile the firmware's `d_display`-like LDD once; the
  N95 analysis is `docs/nokia-d_display.md`) give native orientation, pitch, modes. Fill
  `[board.display]` and `[kernel.lcd]`.
- **Keys**: the firmware's keypad driver matrix table (N95: `keypad.dll` at 0x8014f6d4) and
  `EKData.dll` list the EStdKey codes; the spec sheet names the keys. Write `keymap.toml`
  (`tools/device.py check` verifies the EStdKey names). Slides and the camera half-press are
  just more codes at this stage.
- **Timing**: the firmware's `hal.dll` reports `EFastCounterFrequency` and `ENanoTickPeriod`;
  the board must match (`[board.timing]`). The N95's are 32768 Hz / 1000 µs.
- **Drives**: the firmware's `Z:\sys\data\ESTARTCOMP.TXT` (or `ESTART.TXT`) maps letters to local
  drives; write `[[board.drives]]` and the device's `ESTARTCOMP.TXT` with our local drive
  numbers (0 IRAM, 1 NVMemory, 2 second NVMemory, 8 composite ROFS). CR LF line endings.
- **Stand-ins**: for each driver in the inventory's kernel-side list, look for an existing
  stand-in of the same name in `src/standins/` (families: `common`, `nokia-bb5` for BB5-era
  modem/security/light drivers, `nokia-omap2` for display/DSP/DVFS). Same name → reuse (add it to
  `[[standins]]`); unknown → leave it out and let the boot loop tell you whether anything
  misses it (`[standins.missing]`).
- **Modem**: `[modem]` starts as a copy of the N95's; `isi-replies.txt` starts empty but for the
  boot-critical replies the N95 needed (MTC, SIM status, security, self-test, RTC, light, energy
  management: copy them, they are per ISI resource, not per phone).
- **GL**: if the phone has a PowerVR/SGX GPU and a `libgles_cm.dll`, set `[gl]`: ordinals from the
  matching SDK `.dso` (names only), `imports` = the N95 list to start with (the tool resolves the
  addresses from this device's symindex and fails on missing exports), identity strings from
  the firmware's driver DLLs (`syb_sym.py str`), `blob_addr` = a free gap below the user window
  (the composer refuses overlaps). Otherwise `gl.impl = "none"`.

## 5. Build and boot loop

```
tools/device.py build nokia-e72-rm530            # kernel side, stand-ins, compose, DTB (today scripts/hybrid-boot.sh --compose-only)
sydb start --device-id nokia-e72-rm530 --fresh   # headless; waits for milestones.idle or times out
```
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
