# Debugging tools (N95 hybrid on QEMU 11.1.3)

Quick reference for people and agents. All tools run on the host from the project root.

## Boot, observe, summarise: `tools/syb_run.py`

```
tools/syb_run.py --secs 30 --shot 5 --shot 15            # compose (IPC trace on), boot, report
tools/syb_run.py --trace 0x80000000 --secs 20            # quiet/fast trace mask
tools/syb_run.py --no-compose --key 20:ret --type 25:abc # reuse ROM, send keys/text at times
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

## Older helpers still useful

- `tools/qmp.py SOCK screendump|type|key|hmp ...` (QMP client), `scripts/run-modern.sh` (plain run).
- `tools/ghidra/decompile.sh NAME` (headless Ghidra, output `build/n95/ghidra/NAME.c`).
- `tools/xipdis.py` (objdump with import names), `tools/research/xip_importers.py`.
- `tools/rofs.py`, `tools/e32load.py` (ROFS files, RAM-loaded images to ELF).
- `scripts/measure-boot.sh --qemu modern` (milestone timings, `build/logs/boot-times.tsv`).
