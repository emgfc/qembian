# sydb: dev/debug bridge for the virtual N95

adb-like bridge. Its main user is an LLM (through the MCP server); the CLI is the human interface.
Design and roadmap: [devtools-bridge.md](devtools-bridge.md). Status 2026-11-06: phase 0/1 work, and
phase 3's basic debugger works. Done: session daemon, agent (files, processes, apps, UI, keyguard,
P&S), CLI, MCP server (43 tools), live agent update, persistent gdb with kernel DWARF and app ELF
symbols placed from the kernel's `N95CODE:` line. Not yet: strace / IPC trace LDD (phase 2), XIP
symbol ELFs for Nokia code in gdb backtraces, snapshots, TUI.

## Pieces

| Piece | Where | What |
|---|---|---|
| daemon | `tools/sydb/daemon.py` | one per session dir (`build/sydb`). It starts QEMU (`run-modern.sh -name sydb`; syb_run/syb_gdb kill only `sybtool` QEMUs, so they don't collide with it) and serves JSON on `ctl.sock`. The session dir holds: serial0 → `serial.log` (timestamped), serial1 → `agent.sock`, QMP → `qmp.sock`, gdbstub → tcp port in `gdb.port`, and the phone drives in `drives/` |
| CLI | `tools/sydb/sydb.py` | `sydb <cmd>`; `--json` for raw replies; `--session DIR` for a second phone |
| MCP | `tools/sydb/mcp.py` | stdio JSON-RPC, 27 tools (`session_*`, `log_tail`, `events`, `fs_*`, `app_*`, `proc_*`, `ui_*`, `sys_*`, `qemu_hmp`); registered for Claude Code in `.mcp.json` |
| agent | `src/n95apps/devagent/devagent.cpp` | `devagent.exe`, All-TCB, serial unit 1 (`RBusDevComm`), line protocol (header of the file) |
| TCB helper | `src/n95apps/devagent/dafs.cpp` | `dafs.exe`, ALL caps, EUSER/EFSRV only. Moves files into `\sys\`/`\resource\`, does the boot autostart (as `HALSettings.exe`, hack H-sydb-autostart) and the agent self-update |

Build the agent pair with `src/n95apps/devagent/build.sh` (`--debug` = `-O0 -g`).

## Quick start

```
src/n95apps/devagent/build.sh
tools/sydb/sydb.py start --seed build/runs/mtc-drives    # first time: copy a phone past first boot
tools/sydb/sydb.py start                                 # later: reuse build/sydb/drives
```
`start` (re)installs the agent when seeding (`sydb install` does it alone, session stopped). It
then waits for the idle screen (about 20 s) and the agent (up at about 4 s).

```
sydb ps [PREFIX] | threads [PREFIX] | kill PID|NAME
sydb push LOCAL 'C:\sys\bin\x.exe' | pull REMOTE LOCAL | ls PATH | rm PATH | mkdir PATH
sydb run 'C:\sys\bin\x.exe' [ARGS] --wait 10          # -> {"pid", "exit": {type, reason, category}}
sydb rerun build/n95apps/x.exe [ARGS] --wait 10       # kill old, push to C:\sys\bin, run
sydb apps [FILTER] | launch Calculator | launch 0x10005902
sydb wg | fg WGID                                     # window groups, focus marked with *
sydb unlock | lock | keylock | keepawake on|unlocked|off
sydb prop get 101f8766 41 | prop set CAT KEY VALUE
sydb key menu down ok 1.5 back | type hello | screenshot [OUT] --scale 0.5 --changed
sydb log --grep 'Panic|DEVAGENT' --since 20 | events --kind agent.exit --wait 30
sydb update-agent                                     # rebuilt agent deployed without reboot
sydb hmp 'info registers' | status | stop
```
Key aliases: `lsk rsk ok menu star hash clear back` (F5, F7, F6, F8, numpad *, F10, backspace, F7).

## Debugger

One persistent gdb (MI, `tools/sydb/debugger.py`) on the session's gdbstub port. It runs
all-stop, so a stop freezes the whole phone; agent requests time out until `continue`/`detach`.
```
sydb dbg attach                          # stops the phone; stop record with kernel frames + thread
sydb run 'C:\sys\bin\app.exe'           # (before or after attach)
sydb dbg symbols app [build/n95apps/obj-app/app.elf]   # ELF at the run address from N95CODE
sydb dbg break Name --thread app         # hw breakpoint; other threads hitting it are resumed
sydb dbg continue --wait 20              # -> {"reason":"breakpoint-hit","at":"Name(...)","thread":...}
sydb dbg bt | regs | mem 0x406ef4 32 | eval 'TheScheduler.iCurrentThread->iPriority'
sydb dbg step | next | stepi | finish | interrupt | delete [N] | detach | cmd 'info breakpoints'
```
- `N95CODE: <file> code=<run>+<size> data=... bss=... ep=...` is printed by our kernel for every
  code segment loaded into RAM (C: apps and ROFS binaries; kernel patch
  `patches/kernelhwsrv/0007-sydb-codeseg-load-trace.patch`). ROM XIP code is named by
  `tools/syb_sym.py` in stop records.
- Kernel DWARF placement comes from `tools/ksyms.py` (computed from the ELF; syb_gdb uses it too,
  because the old hardcoded `.rodata` address had gone stale).
- Build apps with `-O0 -g` for source-level stepping (`src/n95apps/devagent/build.sh --debug`
  shows how).

## Measured (2026-11-06)
- push/pull: 300 KB in 2.1 s each way (2 KB acked chunks; without flow control the comm
  driver's receive buffer overflowed and the transfer stalled), CRC32 checked.
- `run` → exit event: dafs.exe with a bad argument exits `kill -6` within a second.
- 128 apps listed; `launch Calculator` starts it in the foreground.

## Notes
- Requests to the agent are serialized. While gdb has the CPU stopped, agent requests time out.
- Events come from the agent (`agent.hello`, `agent.exit` for processes started with run/launch)
  and from serial0 (`thread.panic`, `thread.killed` for any thread, `phone.idle`).
- Names and fields are latin-1; paths with spaces are fine (%XX escaping on the wire).
