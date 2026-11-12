# qembian debugging tools

Quick reference for people and agents. All tools run on the host from the project root.

**Live phone first:** [sydb](sydb.md) (`tools/sydb/sydb.py`, MCP `mcp__sydb__*`) is the bridge to a
running phone: apps, files, processes, UI keys/screenshots, keyguard, P&S, IPC strace, persistent
gdb with symbols. Use it instead of ad-hoc QEMU/gdb runs; the tools below are for scripted boot
experiments, ROM work and offline analysis.

The N-Gage (EKA1) board has its own tools (`eka1/tools/ngage_phone.py`, `ngage_sess.py`,
`ngage_run.py`): see [eka1-ngage.md](eka1-ngage.md).

## Build containers: `scripts/in-container.sh`

```
scripts/fetch.sh                                   # pinned inputs (sources.lock) into external/, downloads/
scripts/in-container.sh CMD                        # legacy image: Symbian host tools, kernel, baseport, stand-ins
CONTAINER=modern scripts/in-container.sh CMD       # modern image: QEMU deps, GCC 14 symbianelf, elf2e32_next, Python
scripts/build-qemu-modern.sh --container           # QEMU in the modern image -> build/qemu-modern-ct/
SYBORG_QEMU=container scripts/run-modern.sh ROM    # run that QEMU (automatic when there is no host build)
S60SDK=/path/Epoc32 CONTAINER=modern scripts/in-container.sh src/sygl/build.sh   # guest code, SDK mounted ro
```
Podman by default, `ENGINE=docker` for Docker. Images build themselves on first use
(`container/Containerfile`, `container/Containerfile.modern`; the modern one compiles the GCC 14
toolchain once, about 10-20 min). Nothing proprietary goes into an image: the S60 SDK and firmware
are mounted at run time. Details: `docs/adding-a-device.md` §8.

## Boot, observe, summarise: `tools/syb_run.py`

```
tools/syb_run.py --secs 30 --shot 5 --shot 15            # compose (IPC trace on), boot, report
tools/syb_run.py --trace 0x80000000 --secs 20            # quiet/fast trace mask
tools/syb_run.py --no-compose --key 20:ret --type 25:abc # reuse ROM, send keys/text at times
tools/syb_run.py --no-compose --keep-drives build/runs/D  # reuse C:/E:/RTC offset across runs
```
Writes `build/runs/<label>/` with `serial.log` (seconds-prefixed lines), `shot-*.png`,
`report.txt`. The report (`tools/syb_report.py LOG [--full] [--ipc-errors]`, works on any serial
log) lists: processes, exits/panics/kills, SYBHACK and faults, caught kernel exceptions per thread,
startup properties, stub driver activity, unanswered ISI requests, and **IPC requests still pending
at the end**: "likely blocked" = the client issued nothing after that request (a synchronous wait);
the rest are grouped async notifications. Use it first for any "UI does not advance" problem.

## Addresses and symbols: `tools/syb_sym.py`

```
tools/syb_sym.py addr 803a86dd 80012d04       # module+offset, export/function, thunk, literal
tools/syb_sym.py sym 'RTimer::At'              # exports by (demangled) name
tools/syb_sym.py callers 'CMdaAudioPlayerUtility::NewFilePlayerL' --sites
tools/syb_sym.py str LightPdd --refs           # strings and the code that references them
tools/syb_sym.py read 8091cb30 8 | lit 8091f780
tools/syb_sym.py fn SALightsService.01.dll 8091ca36   # one decompiled function (Ghidra on demand)
tools/syb_sym.py mod HWRMServer.exe
```
Index per firmware in `<build dir>/symindex.json` (`tools/device.py paths symindex`; `$SYBORG_DEVICE`; rebuilt when `core.json`
changes; `reindex` to force), export names from the `.dso` ordinals of the profile's `firmware.sdk_lib`
(EUSER-style ordinals are append-only, so a name is the nearest export *below*: an internal function
shows as `Export+offset`). Addresses below the firmware's user window resolve to **our** kernel-side
files (`ours.json`, `hybrid-manifest.txt`) with function names from the device's `_<baseport>_ekern.exe.sym`.
RAM-loaded ROFS code is not indexed (see `tools/e32load.py`).

## ROM blobs: `tools/blob_glue.py`

Glue for code placed at a fixed ROM address (src/gcebe; sygl has its own generator):
`imports --list FILE --asm OUT.S --header OUT.h` turns `<macro> <module> <mangled name | ordinal>`
lines into veneers to the device's ROM exports; `rebind --elf BLOB.elf --out PATCHES.txt
MODULE:ORD=SYMBOL[@IMPORTER]` rewrites the XIP import thunks that target an export so they call the
blob instead (romcompose `--patches` lines).

## Runtime probes: `tools/syb_gdb.py`

```
tools/syb_gdb.py leaves --modules HWRMServer,SALights --max 10   # who leaves, with which error
tools/syb_gdb.py brk 'User::Leave(int)' --print '$r0' --stack --max 3
tools/syb_gdb.py brk 0x838f1340 --when-thread HWRM --print 'TheScheduler.iCurrentThread->iPriority'
tools/syb_gdb.py leaves --err -14 --key 60:f5 --key 140:right   # QMP keys (wall clock) during a probe
tools/syb_gdb.py sample --at 12 --n 5      # mode, current thread, symbolised PC/LR/stack chain
tools/syb_gdb.py thread --at 15
```
Each probe first prints `PROBE <loc> at <addr> <symbol>`; when nothing fires, the tail of gdb's own
output (`build/runs/gdb-<cmd>.gdb.txt`) is shown instead of silence. At a kernel-mode stop inside an
exec call the user call site is printed (`user pc <module+off> (9.2 exec S0a ChannelRequest)`), so a
kernel panic such as `KERN-EXEC 0` names its caller: `brk 'K::PanicCurrentThread(int)' --when-thread X`
(bad-handle panics go there directly, not through `K::PanicKernExec`).
`--keep-drives DIR` boots with the drives and RTC offset of a `syb_run.py --keep-drives DIR` run
(for probes that need a phone past its first boot).
Boots the ROM as it is (the device's `hybrid.img`, `tools/device.py paths rom`) with the gdbstub, loads the kernel's DWARF
(`_syborg_ekern.exe.sym` at code 0x80003000, data 0xc8000000), so kernel expressions work; names
the current thread from `TheScheduler`; kills stray QEMUs first. Output also in
`build/runs/gdb-<cmd>.log`. Hardware breakpoints have no practical limit under TCG.

## Device profiles: `tools/device.py`

```
tools/device.py list | firmwares | registry [RM-xxx]      # devices; models with their firmwares; type codes
tools/device.py show|check|paths [ID] | get [ID] KEY | file [ID] NAME   # merged profile, one value, a profile file
tools/device.py firmware-add MODEL RM-xxx/<version> [--rofs2 ID] [--rofs3 ID]   # another firmware of a model
tools/device.py build [ID] [--variant V] [--kernel] [--trace MASK] [--out IMG]  # compose the ROM
```
ID: a model (`nokia-n8-rm596`, its default firmware), `MODEL@FW`, a firmware's own id
(`nokia-n8-rm596-v13`), else `$SYBORG_DEVICE`, the instance's device (`$SYBORG_INSTANCE`), or
`models/registry.toml` `default`. `paths` prints the build dir (`dir`, `build/roms/<id>/<firmware key>/`)
and the store/cache dirs; scripts take every path from it. Schema: docs/devices.md.

## Firmware identification: `tools/fw/identify.py`, `tools/fw/layout.py`

```
tools/fw/identify.py roms/RM-596/011.012      # or flash files, or a .zip/.rar/.7z; --json
tools/fw/layout.py FILE...                             # what each flash file holds (no extraction)
```
```
tools/fw/store.py import FILES|DIR|ARCHIVE [--label L] [--move]   # into roms/<RM>/<version>[~label]/
tools/fw/store.py list | show RM-596/011.012 | path ID | reindex
```
The ROM store (device-model-v2 2.2.2, 2026-11-10): `roms/<type code>/<version>/` with the original
files, package metadata in `package/`, a generated `rom.toml` (identity, products, every file's role,
pack id, partitions, sha256) and `roms/index.toml`. A label separates firmwares of one version with
other files (custom firmware: `RM-596/111.040.1511~delight-6.7`). Archives stay where they are.

`identify.py` says what flash files are, from the files (device-model-v2 section 3): the `.vpl`
(type code, version, products), else `sw.txt`/`langsw.txt` read from ROFS1/ROFS2 in memory, else the
file name; language/customer/UDA files join the core they came with. It prints the registry row
(`models/registry.toml`) and the profiles whose `[firmware.identity]` equals it. `layout.py` is the
partition reader `inventory.py` extracts by (core, ROFS1, language and customer ROFS, paging), so a
profile declares no layout. Not yet read: FIASCO tag 0x30 (E71 RM-346 files).

## Firmware facts: `tools/fw/probe.py`

```
tools/fw/probe.py --device ID header              # memory model, kernel limit, lowest user-side file, fixed processes
tools/fw/probe.py --device ID stubs --vs n95-8gb  # EUSER SWI stub shape and ordered sequence vs a known device
tools/fw/probe.py --device ID exec-sizes --vs n95-8gb   # slow exec handlers whose copy-in sizes differ
tools/fw/probe.py --device ID rofs | display | estart | files REGEX | cat PATH
```
Reads the device's build dir (`tools/device.py paths dir`) as written by `tools/fw/inventory.py`; prints only. Keypad matrices and
the display driver's caps size still need a disassembler (`tools/xipdis.py`).

## Modem (ISI): `tools/isi.py`, `platform/isi/names.txt`, `models/<id>/isi-replies.txt`

```
tools/isi.py name 09 1d                 # PN_SIM/SIM_IMSI_REQ_READ_IMSI
tools/isi.py log build/runs/X/serial.log [--all]   # unanswered requests, or every message decoded
```
`platform/isi/names.txt` holds the resource/message names of the BB5 modem (with evidence tags),
shared by every BB5 device; each device's `isi-replies.txt` (profile `[modem].replies`) is its canned
reply table, layered on the family's shared table (`modem.replies = ["platform/isi/replies-bb5.txt",
"isi-replies.txt"]`; the later file wins per (resource, request)). The modem is QEMU's `syborg-modem`
device (`src/qemu-syborg/hw/arm/syborg_modem.c`, 2026-11-10; since 2026-11-12 a thin QEMU host around the
modem core `syborg_modem_core.c`, which the hosted EKA2 board runs too: `src/hosted/variant/hmodem.cpp`): `tools/device.py qemu-args` writes
`<build dir>/gen/isi-replies.tab` (an instance's own `isi-replies.tab`, its `[overrides]` may change the modem) from them at every launch, so a reply change needs only a
restart, no ROM rebuild. Unsolicited messages a phone waits for: `[modem].indications` (sent once per
subscribing channel).
Adding a simple modem answer = one `reply RES REQ RESP bytes...` line in the device file. Replies that
depend on the request (echoes, IMEI, light, MTC state, RAPIDO DSP timing, power off) are in
`syborg_modem_core.c`; per-device values come from the board config (`[modem].imei`, `power_off_delay_s`,
`sim_service_status`, `indications`). The guest keeps only the ISC/ISA channel driver
(`src/standins/_shared/iscpeer.inl`: IscApi controls, receive parking, delivery), which forwards to the
device's port (0xc0019000, IRQ 15; port 1 at 0xc001b000, IRQ 16; mapped by patches/qemu-baseport/0023). Each channel object (several clients
may open one channel number) has a handle; the port register holds handle << 8 | channel number, and
the modem keeps replies, subscriptions and indications per object.
The `ISCPEER:` serial log lines keep their format (`tools/isi.py log`).

## Phones: device instances (`tools/instance.py`)

The user's phones are device instances, `build/instances/<name>/` (instance.toml + drives, snapshots,
logs; docs/sydb.md "Which phone"): `tools/instance.py list|show|create|clone|rename|delete|set|check|
primary|adopt`. `python3 tools/sydb/sydb.py phone NAME` opens one. `$SYBORG_INSTANCE` (set by sydb)
makes `tools/device.py` load the instance's device with its `[overrides]`.

## Hosted EKA2: `tools/hosted/htest.py`, `tools/hosted/phone.py`

The EPL kernel as a Linux process in a QEMU aarch64 test VM (design: src/hosted/README.md; docs/hosted-eka2/). `htest.py [tests]` builds
and runs the hosted tests (`--rom DEVICE` for the phone-ROM ones, `--env NAME=VALUE`, numbers in hex:
`PROBE_MS=4e20` is 20 s); docs/hosted-eka2/tests.md lists them. `python3 tools/hosted/phone.py` boots the
N95 user side on it and shows the screen live in a window (ivshmem shared memory,
build/hosted/htest/phone.shm; serial log build/hosted/htest/phone.log); keys as in the sydb phone window
(arrows, Enter = OK, F5/F7 = soft keys, digits); Ctrl+T shows the VM's Linux `top` beside the phone (each Symbian
thread is a named Linux thread of the one process "symbian"); closing the window stops the VM.

## Interactive runs: `scripts/run-gui.sh`

`INSTANCE=NAME scripts/run-gui.sh` (default: the primary instance of `$SYBORG_DEVICE` / the default
device) runs an instance in QEMU's own window with the serial log and a QMP socket in
`build/runs/gui-<time>/` (link `gui-last`), so a hang can be analysed afterwards
(`tools/syb_report.py build/runs/gui-last/serial.log`). It uses the instance's drives and ROM
(`DRIVE_C=`, `DRIVE_E=`, `ROM=`, `RTC_OFFSET=` override them). The tools (`syb_run`, `syb_gdb`) start
QEMU as `-name sybtool` and kill only those, never an interactive session.

## Phone drives: `scripts/drive.sh`

An instance keeps C: (`drives/nvmemory.img`) and E: (`drives/nvmemory2.img`, the memory card) across
runs; the phone formats them on first boot. With the phone off:
```
INSTANCE=n95 scripts/drive.sh put e myapp.sisx /   # then boot, File manager -> Memory card -> open it
INSTANCE=n95 scripts/drive.sh ls c /data
INSTANCE=n95 scripts/drive.sh get e /Images/x.jpg .
```
(`sydb --instance NAME drive ...` does the same.) Needs `mtools`. Scripted runs (`syb_run`, `syb_gdb`)
use fresh per-run drives unless given `--keep-drives DIR`.

Clock: the phone's time is host time plus the offset in `drives/rtc-offset`, which QEMU rewrites
whenever the date/time is changed on the phone (survives restarts; delete the file to return to
host time). `-rtc base=...` on the QEMU command line shifts the host base.

## Device smoke test

`tests/device-smoke.sh [DEVICE-ID] [--soft] [--keep]` boots a device headless in a throw-away
instance (`smoke`, ephemeral, made anew each run; from the seed drives `build/seeds/<id>`) and checks: idle within 1.5x the profile's
a budget from `--budget S` / `SMOKE_BUDGET` (default 90 s: a property of the host, not of the phone), screen not black, Multimedia brings Carousel to the front without a
panic on the chosen GL backend, Switch off ends the session. Run it after every platform change
(docs/refactor-plan.md); ~40 s.

## Older helpers still useful

- `tools/qmp.py SOCK screendump|type|key|hmp ...` (QMP client), `scripts/run-modern.sh` (plain run).
- `tools/ghidra/decompile.sh NAME` (headless Ghidra, output `<build dir>/ghidra/NAME.c`).
- `tools/xipdis.py` (objdump with import names), `tools/research/xip_importers.py`.
- `tools/personality/kabi.py` (our kernel's driver-facing vtables vs the N95's 9.2 ekern, slot by slot;
  `--func NAME` disassembles an export from both).
- `tools/rofs.py`, `tools/e32load.py` (ROFS files, RAM-loaded images to ELF).
- `scripts/measure-boot.sh --qemu modern` (milestone timings, `build/logs/boot-times.tsv`).

## Native test apps: `src/n95apps/`

`src/n95apps/dsbtest/build.sh` builds `build/n95apps/dsbtest.exe` on the host (GCC 14 symbianelf +
elf2e32_next, S60 3rd FP1 SDK headers and 9.2 `.dso` only) and its menu registration
`dsbtest_reg.rsc` (rcomp in the container, see the build log of 2026-11-06). Install with QEMU off:
```
DRIVE_C=... scripts/drive.sh put c build/n95apps/dsbtest.exe /sys/bin
DRIVE_C=... scripts/drive.sh put c build/n95apps/obj-dsbtest/dsbtest_reg.rsc /private/10003a3f/import/apps
```
It appears in Menu → Applications. It measures `CDirectScreenBitmap` EndUpdate pacing and prints
`DSBTEST:` lines (fast counter, NTickCount, fps) to the serial log. No C++ EH runtime: it opens a
trap level with `User::MarkCleanupStack()` so cleanup-stack pushes work, but a leave panics.
Approach for app development and debugging in general: [app-dev-debugging.md](app-dev-debugging.md).
Planned LLM-driven debug bridge (MCP server, guest agent, kernel debug LDD): [devtools-bridge.md](devtools-bridge.md).

### `src/n95apps/wgprobe`: window-group probe

`src/n95apps/wgprobe/build.sh` builds `build/n95apps/wgprobe.exe` (no window group of its own).
Its `wgprobe_reg.rsc` is dsbtest's with UID3 and name patched (UID checksum recomputed; rcomp not
needed). Install like dsbtest, launch from Menu → Applications; it prints `WGPROBE: focus <id>
<name>` and the full window-group order (front first, ordinal priority, name = status|UID|caption)
every time focus or order changes, polled every 0.5 s for 10 minutes.

See also [sydb.md](sydb.md): the dev/debug bridge (session daemon + phone agent + CLI + MCP).

## Performance sampling

- `tools/pcsample_hmp.py SESSION SECONDS`: guest PC samples through QEMU's monitor (no gdb stop),
  attributed to ROM files and RAM-loaded code; flags the idle loop. Host side: `perf record -p
  <qemu_pid>`. Method and findings: docs/perf.md.
