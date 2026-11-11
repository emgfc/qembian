# Cleanup review (2026-11-07): configuration slop, per-device code that should be common, leftovers

Static review only (no QEMU runs). Claims are marked [verified in source] (read in the file named)
or [inferred]. GPU and TLB topics are covered by docs/gpu-host-review.md and docs/perf.md and are
not repeated. "all six" below means the six full profiles (N95 8GB, N73, 5730, N79, 5800, N8); the
RM-321 and N8 v13 profiles are `extends` variants.

Method: every key of the six profiles was flattened and compared (which keys exist where, which are
equal everywhere), then grepped for readers in tools/, tests/, scripts/, src/.

## Status

| Item | State |
|---|---|
| 1.1 unread / non-phone fields | done 2026-11-08: `expected_idle_s` (smoke test: `--budget` / `SMOKE_BUDGET`, default 90), `startup_ui`, `first_output`, `drm`, `hwrm_light`, family `wserv`, `board.display.bpp/hal_modes/hal_mode/partial_mode/rotation_caps`, `kernel.lcd.macros`, `modem.stateful`, the family's unread modem knobs and `board.audio.host_default` deleted; `refresh_hz` / `blank_dim_percent` are tool defaults in device.py; stale family/README texts and the N8 status fixed |
| 1.1 `[props]` vs `milestones.idle`, `[status]` rot, `kernel.trace_mask`, `rom.manifest`, `[skin]`, `[composition]` docs, lcd build rename | open |
| 2.1 modem union table | done 2026-11-08: platform/isi/replies-bb5.txt holds all 40 replies (`# on:` tags), device files are headers only, `F3 30` is 58 bytes for every RAPIDO phone, `tools/isi.py lint`; generated tables changed only by additions (plus the F3 30 length); smoke tests green on all six devices |
| 2.2 RAPIDO DSP one configuration | done 2026-11-08: `[board.audio] dsp = "rapido"`, `ring_units = 16` on the 5730, N79, 5800, N8 (board config `audio.ring_units`, read by aamdaifdriver and the ISC peer); per-device stand-in config gone; `[hardware] dsp` corrected; smoke tests green on all six. The rapido_dsp.inl code split is not done (the two readers share the key, not code) |
| 2.3 board-config key naming | done: `modem.imei`, `modem.power_off_ms`, `medrofs.flash_mb`, `d_display.caps/native/pitch/rows`, `audio.ring_units` |
| 3.x leftovers | done 2026-11-08: scripts/run-n73.sh and tools/pcsample.py removed, scripts/n95-drive.sh -> scripts/drive.sh (old name kept as a symlink), `[composition]`/`[board.audio]`/`[gl.host]` documented and the schema text matches the profiles. Left for the owner: measure-boot.sh/boot_milestones.py, the legacy container path, single-reference research tools, src/n95apps rename |

## 1. Configuration slop

### 1.1 Field by field

| Field | Where | Who reads it | Verdict | Action |
|---|---|---|---|---|
| `milestones.expected_idle_s` | all six (n95:291, 5730:256, 5800:275, n8:267, n73:256, n79) | `tests/device-smoke.sh:16-17` only (budget = value x 1.5, fallback 26) [verified in source] | delete | The smoke test takes `--budget S` / `SMOKE_BUDGET`, default a generous constant (60 s) or the `sydb start --timeout` default. Boot time depends on the host; a profile cannot promise it. The N8's line is a verbatim copy of the 5800's, comment included ("[V] 8.6 s from a seed") [verified: n8:267 = 5800:275], so the [V] tag is false. |
| `milestones.startup_ui`, `first_output`, `drm`, `hwrm_light`, family `wserv` | all six; `first_output = ""` on all six; drm/hwrm_light N95 only; wserv family:59 | nobody. `tools/boot_milestones.py:5-8` hard-codes its own list (wserv, drm strings), `tools/sydb/daemon.py:18` reads only `milestones.idle` [verified in source] | delete, or wire | Either make `boot_milestones.py` read `[milestones]` (then `first_output = ""` still goes) or drop everything but `idle`. The N73's `startup_ui` comment says "(9.3: 0x46)" while the value is `/4` (n73:253): copy-paste. |
| `milestones.idle` | all six | `daemon.py:18` (fallback literal = the N95's pattern) | keep | It names the firmware's own log line (3 distinct values on six devices): a device fact. Consider deriving it from `[props].startup` (below) so one place holds the numbers. |
| `[props].startup` | all six, `category`/`state` identical, `ui_phase` 2 values | nobody (`syb_report.py:93` parses SYBPROP lines generically; no tool reads the table) [verified in source] | keep one of props / milestones | `milestones.startup_ui` and `idle` embed the same category/key numbers as strings. Keep the structured `[props]` and build `idle` as `"sets %x/%x = %d"` from `{category, state, value}`, or delete `[props]`. |
| `[status].level/updated/open` | all six | `device.py:691-692` (`list` prints level) | keep, but it rots | N8: `level = "L0-pending"`, `open = ["first boot"]` (n8:272-274) while commit fe76982 and devices/README.md say the N8 013.016 boots to the home screen [verified: git log, README]. 5730/5800 both say "L1 ... (2026-11-07)". Either keep it honest (a `device.py check` warning when `updated` predates the profile's last git change is cheap) or move status to `devices/<id>/README.md` and keep only `level` in the profile. |
| `[hardware]` (13 keys) | all six | nobody in code (docs/devices.md says skin + checklists; `phone.py`, `inventory.py` do not read it) [verified in source] | keep as phone facts | Cheap, correct, and the only machine-readable "what the phone has". But `board.audio.dsp = "none"` (5730:86, 5800:87, n8:89, comment "no stand-in yet") is stale: `aamdaifdriver` is in all three `[[standins]]` lists. Set it to `"rapido"` and let it name the DSP model (section 2.2). |
| `board.display.bpp`, `hal_modes`, `hal_mode`, `partial_mode`, `rotation_caps` | all six (bpp/hal_* identical, "N95 numbering" comment copied) | nobody: `device.py` reads width/height (421), touch (469), refresh_hz (474), blank_dim_percent (476), caps_words/native_orientation/line_pitch/panel_rows (605-606) only [verified in source]. `rotation_caps` is hard-coded in `d_display.cpp:69`; bpp/modes are macros in `src/standins/lcd/lcd_n95.mmp:8-12`, `lcd_portrait.mmp:9-12` | generate or delete | Pick one source of truth: generate the LCD macros and the d_display caps word from these fields (then they earn their place), or delete the fields and keep the mmp/KCaps values. Today they are documentation that can drift. |
| `kernel.lcd.macros` | all six, identical except `SYBORG_FB_N95` on N95 and 5800 | nobody: `device.py:229` and `build-standins.sh:26` read `kernel.lcd.target` only; the macros live in the mmp files [verified in source] | delete | Same values twice. If generation is wanted, generate from `board.display` (above), not from a second copy. |
| `kernel.lcd.target = "lcd_n95"` on the 5800 (5800:161) | 5800, N95 | build | rename | `lcd_n95` vs `lcd_portrait` are named after a device, not after what differs (native-landscape panel + always-on: `SYBORG_FB_N95`, lcd_portrait.mmp:4). Rename the build to the property (`lcd_landscape_panel` or similar). Whether the 5800 really has a native-landscape panel is not checked here [inferred]. |
| `board.display.refresh_hz = 60` ("assumed, not measured"), `blank_dim_percent = 25` | all six, identical | `device.py:474,476` -> QEMU globals | move to tool defaults | Board tunables of ours, not phone facts. Keep them in `device.py` as the default; a profile states them only when measured on the phone (H-vsync's "Real implementation" column asks exactly for that measurement). |
| `board.audio.host_default = "pipewire"` | family:24 | nobody in tools/ scripts/ (grep) [verified in source] | delete | Host-specific; the audio backend belongs to the session / `run-modern.sh` environment. |
| `kernel.trace_mask` | family:34 | `device.py` | move | A debugging knob, not a family fact; `sydb start`/`syb_run` already take `--trace`. Keep `kernel.variant = "udeb"` (build-time) under "how we run every member". |
| `rom.manifest = "hybrid-manifest.txt"` | family:56 | `device.py`, `romcompose.py`, `syb_sym.py`, `kabi.py`, `pcsample.py` | move to tool default | The name of a tool output file. `build/devices/<id>/hybrid-manifest.txt` can be the convention in `device.py paths`. |
| `modem.sim`, `mtc_state`, `battery_bars`, `charger`, `rtc_valid`, `phone_lock` | family:47-53 | nobody: `device.py:586-598` passes only `imei`, `power_off_delay_s`, `indications`, `sim_service_status` to the board [verified in source]; the values are hard-coded in `replies-bb5.txt` (17 0A) and `iscpeer.inl:637-641` | delete (or wire) | They describe what the code does, so they can only drift. Delete now; wire a knob the day a device needs another value (battery bars would be one line in `device.py` plus a generated reply). |
| `modem.stateful` | all six (2 values) | nobody (grep tools/ src/) [verified in source] | delete | The `[[standins]]` list already names the ISC driver that carries `iscpeer.inl`. |
| `modem.device_id = 0x6C` | family:43 | `device.py`, `syborg.c` | keep | A BB5 fact. |
| `board.ram_mb_real` | all six | nobody | keep or move | A phone fact "for the record"; harmless. `[hardware]` is the better home if kept. |
| `device.market/notes`, `firmware.obtain`, `platform.os_version/ui/soc/cellular` | all six | nobody in code (identity shown by docs) | keep | Phone identity. `platform.cellular` should start doing work: select the shared reply layer (section 2.1). |
| `firmware.checksums = "firmware.sha256"`, `firmware.container = "fiasco-bb5"`, `rom.files` (ESTARTCOMP), `device.vendor` | all six, identical | device.py, inventory.py | keep (rule) or make defaults | Equal by coincidence per the family rule, so they stay per device. `checksums` is a tool convention and could become the default file name instead of a field. |
| `[skin]` | N95 only (n95:296-298) | not found in `tools/sydb/phone.py` (reads `keymap.toml`) [inferred: no grep hit for `skin.layout`/`orientation_key`] | wire or delete | |
| `[composition]` | N8 | `device.py:413-417` | document | Read by the build but absent from docs/devices.md section 2.1 (only line 31 and `[gl.host]` mention composition) [verified in source]. |

Stale comments worth a sweep (all [verified in source]): n95:19 "(today personalities/sym92, ...)",
n95:252 "driven today by scripts/hybrid-boot.sh", n95:266 "src/sygl (to become src/sygl)",
n73:253 "(9.3: 0x46)" next to `/4`; `devices/nokia-5800-rm356/isi-replies.txt:1` and
`devices/nokia-n8-rm596/isi-replies.txt:1` both open with "replies of the N79 (RM-348)".

### 1.2 The inverse rule: what the family file holds

`platform/families/bb5-s60.toml` [verified in source]:
- Not family facts: `board.audio.host_default` (host), `kernel.trace_mask` (debugging),
  `rom.manifest` (tool file name), `milestones.wserv` (our stand-in's line, unread), the six unread
  `[modem]` knobs. Remove per the table above.
- Stale description: line 1 says "running S60 3rd Edition ... (N95 8GB, N73, 5730)" and
  docs/devices.md:81 says "Today one family, bb5-s60 (N95 8GB, N73, 5730)". The family now also
  carries the 5800 (S60 5th) and the N8 (Symbian^3). The name "bb5-s60" is still defensible (S60
  P&S keys hold on ^3) but the text is wrong; fix it or rename to `bb5`.
- Fine: `platform.os/cellular`, `board.machine/timing/flash.base/input`, `kernel.variant/
  secondary/critical_thread_policy/user_memory_guards`, `modem.kind/names/replies/device_id/imei/
  power_off_delay_s`, `props.sim_status`.

A second, unofficial family exists by copy-paste: 5730, N79, 5800 and N8 share a byte-identical
`[modem]` block (indications + sim_service_status, comments included: 5730:235-243, 5800:244-252,
n8:246-254), identical `[milestones]` (`Startup::startup sets 101f8766/46 = 104`), identical
`kernel.place`, `board.display.caps_words/panel_rows`, the same `isi-replies.txt` (section 2.1).
The 5730 and N79 profiles differ in 54 non-comment lines out of ~265 [verified: diff]. These are
facts of the 9.3+ start-up software (SAStartupService, cdspcontrolserver), i.e. shared for a
reason that will hold for the next 9.3+ BB5 phone. The clean declarative home is a second layer
selected by `platform.cellular = "rapido"` (or `platform.os_version >= 9.3`): a reply file
`platform/isi/replies-rapido.txt` in `modem.replies` and, if families ever nest, a
`bb5-s60/rapido` family. Until then, a `tools/device.py lint` that reports keys equal across
every member of a family (candidates) and keys the family holds that nobody reads (slop) keeps
this from growing silently.

## 2. Commonalities implemented per device

### 2.1 Fake modem (ISI replies)

What the files hold today [verified in source, all seven files printed side by side]:
- `platform/isi/replies-bb5.txt`: 20 replies, shared.
- N95: no replies of its own (comments only). N73: 4 replies in the 9.1 numbering (`1B 02`
  INFO_PP_READ old id, `09 3B` IMSI-equal, `09 29`, `09 31`).
- 5730 and N79: identical (one comment line differs). 5800 = N79 + one comment. N8 = N79 + eight
  N8-only replies (`0B 04`, `0B 5D`, `09 DE`, `09 09`, `1C 38`, `1C 9E`, `19 19`, `F3 36`) and one
  change: `F3 30 -> 31 len=58` instead of `len=34`.
- Code paths in `iscpeer.inl` are already common and configured, not branched by device: MTC,
  SIM_STATUS per service (`modem.simst`), indications (`modem.ind`), SEC_STATE (a 3-byte reply
  that satisfies both the 9.2 one-byte and the 9.3 three-byte readers, iscpeer.inl:695-703), IMEI,
  lights, RAPIDO `F3 34`.

Is the owner right that one complete reply set can serve all BB5 phones? Largely yes, with two
real exceptions:
1. Same (resource, request), different expected layout: `F3 30/31`, 34 bytes on the 5730 (four
   entries read) vs 58 on the N8 (seven entries, `Aamm.dll 0x805e8106`). A superset reply (58
   bytes) should satisfy the 5730's parser if it reads fixed offsets, as the doc describes
   [inferred; verify once against cdspcontrolserver ConstructL 0x80931f18]. This is the same
   trick the code already uses for SEC_STATE.
2. Same message, different meaning by firmware generation: SIM_STATUS service 01 must be `01`
   for the 9.3 SAStartupService (5730:241-243) while the 9.2 NokiaDSY maps status `01` to "card
   present, wait for SIM init forever" (iscpeer.inl:666-667). Whether the N95's NokiaDSY ever asks
   service 01 is not recorded; if it does, the value is a genuine per-generation variant and must
   stay declared (`sim_service_status`), not merged.
Everything else is additive: a canned reply to a request a firmware never sends is inert, and the
N73's 9.1 ids and the N8's ^3 ids do not collide with anything. The "deliberately unanswered"
`0A 00` is already common to all. So: one union table, two declared variants.

Proposed design:
- `platform/isi/replies-bb5.txt` becomes the union of every device file (N73 and N8 additions
  included), each line tagged with where it was verified (`# on: 5730 n79 5800 n8`) so coverage is
  visible per reply. `F3 30` goes in at 58 bytes after the 5730 check.
- Device files become empty or hold only true conflicts; the layering (`isi.py:43`, later layer
  wins) stays as the mechanism for a future conflict.
- `tools/isi.py lint`: reports (a) replies identical across all device files (move up), (b) the
  same (res, req) with different bytes (declare), (c) stale headers. Cheap: the loader exists.
- `[modem].indications` and `sim_service_status` for the 9.3+ members move into the shared layer
  once the `cellular`/generation layer exists (the indication is gated on subscription and
  channel 35, so it is inert on 9.2 phones [verified: iscpeer.inl:604-605]).
Migration: (1) write the union file + lint; (2) build each device and compare the generated
`isireplies.h` with the previous one (the only differences must be additions); (3) smoke-test the
5730 first (the F3 30 length change), then the others; (4) delete the device files that are empty.
Risk: a longer-than-expected reply can also panic a parser that checks `len` strictly (the
"OFFS -9" family of panics is about too short, not too long, in every case recorded) [inferred].
What stays per device: nothing in the reply table by design; `sim_service_status` per generation;
`imei` per profile.

### 2.2 Audio / DSP

Two genuinely different hardware protocols, correctly two stand-ins [verified in source]:
- TI DSP/BIOS bridge (N95 OMAP2420, N73 OMAP1710): `ehwa` + `ehwaomap`, the OMAP1710 variant
  declared as `ehwa.bridge = "omap1710"` (n73:208, ehwa.cpp:459-473). Keep.
- RAPIDO (5730, N79, 5800, N8): `aamdaifdriver` (MdaifDriver controls) plus ISI resource `F3`
  handled half in canned replies and half in `iscpeer.inl:683-694` (F3 34 "data written", the
  virtual DSP clock). Keep as one model, but it is split today:
  - `aamdaifdriver.ring_units` is read in two stand-ins (`aamdaifdriver.cpp:100`,
    `iscpeer.inl:691`): the modem peer knows about the audio driver's ring. The virtual DSP
    clock is an audio-model fact, not a modem fact.
  - `ring_units = 16` on the N8 only (n8:212); 0 on 5730/5800 "as tested" (aamdaifdriver.cpp:98-99).
    The ring is what the real DSP has; 0 is the state the first port happened to work with, not a
    hardware difference. Accidental divergence.
  - `board.audio.dsp = "none"` on all three RAPIDO profiles while the stand-in is listed.
Proposed design: a `src/standins/_shared/rapido_dsp.inl` (ring geometry, the virtual clock, the
F3 30/32/36 reply contents that depend on the ring) used by both `aamdaifdriver` and the ISC peer,
configured once from `[board.audio] dsp = "rapido"`, `ring_units = 16` (board config
`audio.ring_units`), default 16 for every RAPIDO phone. Migration: set 16 on the 5730/5800/N79,
smoke-test each (risk: a 9.3 aamdaif that wraps differently; the doc says size 0 "never wraps", so
a non-zero ring is the first time those clients wrap [inferred]); then move the `F3 34` branch out
of `iscpeer.inl` behind a `RapidoDsp::DataWritten()` call; then delete the per-device `config`.
What stays per device: nothing for RAPIDO unless a client proves a different ring; `ehwa.bridge`
for the TI side. The F3 30 reply length (section 2.1) is a parser difference, not a DSP difference.
H-startuptone-none (5800, N8 hacks.md "Real implementation: a RAPIDO DSP model") is not retired by
this: the tone needs sound output, which is still "Towards real sound (not done)"
(docs/devices/nokia-n8-rm596/rapido-audio.md:85).

### 2.3 Other stand-ins

- The ISC link comes in four directories of 4-7 lines each (`iscdriver`, `nokiaiscdriver`,
  `iscdriverextension`, `nokiaiscdriverextension`), all `#define ISC_LDD_NAME ... #include
  "iscpeer.inl"` [verified in source]; `nokiaiscdriverextension` already takes its name from
  `[[standins]] options` (n95:120). One `iscpeer` directory built under four targets via
  `options = { ISC_LDD_NAME = ..., ISC_CHANNEL_BASE = ... }` would need a `target` key in
  `[[standins]]` [inferred: build-standins.sh derives the target from `name`]. Low value; the
  duplication is 20 lines. Do it only if `[[standins]]` grows a `target` key for another reason.
- Board-config key naming is inconsistent [verified in source]: `<standin>.<key>` for declared
  variants (`ehwa.bridge`, `custom.api`, `exyin.rotate`, `aamdaifdriver.ring_units`),
  `modem.ind`/`modem.simst` for two modem knobs, but bare `imei` and `power_off_ms`
  (device.py:586-589, iscpeer.inl:559,723) and `disp_caps/disp_native/disp_pitch/disp_rows`
  (device.py:605-606, d_display.cpp:46-52) for others. Normalise to `modem.imei`,
  `modem.power_off_ms`, `d_display.caps` etc.; one sweep over device.py and four stand-ins.
- `d_display` rotation caps are hard-coded (`d_display.cpp:69`) while the profile has
  `rotation_caps` (section 1.1): wire or delete.
- Load-only stubs (xbus, iscisimultiplexer, sosadlserver, sisadriver, lightldd/pdd, usbaudiodriver,
  dvfs_interface) are 4-19 lines on top of `nokiastub.h`: already common. Nothing to do.

## 3. Other cleanup, ranked by value/effort (small and safe first)

1. Delete `scripts/run-n73.sh` (4 lines, zero references [verified: grep]; a per-device wrapper
   around `SYBORG_DEVICE=... sydb phone`, the pattern CLAUDE.md's device rules exist to prevent).
2. Fix the stale headers of `devices/nokia-5800-rm356/isi-replies.txt` and `devices/nokia-n8-rm596/isi-replies.txt`
   ("N79"), the N8's copied `expected_idle_s` comment and `[status]`, the N73's `startup_ui`
   comment, the family file's line 1 and docs/devices.md:81. Ten minutes.
3. Document `[composition]` in docs/devices.md 2.1 (read at device.py:413).
4. `scripts/n95-drive.sh` is a generic mtools helper (DRIVE_C/DRIVE_E) used by `sydb.py:95-99`
   for every device [verified in source]; rename to `scripts/drive.sh`, keep the old name as a
   symlink for one release. Same family: `src/n95apps/` and `build/n95apps/` hold the dev agent and
   test apps for all devices (sydb.py:49-50, romcompose.py); a rename to `src/guest-apps/` is
   bigger (build scripts, docs) and can wait.
5. `tools/pcsample.py` (gdbstub, stops the vCPU) vs `tools/pcsample_hmp.py` (monitor): docs/tools.md:183
   documents only the hmp one [verified in source]. Delete `pcsample.py` or fold it in as
   `--via gdb`.
6. `scripts/measure-boot.sh` + `tools/boot_milestones.py` duplicate what `sydb start --timeout`
   plus `syb_report.py`'s timeline give (the milestone list is hard-coded in both places, section
   1.1). Keep one: make `syb_report` print the milestone times from `[milestones]` and drop the
   two files, or make `boot_milestones.py` read the profile. [inferred: measure-boot's
   `boot-times.tsv` history may still be wanted; check docs/perf.md before deleting.]
7. `tools/syb_run.py` docstring "boot the N95 hybrid" and two N95 literals; CLAUDE.md still names
   it for one-shot boots, so keep it, but it should take the device like everything else.
8. Single-reference tools: `tools/n95_installserver.py` (referenced from its own doc only),
   `tools/rcomp.sh`, `tools/dso_ordinals.py`, `tools/ksyms.py`, `scripts/build-rom-trace.sh`,
   `scripts/fix-toolchain-libgcc.sh` [verified: reference counts]. Move the research-grade ones
   under `tools/research/` or list them in docs/tools.md with a one-line purpose; delete what the
   owner no longer uses (not decided here).
9. Legacy-container path (`scripts/run.sh`, `boot-test.sh`, `build-qemu.sh`, `qemu-container.sh`,
   `in-container.sh`): still referenced from docs (3-6 references each) [verified: counts only].
   If the modern QEMU is the only path now, retire them together with their docs section;
   otherwise leave. [inferred]
10. `[gl]` sits in the N95 profile only (n95:262-281) with `board.gl` (device/backend) also N95-only;
    the N8 uses `[composition]` + `[gl.host]`. Two phones, two shapes of the same concern (host
    GPU glue). docs/gpu-host-review.md owns this; noted here only so the profile schema gets one
    `[gl]` with `impl = sygl | gcebe | none` when that review is acted on.
11. Hack rows whose "Real implementation" is now cheap: H-vsync "measure the real N95 rate" is
    still open and is the only thing that would justify `refresh_hz` in a profile; the five
    `[modem]` knobs (section 1.1) pretend H-em/H-rtc are configurable when they are not.
12. docs/evidence/ contains our own eshell screenshots and build logs (clean-build-eshell-dir.png
    etc.) [verified: git ls-files]. Not firmware screenshots, so allowed; fine.

## 4. Action plan for one limited session, in order

1. Config slop, no behaviour change (one commit): remove `expected_idle_s` (smoke test gets
   `--budget`, default 60 s), `first_output`, `modem.stateful`, the six unread `[modem]` knobs,
   `board.audio.host_default`, `kernel.lcd.macros`; fix the stale comments and headers listed in
   section 1.1 and item 3.2; set `board.audio.dsp = "rapido"` on the RAPIDO profiles; fix the N8
   `[status]`. Run `tests/device_py_test.py` and `tools/device.py check` for every device.
2. `tools/isi.py lint` (identical-across-devices and conflicting replies), then the union
   `replies-bb5.txt` with `# on:` tags and the 5730 check of `F3 30 len=58`. Smoke-test 5730, N79,
   5800, N8, N95, N73 (six runs; the owner's session untouched).
3. RAPIDO ring default 16 for all four phones, smoke-test; then the `rapido_dsp.inl` split and the
   `modem.*`/`d_display.*` board-config key normalisation (one sweep, rebuild, smoke-test).
4. Tool overlap: delete `run-n73.sh`, `pcsample.py`; decide measure-boot vs syb_report; rename
   `n95-drive.sh`.
5. Later: `tools/device.py lint` for the family rule (equal-everywhere keys, unread family keys),
   the `cellular`/generation reply layer, `[props]`-derived milestones, LCD macro generation.
