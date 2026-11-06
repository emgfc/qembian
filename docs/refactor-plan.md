# Refactor plan: from the N95 hack pile to the device model

Ordered steps for the main agent. Each step names the files, what moves where, how the N95 keeps
booting, and a test. **[M]** = must-have before a second device can be started, **[S]** = should
follow soon (removes a guest rebuild or a hand edit per device), **[L]** = later. Design:
[devices.md](devices.md); flow for new phones: [adding-a-device.md](adding-a-device.md).

## Status (2026-11-06)

Done: steps 0-5 (commits 8ec4d46..694fc05) and firmware identity (4497c5a). Deviations from the
plan below, on purpose:
- Step 1: the ISI database moved whole to `devices/<id>/isi-db.txt` (`[modem].db`); the split into
  platform names + device replies is still to do. `platform/symbian/syborg-pc-keys.toml` is the base
  key layer (shipped Syborg table + patch 0005's extended navigation).
- Step 2: `build/n95` is a symlink to `build/devices/nokia-n95-8gb-rm320`; mcp.py, boot milestones and
  measure-boot.sh still read the old defaults. run-modern.sh also refuses a drive image another QEMU
  has open (after an incident on the owner's drives).
- Step 3: stand-in options (`[[standins]].options`) are not generated yet (the extension LDD keeps its
  #defines); variants omit patches by `[H-...]` tag in the patch line.
- Step 4: no SWI-table comparison in `check.py` yet (driver ABI only).
- Step 5: the phone window and docs/n95-controls.md are not generated from keymap.toml yet.
Owner's list additions: the phone window mirrors the host keyboard on its on-screen keys (pressed
while held) - done 2026-11-06; it must keep doing so when step 5's skin generation builds the
buttons from keymap.toml (one button per key, found by its host qcodes).
- Step 6 (done): tools/fw/inventory.py --device ID (profile [[firmware.files]], checksums, partitions,
  unpaging, core.json, kernel-drivers.txt, ABI reports, identify); re-extraction of the RM-320 set is
  byte-identical (core-flat, core.json, rofs1/2). The helper tools stay in tools/ for now (imported
  by others); the firmware-inventory doc did not move.
- Step 7 (done): syborg-keyboard.keymap=FILE (device.py gen qemu-keymap, passed by qemu-args when
  board.input.translation = "qemu"): mapped keys arrive as Symbian scan codes (bit 30), the guest
  EKEYB passes them through (patch qemu-baseport 0014, which also bounds-checks the set-1 table);
  unmapped keys keep the set-1 table. Verified: a host key mapped only in keymap.toml (F22, whose
  set-1 code is past the guest table) opened Carousel without a guest rebuild.
Next: step 8 (board tells the guest RAM/display/device id), then the rest.

## Proposed section for CLAUDE.md (paste as is)

```
## Devices (declarative device model)

One QEMU binary and one kernel per personality serve every phone; everything phone-specific is
declared in `devices/<id>/device.toml` (+ keymap.toml, isi-replies.txt, code-patches.txt,
ESTARTCOMP.TXT, gl/). Read `docs/devices.md` (schema, layering) before touching anything under
`devices/`, `src/standins/`, `personalities/` or the run scripts; `docs/adding-a-device.md` is the
flow for a new phone. Rules:
- No new N95 literal in a script or tool: it goes into the profile and is read through
  `tools/device.py` (`show`, `qemu-args`, `build`, `gen`). The default device is `devices/default`
  (`SYBORG_DEVICE=<id>` or `--device-id` override).
- Stand-ins live in `src/standins/<driver>/` with a family tag; the profile lists which ones a
  device composes. Device-specific hacks go in `devices/<id>/README.md`, platform-wide ones in
  `docs/hacks.md`.
- Personalities (`personalities/sym92/…`) are generated from the device's own EUSER/ekern and
  verified by `tools/personality/check.py`; never hand-edit a generated map.
- Smoke test after every change to the platform: `tests/device-smoke.sh nokia-n95-8gb-rm320`
  (boots to idle, Carousel renders, Switch off ends the session).
- Firmware stays in `firmware/<id>/` and `build/devices/<id>/` (git-ignored); checksums only.
```

## The one test used by every step

`tests/device-smoke.sh <id>` (step 0 creates it), on top of sydb:
1. `sydb start --device-id <id> --seed build/runs/mtc-drives --fresh` (headless); fail if
   `milestones.idle` is not reached within `expected_idle_s × 1.5` (today ~26 s → 40 s budget; the
   seeded phone skips the first-boot queries and has reached idle in about 15 s in recent runs —
   take the measured number from the first green run as the budget).
2. `sydb screenshot` → not uniformly black.
3. `sydb key multimedia`, wait 4 s, `sydb wg` shows Carousel focused, screenshot changed, no
   `thread.panic` event for Carousel (GL path, host backend). Repeat once with `--gl soft`
   (optional flag `--soft`).
4. `sydb key menu`, `sydb key back`, `sydb unlock` (keyguard round trip), `sydb prop get 101f8766 41`
   = 203.
5. `sydb key power:2.5` then `sydb key down ok` on the power menu (Switch off); the session must end
   by itself within 20 s (H-modem-power), and `sydb status` reports no QEMU.
Each step below says "smoke" for this; add the step's own check next to it.

## Step 0 [M] — baseline and the smoke test

Files: new `tests/device-smoke.sh`; `docs/tools.md` one paragraph.
Change nothing else. Run it three times on the current tree and record the idle time and the
pass/fail of the Carousel and switch-off checks in `devices/nokia-n95-8gb-rm320/README.md`
(create it: status, level, open items from `docs/roadmap.md`).
Test: smoke green three times in a row. If the Carousel boot-instance race (roadmap) makes step 3
flaky, the test uses `sydb launch 0x10282bf6` after killing the boot instance, and the flake is noted.

## Step 1 [M] — the profile and its loader, no consumers yet

Files: `devices/nokia-n95-8gb-rm320/device.toml`, `keymap.toml` (written), `README.md`;
`devices/README.md` (written); `devices/default` symlink; new `tools/device.py` (stdlib only:
`tomllib`, `argparse`); new `platform/symbian/stdkeys.toml` (EStdKey name → number from e32keys.h,
the SDK header is a naming reference, generate the table with a one-off script and commit the TOML).
Move (git mv, leave a one-line pointer file or a symlink for one release): `rom/hybrid/code-patches.txt`
→ `devices/…/code-patches.txt`, `rom/hybrid/ESTARTCOMP.TXT` → `devices/…/ESTARTCOMP.TXT`,
`rom/hybrid/gles-patches.txt` → `devices/…/gl/gles-patches.txt`, `docs/evidence/n95/firmware.sha256`
→ `devices/…/firmware.sha256`. Split `src/n95compat/isi/isi-db.txt`: `res`/`msg` lines →
`platform/isi/names.txt`, `reply` lines → `devices/…/isi-replies.txt` (`tools/isi.py` reads both;
`gen-header` output unchanged).
`device.py` subcommands now: `check` (schema, inheritance, EStdKey names, files exist, every
code-patch address inside `firmware.rom.base..+size`, every `set1` unique), `show [--json]`,
`qemu-args` (prints what `run-modern.sh`/daemon pass today, byte for byte, for the N95), `gen
keymap-header` (emits the `syborg_keyboard.h` table lines; **diff it against the patched header**,
must be identical, not yet consumed).
Keep the N95 booting: scripts still read the old paths through the pointer files/symlinks.
Test: `tools/device.py check nokia-n95-8gb-rm320` and `… nokia-n95-8gb-rm321` pass; `device.py
qemu-args` output equals the daemon's argument list (a unit test in `tests/device_py_test.py`,
plain `unittest`); keymap header diff empty; smoke.

## Step 2 [M] — scripts and sydb consume the profile

Files: `scripts/run-modern.sh` (new `--device ID`: everything after `-M syborg` from `device.py
qemu-args`; the explicit flags stay as overrides), `scripts/run-gui.sh`, `tools/sydb/daemon.py`
(`Session` takes a device id; ROM default, drive sizes, DTB, memory backend size, gl/audio
defaults, `IDLE_RE` and key aliases from the profile), `tools/sydb/sydb.py` (`--device-id`, default
from `devices/default`/`SYBORG_DEVICE`; `--device` keeps its meaning "the owner's run-gui phone"),
`tools/sydb/mcp.py` (`session_start` gains `device_id`), `tools/syb_run.py`, `tools/syb_gdb.py`,
`scripts/measure-boot.sh` + `tools/boot_milestones.py` (milestones from the profile).
Rename `build/n95/` → `build/devices/nokia-n95-8gb-rm320/` with `build/n95` as a symlink (tools
resolve through `device.py paths ID`: `core_flat`, `core_json`, `symindex`, `rom`, `rofs`, `dtb`,
`drives_seed`). `tools/syb_sym.py`, `kabi92.py`, `gen-gles-glue.py`, `n95-inventory.sh` read their
inputs through it.
Test: `device.py qemu-args` is now the only place QEMU arguments are assembled (grep: no
`syborg-fb.width` outside `device.py` and the board source); `sydb start --device-id
nokia-n95-8gb-rm320` and plain `sydb start` boot; `run-gui.sh` boots; smoke; `syb_sym.py addr
803a86dd` still resolves.

## Step 3 [M] — stand-ins as a library, the ROM composed from the profile

Files: `src/n95compat/stubs/*` → `src/standins/<driver>/` (one directory per mmp; `iscpeer.inl`,
`nokiastub.h`, `isireplies.h`, `stubcert.h` go under `src/standins/_shared/` or next to the driver
that owns them; `medrofs`, `mednvmem2`, `lcd` likewise), each with a 5-line `README.md` (family,
role, evidence doc). `scripts/build-n95compat.sh` → `scripts/build-standins.sh ID`: `bld.inf`
and the oby fragment are **generated** from `[[standins]]` (order preserved; `options` → `MACRO`
lines in a generated `.mmp` wrapper or `-D` through the existing mmp `MACRO` with `#ifdef`
defaults, as `iscpeer.inl` already does); LCD macros from `[kernel.lcd]`.
`scripts/hybrid-boot.sh` → `tools/device.py build ID [--variant NAME] [--no-build]`, which runs:
baseport build (unchanged script), standins build, sygl build if `gl.impl = sygl`, `romcompose.py`
with every argument from the profile (`--exec92` becomes `--exec-personality sym92`, same bit for
now), DTB generation (`dtb_set.py` with `board.display`), manifest into `build/devices/ID/`.
`scripts/compose-exp.sh` → `device.py build ID --variant audio-exp` (omit by hack id: the patch
file lines carry `# H-…` tags; the tool filters by tag instead of by address). `hybrid-boot.sh`
and `compose-exp.sh` remain as two-line wrappers for one release.
Test: the composed ROM is **byte-identical** to the one `hybrid-boot.sh --compose-only` produced
before the step (keep a copy; `cmp`), for both the working and the audio-exp variant; `sydb strace`
unaffected; smoke.

## Step 4 [M] — personalities directory and verification tool

Files: `personalities/sym92/` → `personalities/sym92/` (`exec.map`, `align.py`, `map-draft.txt`,
README with the derivation), `gen_kernel_map.py` → `tools/personality/gen_exec_map.py` (input path
from the personality dir; `hybrid-boot.sh`'s first line moves into `device.py build`);
`tools/personality/kabi.py` → `tools/personality/kabi.py` with the class list and expected deltas in
`personalities/sym92/kabi.toml`, inputs from `device.py paths`; new `tools/personality/check.py
--device ID` (runs `swi_map.py --compare` + `kabi.py`, prints unknown SWIs / slot deltas, exit 1 if
any that the personality or `[personality.overrides]` does not list). `patches/kernelhwsrv/` →
`platform/` (0002, 0004, 0007), `policy/` (0003, 0006), and `personalities/sym92/patches/` (0001,
0005, 0008, 0009); `scripts/setup-epocroot.sh` applies `platform/*`, `policy/*` and the patches of
every personality present (they are independent today; one kernel build still carries only
`sym92`'s tables until step 9).
Test: kernel rebuild identical in behaviour (smoke); `check.py --device nokia-n95-8gb-rm320`
reports the known six unmapped 9.2 calls as "listed" and exits 0; `personalities/sym92/README.md`
explains how to derive a `sym93`.

## Step 5 [M] — keymap generation (translation still in the guest)

Files: `device.py gen keymap-header` output replaces the hand-edited table: a generated
`build/devices/ID/syborg_keyboard_map.h` included by `syborg_keyboard.h` through one small generic
baseport patch (`0005…0016` collapse into one patch that `#include`s the generated file and keeps
the extended-navigation entries as a `platform/symbian/pc-keys.toml` base layer). `device.py gen
sydb-aliases` → read by the daemon at start (no more `KEY_ALIASES` literal); `gen skin` → the
phone window builds its buttons from `keymap.toml` groups (`front`, `nav`, `keypad`, `side`,
`media`) with `skin.toml` only for geometry overrides; `gen controls-doc` → the table in
`docs/n95-controls.md` (now `docs/devices/nokia-n95-8gb-rm320/controls.md`).
Test: generated header identical to the pre-step header (step 1 diff, now enforced in CI);
`sydb key multimedia` opens Carousel, `landscape`/`portrait` switch the layout, phone window shows
the same buttons; smoke.

## Step 6 [S] — firmware pipeline per device

Files: `scripts/n95-inventory.sh` → `tools/fw/inventory.py --device ID` (globs, partitions, output
dir and SDK `.dso` path from the profile; `fiasco.py`, `rom_unpage.py`, `romimg.py`, `swi_map.py`,
`xip_imports.py` move to `tools/fw/`), writes `firmware.sha256`, `[firmware.rom]` facts (printed
for the human to paste, or `--write-profile`), and `build/devices/ID/kernel-drivers.txt` (the
firmware's LDD/PDD/extension list: input for `[[standins]]`). `docs/n95-firmware-inventory.md` →
`docs/devices/nokia-n95-8gb-rm320/firmware-inventory.md` (content unchanged, paths updated).
Test: re-running the inventory on the RM-320 set reproduces `core-flat.img`/`core.json` byte for
byte and the same `abi/` reports; `kernel-drivers.txt` lists the 15 drivers we stand in for plus
the `[standins.missing]` ones.

## Step 7 [S] — keyboard translation in QEMU

Files: `src/qemu-syborg/hw/arm/syborg_input.c` (`keymap` string property: file of `qcode-name
estdkey` lines; `MODE` register bit 0 = Symbian codes; with a keymap, push `code | 0x80` on
release; unmapped keys logged), `device.py gen qemu-keymap` (from `keymap.toml`, qcode names as
QEMU's `QKeyCode` enum strings), `device.py qemu-args` adds `-global syborg-keyboard.keymap=…`;
baseport `keyboard/` patch: read `MODE`, pass through when set, else the set-1 table (fixture ROM
and old images unchanged). `board.input.translation = "qemu"` in the N95 profile. The generated
EKEYB table from step 5 stays for the fallback path.
Test: smoke; a key added only in `keymap.toml` (e.g. the Edit key) reaches the phone after `sydb
restart` without a guest rebuild; the PDK text-shell fixture still takes keys (`scripts/boot-test.sh
--fixture`).

## Step 8 [S] — board tells the guest the rest (RAM, display HAL, device id)

Files: `syborg.c` platform device: `DEVICE_ID` string window and `RAM_MB` register, properties
`syborg-platform.device-id`, machine `-m` already sets RAM; bootstrap patch 0010 → read `RAM_MB`
from the platform device (small assembler change in `bootstrap/syborg.inc`/`syborg.s`, falling
back to the constant when the register reads 0); LCD HAL extension: one `lcd.dll` reading bpp,
mode count, current mode, native orientation and pitch from new fb registers (`BPP`,
`MODE_COUNT`, `NATIVE_ORIENTATION`, `PITCH`; `-global syborg-fb.*` from `[board.display]`),
dropping `lcd_n95.mmp`/`[kernel.lcd]`; `device.py gen dtb` writes a DTB from the profile (display,
drives, device id) instead of patching the baseport's.
Test: `sydb hmp 'info mtree'` shows the registers; `ekern` prints the device id at boot
(`Variant installed … device nokia-n95-8gb-rm320`); HAL `EDisplayXPixels/YPixels/
EDisplayBitsPerPixel` unchanged (`sydb` agent `halinfo` or a test app); the kernel-side ROM is now
identical between the N95 and the RM-321 profile; smoke.

## Step 9 [L] — personalities selected at run time

Files: `sexec92.cpp` → `sexec_personality.cpp` with every personality's generated map compiled in
(`personalities/*/exec.map` → one header each); ROM header config bits 29–31 = personality number
(`romcompose --exec-personality NAME`); `kabi` deltas that need code (`0008`) stay compile-time
until a second personality needs a different vtable shape (then: proxy objects, hacks.md option 1).
Test: `--exec-personality native` boots our own text shell ROM, `sym92` the N95; smoke.

## Step 10 [L] — GL per device

Files: `src/n95gl/` → `src/sygl/`; `libgles_cm.ordinals`, `n95imports.txt`, identity strings →
`devices/ID/gl/` and `[gl.identity]` (generated `identity.h`); `build.sh` takes `--device ID`
(blob address, output dir); `gen-gles-glue.py` reads the device symindex.
Test: `sygl.bin` and `gles-patches.txt` identical to pre-step outputs for the N95; `tests/sygl/run.sh`
green; smoke with both backends.

## Step 11 [L] — stand-in families and the modem state at run time

Files: `[modem]` knobs (`sim`, `battery_bars`, `imei`, `power_off_delay_s`) read by `iscpeer.inl`
from the platform device/DTB instead of constants, so one `nokiaiscdriver.ldd` build serves every
BB5 phone; family READMEs in `platform/families/`.
Test: `sydb start` with `[modem].battery_bars = 3` shows three bars without a rebuild; smoke.

## Step 12 [L] — docs split

`docs/hacks.md` keeps the kernel/QEMU/board sections; the "N95 hardware stand-ins", "User-side code
patches" and ROM sections move to `devices/nokia-n95-8gb-rm320/README.md` (hack ids unchanged,
with a pointer left behind). `docs/n95-*.md`, `nokia-*.md` → `docs/devices/nokia-n95-8gb-rm320/`
with the old names as one-line redirects for a release. `CLAUDE.md`: the section above.
Test: every link in `docs/` resolves (a `tests/docs-links.sh` grep); `grep -rn build/n95 scripts
tools` returns only the compatibility symlink note.

## Order and gates

- Steps 0–5 before anyone starts a second device (they remove every N95 literal from scripts and
  tools and make the profile the source of the keymap, ROM and QEMU arguments).
- Step 6 is the first thing a second device needs; do it together with that device's inventory.
- Steps 7–8 remove the last guest rebuilds that depend on board facts (keys, RAM, display HAL).
- Steps 9–12 are growth and hygiene; none blocks a new device.
- After each step: smoke green, `tools/device.py check` green, `docs/hacks.md`/device README
  updated, local commit.
