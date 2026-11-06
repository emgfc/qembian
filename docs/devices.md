# Devices: the declarative device model

How syborg-pg grows from one hand-built virtual N95 8GB into a platform where any S60 phone is
added by writing a profile directory, without a QEMU fork or rebuild per phone. Written 2026-11-06
from the state of the repository (commit 44c48d2). [V] = checked in the code or docs named,
[I] = inferred or proposed. The ordered refactor is in [refactor-plan.md](refactor-plan.md); the
flow for a new phone is in [adding-a-device.md](adding-a-device.md).

## 1. What exists today, and where the N95 is hard-wired

| Layer | Today | Device-specific bits buried in it |
|---|---|---|
| QEMU board `src/qemu-syborg/hw/arm/` (one binary, `scripts/build-qemu-modern.sh`) | all devices always present; options via `-global` (fb width/height/refresh-rate/blank-dim, gl backend, rtc offset-file, pointer input), `-cpu`, `-m`, `-drive if=mtd`, `-device loader` for ROFS [V] | machine class defaults (`arm1136-r2`, 168 MB) and the description string say "N95" [V `syborg.c`]; nothing else |
| Run scripts `scripts/run-modern.sh`, `run-gui.sh`, `tools/sydb/daemon.py` | compose the QEMU command line | `build/n95/*` paths, `syborg-n95.dtb`, 168M memory backend, drive sizes 64M/1G, audio/gl defaults [V] |
| Guest baseport `patches/qemu-baseport/` (16 patches on `external/oss.FCL.sf.adapt.Qemu`) | built by `scripts/build-syborg.sh` | keyboard table (0005/0012/0014/0015/0016), LCD modes and pitch (0001/0003/0013), RAM size in the bootstrap (0010), CPU (0011), tick/fast counter (0008/0009), RTC hook (0007), nvmem instance (0006) [V] |
| Kernel `patches/kernelhwsrv/` (9 patches) | `scripts/build-kernel.sh` | 9.2 exec personality (0001/0005, tables from `tools/exec92/exec92.map`), 9.2 driver ABI (0008/0009), N95 critical-thread policy (0003/0006), debug traces (0002/0004/0007) [V] |
| Stand-ins `src/n95compat/` | `scripts/build-n95compat.sh` copies the tree into the baseport and writes an oby fragment | 15 drivers listed in `bld.inf`; `isi/isi-db.txt` (names + canned replies) → generated `isireplies.h`; stateful modem in `stubs/iscpeer.inl`; `lcd/lcd_n95.mmp` macros [V] |
| ROM composition `tools/romcompose.py`, `rom/hybrid/` | `scripts/hybrid-boot.sh` / `compose-exp.sh` | `code-patches.txt` (addresses in the RM-320 v35 ROM), `ESTARTCOMP.TXT`, blob address `0x80320000`, `--exec92`, trace mask [V] |
| GL `src/n95gl/` | `build.sh` → `sygl.bin` + `rom/hybrid/gles-patches.txt` via `tools/gen-gles-glue.py` | ordinals of this ROM's `libgles_cm.dll`, `n95imports.txt`, identity strings in `gl.c`/`egl.c`, `SYGL_BASE` [V] |
| Firmware pipeline `scripts/n95-inventory.sh`, `tools/fiasco.py`, `rom_unpage.py`, `romimg.py`, `swi_map.py`, `xip_imports.py` | outputs to `build/n95/` | `N95_FW_DIR`, `RM-320_*.C0R` globs, SDK paths [V] |
| sydb `tools/sydb/` | daemon, CLI, MCP, phone window | `IDLE_RE`, key aliases, `HOST_KEYS`, phone-window buttons, ROM defaults (`hybrid.img`, `hybrid-exp.img`), `build/n95` drives [V] |
| Docs | `docs/n95-*.md`, `hacks.md` | all N95 |

Two facts shape the design:
- **The guest already learns the display from device registers, not from the DTB.** The fb driver
  reads `FB_WIDTH`/`FB_HEIGHT` from the QEMU device [V `baseport/syborg/fb/syborg_fb.cpp:220`];
  `tools/dtb_set.py` only mirrors the size into the DTB the platform device serves. So "runtime
  config through QEMU properties, read by the guest from registers" is the existing pattern.
- **The user side is byte-identical firmware.** Everything that carries an address (byte patches,
  GL entry patches, import tables, symbol index) is per firmware version, not per model.

## 2. Profile format: TOML, one directory per device

`devices/<id>/device.toml` plus small data files next to it. Format choice: **TOML**.
- Python 3.11 reads it from the standard library (`tomllib`), so the `python3 -I` tools gain no
  dependency; every consumer (bash through `tools/device.py`, Python, QEMU via a generated file) is
  covered.
- Comments are needed: every value cites the file it came from and its evidence tag. JSON has none.
- Hex literals (`0x80320000`) and no implicit typing (YAML's `no` → false, `0x` strings, indentation
  errors) matter for a file LLM agents will edit.
- Arrays of tables (`[[standins]]`, `[[board.drives]]`, `[[keys]]`) map naturally to "one record
  per driver/drive/key", which is most of a profile.

Directory layout of a device:

```
devices/nokia-n95-8gb-rm320/
  device.toml          the profile (schema below)
  keymap.toml          physical keys -> EStdKey -> host keys -> skin placement
  skin.toml            optional phone-window geometry (falls back to a generic S60 layout)
  isi-replies.txt      canned fake-modem replies for this firmware (names stay platform-wide)
  code-patches.txt     byte patches on this ROM (romcompose --patches format, originals verified)
  ESTARTCOMP.TXT       drive map (CR LF)
  firmware.sha256      checksums of the input files (not the bytes)
  gl/                  libgles_cm.ordinals, n95imports.txt, generated gles-patches.txt
  README.md            status, levels reached, open items, device-specific hacks
```

Inheritance: `extends = "<id>"`. Tables deep-merge, scalars and plain arrays in the child replace
the parent's, arrays of tables merge by `name` (child entries with the same `name` replace; a
`remove = ["name", ...]` list under the table drops parent entries). The RM-321 example in
`devices/nokia-n95-8gb-rm321/` sets `hardware.wlan = false`, its own firmware globs and its own
address-bearing files, and inherits the rest. Variants of a *ROM* for the same device (today the
audio experiment ROM) are `[rom.variants.<name>]`, not separate devices.

### 2.1 Schema, field by field

`schema = 1` at the top; the loader refuses unknown schema numbers and unknown top-level tables.

**`[device]`** identity. `id` (= directory name), `vendor`, `model`, `type_code` (Nokia RM-xxx),
`aliases` (what `sydb --device-id` also accepts), `market`, `notes`, `extends`.

**`[platform]`** `os` (`symbian`), `os_version` (`9.1`…`9.4`, `3`, `belle`), `ui` (`s60-3.0`,
`s60-3.1`, `s60-3.2`, `s60-5.0`, `s60-5.2`…), `personality` (directory under `personalities/`, §4),
`soc`, `cellular` (`bb5`, `rapido`…: which fake-modem family).

**`[firmware]`** `version`, `container` (`fiasco-bb5`, later others), `checksums`, `obtain` (free
text: where a user gets their own copy; never a download link to proprietary bytes), `[[files]]`
with `role` (`core`, `rofx`, `uda`…), `glob`, `partitions` (container partition names → roles),
`optional`. `[firmware.rom]`: `base`, `kernel_data`, `memory_model`, `paged`,
`user_window_start` (lowest address the user side occupies after our kernel side; the composer
checks the overlap, the GL blob goes below it).

**`[board]`** everything turned into QEMU arguments at run time. `machine` (always `syborg`),
`cpu` (QEMU model), `ram_mb`, `ram_mb_real`. Sub-tables:
- `display`: `width`, `height` (UI size), `native_orientation`, `line_pitch`, `bpp`, `hal_modes`,
  `hal_mode`, `refresh_hz`, `blank_dim_percent`, `partial_mode`, `touch`, `rotation_caps`.
- `timing`: `fast_counter_hz`, `nanokernel_tick_us`, `rtc`.
- `audio`: `device`, `host_default`, `dsp` (which stand-in feeds the device).
- `gl`: `device`, `backend` default.
- `flash`: `base`, `size_mb`, `content` (what the loader puts there).
- `[[drives]]`: `letter`, `local_drive`, `kind` (`iram`, `nvmem`, `nvmem2`, `composite`, later
  `mmc`), `image`, `size`, `removable`, `flags` (ESTART flags; the tool can generate ESTARTCOMP
  from these later).
- `input`: `keymap` file, `translation` (`guest` today, `qemu` after refactor step 7).

**`[hardware]`** presence flags: `wlan`, `bluetooth`, `camera`, `fm_radio`, `gps`,
`accelerometer`, `slides`, `leds`, `memory_card`, `mass_memory_gb`, `tv_out`, `dsp`, `gpu`. Read by
the skin (which buttons/toggles to show), by `adding-a-device` checklists (which levels apply), and
by stand-in selection hints. They do not change the emulation by themselves.

**`[kernel]`** guest build: `baseport`, `variant`, `trace_mask`, `secondary` (file server the ROM
boots), `critical_thread_policy` (`spare` = H-critical-exit, `strict`), `user_memory_guards`,
`patchsets` (named directories under `patches/kernelhwsrv/`, §4.2), `[kernel.lcd]` (`target`,
`macros` for the LCD HAL extension until §3.3 moves them to registers).

**`[[standins]]`** kernel-side stand-ins composed into the ROM, in obey order: `name` (directory
under `src/standins/`), `kind` (`ldd`, `pdd`, `extension`, `lcd`), `family` (`common`,
`nokia-bb5`, `nokia-omap2`, …), `role` (one line), `options` (MACRO= pairs for the mmp, e.g. ISC
channel base). `[standins.missing].drivers` lists the firmware's kernel-side files that have no
stand-in (documented gaps, read by the status report).

**`[modem]`** `kind` (`isi-fake-peer`), `names` (platform-wide resource/message names),
`replies` (device table), `stateful` (which stand-in holds the code), plus the state knobs the
stateful code reads at build time (later at run time through the platform device): `sim`,
`mtc_state`, `imei`, `battery_bars`, `charger`, `rtc_valid`, `phone_lock`, `power_off_delay_s`.

**`[rom]`** `code_patches` (files), `files` (rom path ← local), `manifest`, `[rom.variants.<n>]`
with `omit_patches` (hack ids from the patch file comments) / `add_patches`.

**`[gl]`** `impl` (`sygl` or `none`), `target_dll`, `ordinals`, `imports`, `blob_addr`,
`entry_patches`, `surface_model`, `present`, `vsync_request`, `cmd_controls`, `[gl.identity]`
strings read from the device's own driver DLLs.

**`[milestones]`** serial-log patterns the tools watch (`idle` is what sydb waits for;
`expected_idle_s` sets the timeout and the smoke test's budget). **`[props]`** P&S keys tools read.
**`[skin]`** `layout`, `orientation_key`. **`[status]`** `level`, `updated`, `open`.

### 2.2 `keymap.toml`

`[[keys]]`: `key` (our name, used by `sydb key`), `label`, `scancode` (EStdKey name from
`platform/symbian/stdkeys.toml`, the e32keys.h numbering), `host` (QEMU qcodes), `set1` (PC set-1
code the guest table is indexed by while translation is in the guest), `group`/`row`/`col` (skin),
`long_press`, `evidence`. `[[switches]]`: slides and the like, each position with its own scan
code (or, later, a stand-in event). One file feeds five consumers (§3.4).

## 3. Layering: one QEMU binary, per-device guest builds

```
                 runtime (profile -> arguments, no rebuild)          build-time
host   QEMU 11.1.3 + syborg board (all devices, platform-wide)  ....  once per repo
       -cpu, -m, -global syborg-*.*, -drive, -device loader,
       keymap file, DTB, audio/gl backend, drives, RTC file
guest  composed ROM = kernel side (ours) + user side (firmware)  ....  per device:
         kernel: one build per personality/memory model (§4)            kernel-side ROM (oby from profile)
         baseport: generic + LCD/keyboard tables from the profile        composed ROM, GL blob, DTB,
         stand-ins: the profile's [[standins]] list                      byte patches, symbol index
tools  sydb / syb_run / isi.py / syb_sym read the profile          ....  nothing
```

### 3.1 Runtime (host, from the profile)

`tools/device.py qemu-args <id> [--session DIR] [--gl soft] [--audio X]` prints the QEMU argument
list; `run-modern.sh --device <id>` and the sydb daemon use it. Covered today by existing
properties [V]: `-cpu board.cpu`, `-m board.ram_mb` (+ the memory backend size the daemon adds),
`syborg-fb.width/height/refresh-rate/blank-dim`, `syborg-gl.backend`, `syborg-rtc.offset-file`,
`syborg-pointer.input`, `-drive if=mtd` per `nvmem` drive, `-device loader` for `board.flash`,
`-audiodev`. Added by the refactor [I]: `syborg-keyboard.keymap=FILE` (§3.4),
`syborg-platform.device-id=STRING` and `syborg-platform.ram-mb` (§3.3), `-dtb` generated from the
profile instead of patched from the baseport's DTS.

Which board devices exist is also runtime: all of them are instantiated (unmapped ones read as
zero, as on 0.9.1 [V `syborg.c`]), and a device the profile does not use is simply left
unconfigured (`syborg-gl.backend=off`, no second `-drive`). No machine variants are needed; a phone
with a touch screen sets `syborg-pointer.input=on`.

### 3.2 Build-time, platform-wide

QEMU with the board (`scripts/build-qemu-modern.sh`, `patches/qemu-modern/` are CPU-model fixes,
not device-specific [V]), the host tools, the kernel per personality (§4), the generic baseport.

### 3.3 Build-time, per device (guest only)

`tools/device.py build <id>` = today's `hybrid-boot.sh --compose-only` generalised:
1. Baseport build with the profile's LCD macros and keyboard table (until §3.4/§3.3 make them
   runtime) → `build/devices/<id>/kernel-side.img` (the oby fragment generated from `[[standins]]`
   replaces the `grep bld.inf` loop in `build-n95compat.sh`).
2. Stand-ins: `src/standins/<name>` for each entry, with `options` as MACROs; `isireplies.h`
   generated from `platform/isi/names.txt` + `devices/<id>/isi-replies.txt`.
3. GL blob and entry patches if `gl.impl = "sygl"` (addresses from this device's symbol index).
4. Compose: `romcompose.py --ours … --n95 core-flat.img --secondary … --exec-personality sym92
   --trace … --file … --rofs … --patches … --blob …`, all from the profile.
5. DTB from the profile.

**How the guest learns the hardware.** Three channels, in order of preference:
1. **Device registers** (already the pattern): fb size → width/height; keyboard mode/keymap
   presence (§3.4); platform device `DEVICE_ID`, `RAM_MB` [I] so the bootstrap's `KHwRamSizeMb`
   (patch 0010) can become a register read and one bootstrap serves every RAM size.
2. **The DTB served by the platform device** (+0x1000 window [V `syborg.c`]) for structured data:
   display HAL modes/bpp/pitch/native orientation (replacing the `lcd_n95` macros, so one `lcd.dll`
   serves all), drive layout, the device id string. The baseport ships libfdt
   (`svpplatform/libfdt`) [V], so reading it is cheap.
3. **HAL**: user-side HAL values the firmware's own `hal.dll` reports (fast counter, tick) are
   fixed by the firmware; the board must match them (`board.timing`), which is why they are
   compiled into the baseport today and should stay declared in the profile as a *check*, not a
   knob.

What stays a per-device guest rebuild even after the refactor: the stand-in set, the composed ROM,
the GL blob, byte patches. What never needs a rebuild: QEMU, host tools, the kernel (once
personalities are runtime-selected, §4.3).

### 3.4 Keyboard: where translation should live

Today: host key → QEMU qcode → `qemu_input_map_linux_to_atset1` set-1 code → FIFO [V
`syborg_input.c:66`] → guest `EKEYB` compiled table (`syborg_keyboard.h`, patched five times) →
EStdKey → the firmware's own `EKData.dll` → WSERV. The table is the only device-specific thing in
the chain, and it lives in the one place that needs a guest rebuild.

Proposal [I]: translate in QEMU, keep the guest driver a passthrough.
- `syborg-keyboard` gains `keymap=FILE` (lines `qcode  estdkey-number`, generated by
  `tools/device.py gen keymap <id>`) and a `MODE` register bit "Symbian scan codes". With a keymap
  loaded the FIFO carries `EStdKey | 0x80 on release` (the N95 keypad driver's own model: one
  down/one up, no autorepeat, already enforced in `syborg_input.c` [V]). Keys with no mapping are
  dropped and logged (`LOG_UNIMP`).
- Guest `EKEYB`: if `MODE` says Symbian codes, pass the code through; else the old set-1 table (so
  old ROMs and the PDK text-shell fixture keep working). One generic keyboard driver for all phones.
- Slides, camera half-press and other switches are just more EStdKey codes in the same map until a
  device needs a real slide stand-in (then the map entry names the stand-in event instead).
- The same `keymap.toml` generates sydb's key aliases, the phone window's buttons and host keys,
  the controls doc table, and (while translation is still in the guest) the EKEYB table header.
  Step 5 of the plan does the generation; step 7 moves translation.

## 4. Kernel ABI strategy: personalities

A **personality** is everything our newer EPL kernel must present to an older user side and to
drivers built for the older kernel. Today's `sym92` personality is spread over `tools/exec92/`
(exec map, generator), kernel patches 0001/0005 (tables + shims), 0008/0009 (driver ABI, memory
guards), `tools/kabi92.py` (vtable check) and `scodeseg.cpp` loader-struct sizes [V hacks.md].

```
personalities/sym92/
  personality.toml     os_version, memory model, which shims, loader struct deltas, guard policy
  exec.map             9.2 exec numbering -> our handlers (today tools/exec92/exec92.map)
  kabi.toml            driver-facing classes to compare + known slot deltas (today in kabi92.py)
  patches/             kernel patches that implement it (today 0001, 0005, 0008, 0009)
  README.md            evidence: how the map was derived (swi_map + EKA2L1 cross-check), status
```

Generated per device, never hand-written: `build/devices/<id>/abi/{euser-swi.json,
swi-vs-ours.txt, kabi-report.txt}` from the device's own `EUser.dll` and `ekern.exe`
(`scripts/n95-inventory.sh` does the first two today [V]; `kabi92.py` the third). A device declares
`platform.personality = "sym92"` and the tools **verify** its firmware against that personality:
unknown SWI numbers and vtable deltas are listed; if they are non-empty the device either adds a
`[personality.overrides]` table (extra map lines, extra class deltas) or a new personality is forked
from the nearest one.

### 4.1 Versions, what is known [I unless marked]

| User side | Personality | Expected work |
|---|---|---|
| Symbian 9.1 (S60 3rd: N73, N80, N93, E60/E61) | `sym91` | same mechanism as 9.2; exec numbering differs by a handful of entries (9.1→9.2 added calls at the end and a few in the middle: derive with `swi_map.py`); loader structures older (`TProcessCreateInfo` smaller again); no `RTimer::AtUTC`. Driver ABI: DObject/DLogicalChannel layouts as 9.2 [I]. |
| Symbian 9.2 (S60 3rd FP1: N95, N95 8GB, N82, N81, E90, 6120) | `sym92` | done [V]; six unmapped calls remain logging stubs (hacks.md) |
| Symbian 9.3 (S60 3rd FP2: N96, N85, N86, N79, E52, E55, E66, E71, E72, E75, 5320, 6220) | `sym93` | exec table grew (new calls for `RProperty`/`RMessage2` variants, the 9.3 `User::FastCounter` family unchanged); demand paging in the firmware ROM is the norm (rom_unpage handles it); kernel ABI: Symbian 9.3 added `DLogicalChannel::SendMsg` and `DObject::RequestUserHandle` variants later than 9.2, so `kabi` deltas likely shrink. Many of these phones have a **touch-less** but **OMAP2430/RAPIDO** platform: new stand-in families (`nokia-rapido`), different DSP bridge, same BB5 ISI modem. |
| Symbian 9.4 (S60 5th: N97, 5800, 5530, X6) | `sym94` | closest to the EPL kernel's native table (EPL is the Symbian^3/^4 line); touch (`syborg-pointer.input=on`, pointer stand-in may need pressure/multi-touch flags); the N97 has a slide + QWERTY keypad (keymap only), accelerometer and a RAPIDO/OMAP2430 platform like 9.3 devices. ROM is paged with the flexible memory model on some S^3 builds, but 9.4 N97 still uses the multiple model [I]. |
| Symbian^3 / Anna (N8, C7, E7, X7) | `sym3` | user side matches the EPL kernel generation: the native exec table may work with no map (verify with `swi_map`). Nokia's kernels use the **flexible memory model** and data paging [I]; our build is the multiple model, so the personality would need an FMM kernel build (the EPL source has it) — one more kernel build, not a new mechanism. Drivers: kabi against the N8's ekern. Platform: OMAP3630 (ARMv7, Cortex-A8: `-cpu cortex-a8`, our A8 build path still exists [V]), SGX GPU (sygl again, strings differ), ISI modem on a newer BB5 generation. |
| Belle, Belle FP1/FP2/Refresh (2011–2012, 808 PureView) | `belle` | newer than any EPL drop: Nokia closed the source in 2011 and shipped kernels after that. Expect a few exec calls beyond our table (handled like 9.2's unmapped ones: log and `KErrNotSupported`), and kabi deltas. [I; nothing measured] |

Devices named by the owner: N95 8GB (9.2, done), original N95 RM-159 (9.2, same family: mostly
a new firmware inventory, 160 MB ROM window differs, no mass memory), N86 (9.3), N97 (9.4, touch),
E72 and E52 (9.3). "N83" is not a Nokia model I know; if N82 or N93 is meant, those are 9.2 and 9.1.

### 4.2 Kernel patch sets

Split `patches/kernelhwsrv/` by what the patch is for, and let the profile name them:
- `platform/`: debug and tooling (0002 traces, 0004 startup props, 0007 N95CODE code-segment
  trace) — always applied.
- `personalities/sym92/patches/`: 0001 exec92, 0005 TimerAt92, 0008 kabi92, 0009 no user memory
  guards.
- `policy/`: 0003/0006 spare critical threads — a kernel config flag in the ROM header (like the
  personality bit) selected by `kernel.critical_thread_policy`, so one kernel serves strict and
  lenient devices.

### 4.3 Runtime selection

Today the ROM header's kernel config bit 29 turns the 9.2 tables on [V `sexec92.cpp`]. Proposal:
compile every personality's tables into the kernel (they are small: 265 entries each) and put a
personality *number* in the ROM header config word (bits 29–31, 0 = native) set by `romcompose
--exec-personality NAME`. Then one kernel build per memory model covers all devices; per-process
selection (hacks.md "personality is global") is a later refinement if our own user binaries are
mixed in.

## 5. Repository layout after the refactor

```
devices/<id>/                 profiles and their data (§2)
devices/default -> nokia-n95-8gb-rm320
personalities/<name>/         exec map, kabi spec, patches, README (§4)
platform/                     shared data: symbian/stdkeys.toml (EStdKey numbers), isi/names.txt,
                              families/<family>.md (what a stand-in family covers)
patches/qemu-modern/          CPU-model fixes (platform-wide)
patches/qemu-baseport/        generic baseport changes; device values come from the profile
patches/kernelhwsrv/platform/ , policy/  (personality patches live in personalities/)
src/qemu-syborg/              the board (one binary)
src/standins/<name>/          kernel-side stand-ins, one per driver; README with family + evidence
                              (today src/n95compat/stubs/*, medrofs, mednvmem2, lcd)
src/sygl/                     GLES/EGL implementation (today src/n95gl); device inputs in devices/<id>/gl
src/apps/                     guest test apps and the sydb agent (today src/n95apps)
tools/device.py               profile loader/validator; subcommands: check, show, qemu-args, build, gen, new
tools/fw/                     firmware pipeline: fiasco.py, rom_unpage.py, romimg.py, inventory.py (per device)
tools/personality/            swi_map.py, exec align/gen, kabi.py (generalised kabi92)
tools/sydb/, syb_*.py, isi.py, syb_sym.py   take --device-id (default devices/default)
build/devices/<id>/           every generated artifact of a device (today build/n95): core-flat.img,
                              core.json, rofs*.img, symindex.json, abi/, kernel-side.img, rom.img,
                              rom.rofs, syborg.dtb, sygl.bin, sessions' drives
firmware/<id>/                the user's own firmware files (git-ignored)
docs/devices/<id>/            long-form device notes (today docs/n95-*.md), hacks per device
docs/hacks.md                 platform-wide hacks only
```

Naming: device ids `<vendor>-<model>-<typecode>`; stand-in directories keep the driver's ROM
name (`d_display`, `nokiaiscdriver`) because that is what the firmware loads by name; families
`common`, `nokia-bb5`, `nokia-omap2`, `nokia-rapido`, `nokia-omap3`; personalities `sym<version>`;
hack ids `H-<area>-<name>` (unchanged) with the device id as a prefix when device-specific
(`n95-H-splash-behind`).

## 6. How tools consume a profile

| Tool | Reads | Produces / does |
|---|---|---|
| `tools/device.py check ID` | profile + inheritance | schema validation, file existence, EStdKey names, address ranges inside the ROM, evidence-tag census |
| `device.py show ID [--json]` | resolved profile | the merged profile (what every other tool sees) |
| `device.py qemu-args ID` | `[board]`, `[gl]`, drives | QEMU command line fragment (`run-modern.sh --device`, sydb daemon) |
| `device.py build ID [--variant audio-exp]` | `[kernel]`, `[[standins]]`, `[modem]`, `[rom]`, `[gl]` | oby fragment, isireplies.h, LCD/keyboard headers, sygl, composed ROM, DTB into `build/devices/ID/` |
| `device.py gen keymap|controls-doc|skin ID` | `keymap.toml` | QEMU keymap file, EKEYB header, sydb aliases, docs table |
| `device.py new ID --from BASE` | a base profile | scaffold with `extends`, empty status, TODO markers |
| `tools/fw/inventory.py --device ID` | `[firmware]`, `firmware/ID/` | `build/devices/ID/{core-flat.img, core.json, rofs*.img, symindex.json, abi/}`, `firmware.sha256`, kernel-side driver list (input for `[[standins]]`) |
| `tools/personality/check.py --device ID` | `[platform].personality`, `abi/` | unknown SWIs, vtable deltas, suggested overrides |
| `sydb start --device-id ID` | `[board]`, `[milestones]`, `[props]`, `keymap.toml` | session with the right ROM/drives/args; waits for `milestones.idle`; key aliases |
| `sydb phone` | `keymap.toml`, `skin.toml`, `[hardware]` | buttons, slides, toggles shown only for hardware the phone has |
| `syb_run.py`, `syb_gdb.py`, `syb_report.py` | milestones, stand-in log prefixes, ISI names | reports per device |
| `isi.py` | `platform/isi/names.txt` + device replies | decoding and the generated header |
| `syb_sym.py` | `build/devices/ID/symindex.json` | symbols of this device's ROM |

The profile is the only input a new contributor's agent edits; tools fail loudly (`device.py
check`) rather than booting a half-configured phone.
