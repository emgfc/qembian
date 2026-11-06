# Debugging tools (N95 hybrid on QEMU 11.1.3)

Quick reference for people and agents. All tools run on the host from the project root.

**Live phone first:** [sydb](sydb.md) (`tools/sydb/sydb.py`, MCP `mcp__sydb__*`) is the bridge to a
running phone: apps, files, processes, UI keys/screenshots, keyguard, P&S, IPC strace, persistent
gdb with symbols. Use it instead of ad-hoc QEMU/gdb runs; the tools below are for scripted boot
experiments, ROM work and offline analysis.

## Boot, observe, summarise: `tools/syb_run.py`

```
tools/syb_run.py --secs 30 --shot 5 --shot 15            # compose (IPC trace on), boot, report
tools/syb_run.py --trace 0x80000000 --secs 20            # quiet/fast trace mask
tools/syb_run.py --no-compose --key 20:ret --type 25:abc # reuse ROM, send keys/text at times
tools/syb_run.py --no-compose --keep-drives build/runs/D  # reuse C:/E:/RTC offset across runs
```
Writes `build/runs/<label>/` with `serial.log` (seconds-prefixed lines), `shot-*.png`,
`report.txt`. The report (`tools/syb_report.py LOG [--full] [--ipc-errors]`, works on any serial
log) lists: processes, exits/panics/kills, N95HACK and faults, caught kernel exceptions per thread,
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
N95 core ROM index in `build/n95/symindex.json` (rebuilt when `core.json` changes; `reindex` to
force). Addresses below the N95 user window resolve to **our** kernel-side files
(`build/n95/ours.json`, `hybrid-manifest.txt`) with `ekern` function names from its ELF symbols.
RAM-loaded ROFS code is not indexed (see `tools/e32load.py`).

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
Boots the ROM as it is (`build/n95/hybrid.img`) with the gdbstub, loads the kernel's DWARF
(`_syborg_ekern.exe.sym` at code 0x80003000, data 0xc8000000), so kernel expressions work; names
the current thread from `TheScheduler`; kills stray QEMUs first. Output also in
`build/runs/gdb-<cmd>.log`. Hardware breakpoints have no practical limit under TCG.

## Modem (ISI): `tools/isi.py` and `src/n95compat/isi/isi-db.txt`

```
tools/isi.py name 09 1d                 # PN_SIM/SIM_IMSI_REQ_READ_IMSI
tools/isi.py log build/runs/X/serial.log [--all]   # unanswered requests, or every message decoded
```
`isi-db.txt` holds resource/message names (with evidence tags) and the canned reply table;
`scripts/build-n95compat.sh` regenerates `src/n95compat/stubs/isireplies.h` from it. Adding a
simple modem answer = one `reply RES REQ RESP bytes...` line. Replies that depend on the request
(echoes, IMEI, light, MTC state) stay in `src/n95compat/stubs/iscpeer.inl`.

## Interactive runs: `scripts/run-gui.sh`

GUI window with the serial log and a QMP socket in `build/runs/gui-<time>/` (link `gui-last`), so a
hang can be analysed afterwards (`tools/syb_report.py build/runs/gui-last/serial.log`). Every run
gets its own C: drive image; the tools (`syb_run`, `syb_gdb`) start QEMU as `-name sybtool` and kill
only those, never an interactive session.

## Phone drives: `scripts/n95-drive.sh`

`scripts/run-gui.sh` keeps C: (`build/n95/drive-c.img`, 64 MB) and E: (`build/n95/drive-e.img`,
1 GB memory card) across runs; the phone formats them on first boot. With the emulator closed:
```
scripts/n95-drive.sh put e myapp.sisx /        # then boot, File manager -> Memory card -> open it
scripts/n95-drive.sh ls c /data
scripts/n95-drive.sh get e /Images/x.jpg .
```
Needs `mtools`. Scripted runs (`syb_run`, `syb_gdb`) use fresh per-run drives unless given
`--keep-drives DIR`.

Clock: the phone's time is host time plus the offset in `build/n95/rtc-offset`, which QEMU rewrites
whenever the date/time is changed on the phone (survives restarts; delete the file to return to
host time). `-rtc base=...` on the QEMU command line shifts the host base.

## Older helpers still useful

- `tools/qmp.py SOCK screendump|type|key|hmp ...` (QMP client), `scripts/run-modern.sh` (plain run).
- `tools/ghidra/decompile.sh NAME` (headless Ghidra, output `build/n95/ghidra/NAME.c`).
- `tools/xipdis.py` (objdump with import names), `tools/research/xip_importers.py`.
- `tools/kabi92.py` (our kernel's driver-facing vtables vs the N95's 9.2 ekern, slot by slot;
  `--func NAME` disassembles an export from both).
- `tools/rofs.py`, `tools/e32load.py` (ROFS files, RAM-loaded images to ELF).
- `scripts/measure-boot.sh --qemu modern` (milestone timings, `build/logs/boot-times.tsv`).

## Native test apps: `src/n95apps/`

`src/n95apps/dsbtest/build.sh` builds `build/n95apps/dsbtest.exe` on the host (GCC 14 symbianelf +
elf2e32_next, S60 3rd FP1 SDK headers and 9.2 `.dso` only) and its menu registration
`dsbtest_reg.rsc` (rcomp in the container, see the build log of 2026-11-06). Install with QEMU off:
```
DRIVE_C=... scripts/n95-drive.sh put c build/n95apps/dsbtest.exe /sys/bin
DRIVE_C=... scripts/n95-drive.sh put c build/n95apps/obj-dsbtest/dsbtest_reg.rsc /private/10003a3f/import/apps
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
