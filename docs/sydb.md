# sydb: dev/debug bridge for the virtual N95

adb-like bridge. Its main user is an LLM (through the MCP server); the CLI is the human interface.
Design and roadmap: [devtools-bridge.md](devtools-bridge.md). Status 2026-11-06: phase 0/1 work, and
phase 3's basic debugger works. Done: session daemon, agent (files, processes, apps, UI, keyguard,
P&S), CLI, MCP server (43 tools), live agent update, persistent gdb with kernel DWARF and app ELF
symbols placed from the kernel's `N95CODE:` line. IPC strace via the kernel's KIPC trace works. Not yet:
exec-call strace (phase-2 LDD), XIP
symbol ELFs for Nokia code in gdb backtraces, snapshots, SIS install.

## Pieces

| Piece | Where | What |
|---|---|---|
| daemon | `tools/sydb/daemon.py` | one per session dir (`build/sydb`). It starts QEMU (`run-modern.sh -name sydb`; syb_run/syb_gdb kill only `sybtool` QEMUs, so they don't collide with it) and serves JSON on `ctl.sock`. The session dir holds: serial0 → `serial.log` (timestamped), serial1 → `agent.sock`, QMP → `qmp.sock`, gdbstub → tcp port in `gdb.port`, and the phone drives in `drives/` |
| CLI | `tools/sydb/sydb.py` | `sydb <cmd>`; `--json` for raw replies; `--session DIR` for a second phone |
| MCP | `tools/sydb/mcp.py` | stdio JSON-RPC, 43 tools (`session_*`, `log_tail`, `events`, `fs_*`, `app_*`, `proc_*`, `ui_*`, `sys_*`, `dbg_*`, `qemu_hmp`); registered for Claude Code in `.mcp.json` |
| agent | `src/n95apps/devagent/devagent.cpp` | `devagent.exe`, All-TCB, serial unit 1 (`RBusDevComm`), line protocol (header of the file) |
| TCB helper | `src/n95apps/devagent/dafs.cpp` | `dafs.exe`, ALL caps, EUSER/EFSRV only. Moves files into `\sys\`/`\resource\`, and does the agent self-update |

Build the agent pair with `src/n95apps/devagent/build.sh` (`--debug` = `-O0 -g`).

## Quick start

```
src/n95apps/devagent/build.sh
tools/sydb/sydb.py start --seed build/runs/mtc-drives    # first time: copy a phone past first boot
tools/sydb/sydb.py start                                 # later: reuse build/sydb/drives
```
`start` (re)installs the agent when seeding (`sydb install` does it alone, session stopped). It
then waits for the idle screen and the agent (Starter starts it at about 21 s).

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

## Autostart: Starter's dynamic startup list

The agent starts the way an installed S60 app with a startup entry does. It is a record in the
N95 Starter's dynamic startup list, and Starter starts it at about 21 s (right after the global
state reaches 0xCB). Found with sydb itself: the IPC trace of a boot shows Starter reading
`c:\private\100059C9\start.dat`, and `Starter.exe` / `StarterClient.dll` /
`SWInstStartupTask.dll` give the rest:

- **Store:** `c:\private\100059C9\start.dat`, DES-ECB with a fixed 8-byte key from Starter. The
  last block is padded with `'0'`, which is stripped on reading. Plaintext records:
  `path;recovery;type;w2;base64(MD5(exe));}`. Starter backs it up via `start.tmp` and
  `bckupstart.dat` while rewriting.
- **API:** session to `StarterServer` (StarterClient ord9 = connect, version 0.0.0).
  - op 5 add, args {12-byte struct w0 recovery, w1 type, w2} + path (ord5/ord11);
  - op 6 remove, arg path (ord7);
  - op 8 path (ord3; the installer calls it too).

  Add computes the EXE's MD5 itself and replaces an older record for the same path. The
  installer's `SWInstStartupTask.dll` is the normal client.
- **At boot:** on state 0xCB, if FeatureManager feature 0x421 is supported, Starter starts each
  record of type 1, 7 or 8 whose EXE still has the recorded MD5 (recovery is forced to 0 on
  `e:`).

| Command | What |
|---|---|
| `sydb install` / `uninstall` (session stopped) | copy/delete the EXEs; write/remove the record in `start.dat` offline (`tools/sydb/startlist.py`, same format, byte-identical to what Starter writes) |
| `sydb startup add|rm|check PATH [w0 w1 w2]` (running) | the StarterServer requests, as the installer makes them (defaults 0 1 0) |
| `sydb startup list` | decrypted records |
| `sydb update-agent` | re-registers the new binary (its MD5 changed) |

## Catch, snapshots and forks (time travel)

`sydb catch` freezes or dumps the phone the moment a thread dies the way a rule says, and
`sydb snap`/`fork` save the whole machine and resume it later, any number of times, as separate
sessions. Built 2026-11-06 to hunt a timing-dependent Hitchcock crash: breakpoints and
watchpoints on hot paths changed the timing and hid it, while the catch breakpoints are on cold
paths and caught it in the first run.

```
sydb catch add --kind panic --thread Carousel --action freeze --snapshot   # rule 1
sydb catch add --category KERN-EXEC --reason 3                             # any thread, dump only
sydb catch list | hits | show ID | rm [ID] | resume
sydb snap save [NAME] [--stopped] | snap list | snap rm NAME
sydb fork catch-1 [--into DIR] [--run]     # resume the snapshot as build/sydb-fork-catch-1, paused
sydb start --fresh --paused                # QEMU stopped before the first instruction: arm catches/breaks, then sydb resume
sydb --session build/sydb-fork-catch-1 dbg attach    # ... the very moment, as often as needed
sydb restore NAME                          # the same into this session
sydb pause | resume
```

- **Triggers:** hardware breakpoints through the persistent gdb on `User::Panic(const TDesC16&, TInt)`
  (EUser 0x803a9f8a). That is user mode, before the panic exec call, so the thread is still alive
  with exact registers; `User::RaiseException` (USER-EXEC 3) and CBase/USER panics end there. The
  other breakpoint is `DThread::Die(TExitType, TInt, const TDesC8&)`, the kernel's single exit path,
  which also sees exits the kernel decides (KERN-EXEC 3 for exceptions, KERN-EXEC 0, kills by other
  threads). There, user registers come from the kernel's own `ThreadUserContextTables` (exact for
  exceptions; for exec calls r4-r11 and pc). A Die that follows a caught User::Panic is skipped.
- **Rules:** kind (panic, kill, terminate, exit = any abnormal exit, any), thread regex on
  `process::thread`, exact category, exact reason, action dump (continue) or freeze (hold; inspect
  with `dbg`, then `catch resume`), `--snapshot`, `--once`. A stop that matches no rule continues at
  once. Only thread deaths stop the machine, so normal running is unaffected.
- **Dump** (`build/sydb/catches/NNN-thread.{txt,json,stack.bin}`, event `catch.hit`): registers, `at`
  and `caller` symbolised, and a call-stack scan. The scan takes the words on the user stack
  whose preceding instruction is a BL/BLX/`mov lr,pc` and symbolises them against ROM modules (syb_sym)
  and RAM code segments (`N95CODE` lines), followed by the log tail.
- **Snapshots** (`<session>/snaps/NAME/`): QMP `migrate` to `state.bin` (RAM + all device state,
  about 120 MB, under 1 s) plus copies of the drive images (reflinks on btrfs: instant, shared blocks) and the RTC offset. QEMU runs with
  `migration.store-global-state=off` so snapshots taken while gdb holds the machine can be
  restored; after saving, the block devices are reactivated (`blockdev-set-active`) so the source
  runs on. **Forks** start a new daemon + QEMU with `-incoming file:… -S` on copies of the drives:
  paused, own gdb port, the agent answers after `resume`.
- **Retention** (`sydb gc`, MCP `session_gc`, and automatically after each catch hit): automatic
  snapshots (`catch-*`, `auto-*`) beyond the newest 5 and dumps beyond the newest 200 are deleted;
  `sydb gc` also removes fork session dirs whose daemon is not running. Snapshots named by hand stay
  until `snap rm`. A snapshot is about 120 MB of `state.bin`; the drive copies are reflinks.
- MCP: `catch_add/list/rm/hits/show/resume`, `snap_save/list/rm`, `session_fork`, `session_select`
  (point the tools at a fork and back), `vm_pause/resume`.

## Phone window

`sydb phone [--scale 2]` opens an emulator window like the Android emulator's: the live screen
(read straight from guest RAM, which sessions back with a shared file `/dev/shm/sydb-<hash>.ram`
(`status` → `ram`); the window follows the fb registers for base, pitch, rotation, partial band and
blanking, ~60 frames/s, pixel-identical to QEMU's screendump; `SYDB_VNC=1` or an older session falls
back to VNC on `<session>/vnc.sock`), the phone's keys with real press/release (hold Menu = task
switcher, hold Power = switch off), the host keyboard (its keys show pressed on the phone while held), and a side toolbar: Power, Vol +/-,
**Landscape (media slide)** (taps F13, the phone switches its layout, the view turns so it stays
upright; Ctrl+Left/Right toggles it too), **Keypad slide** (shows the number keys; no slide event to
the phone yet), Shot, Snapshot, Pause, Reset. **Multimedia** (F20), **Gallery** (F21) and the media
keys send the N95's own key codes. `sydb phone --device` (or `start --device`) runs on your
run-gui phone in place (build/n95/drive-c.img, drive-e.img, rtc-offset; ROM hybrid-exp.img, which
plays the startup jingle; session build/sydb-device; `--install` adds the sydb agent to its C:).
Restoring a snapshot in such a session writes the device images. It starts a session if none is running, with sound
on the host speakers (`--audio host`: `$AUDIO` or pipewire, like run-gui). `sydb start` keeps sound in
`<session>/audio.wav` unless given `--audio host` (or a QEMU audiodev driver). GLES runs on the host
GPU by default (`--gl soft` for the software rasteriser, H-gles-host). Details:
[phone-skin.md](phone-skin.md).

## TUI

`sydb tui` (curses) shows the status line (phone state, agent, focused window group), the named
window groups, live events, and the serial log tail (`:log REGEX` filters it). Its command line
takes any sydb command. Tab switches to key mode: arrows, Enter = OK, F5/F7/F8 and digits go to
the phone; Esc leaves. `q` quits.

## IPC strace

`sydb strace MATCH --secs 5` (MCP `trace_ipc` + `events kinds=["ipc"]`) switches the kernel's KIPC
debug bit on through the agent (`User::SetDebugMask`) and turns the kernel's `MsgAc`/`MsgCo`/`IPCRD`
lines into events. Only processes whose client or server name contains MATCH are kept:
```
  20.760 Calcsoft.exe::Calcsoft -> !FileServer  Entry
         efile.exe::Main read "C:\Resource\Uiklaf\eikcore.r01"
  20.761 Calcsoft.exe::Calcsoft <- efile.exe::DriveThread_02  fn 22 = -12
```
File-server function names come from F32's `OperationArray` (index = opcode) in our source; our
`efile.exe` is built from it. Other servers show numbers. All IPC in the system is printed while
the bit is on, which slows the phone a little; it is switched off when the strace ends.
Exec-call (system call) tracing per thread needs the phase-2 driver (devtools-bridge.md).

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
  `patches/kernelhwsrv/platform/0007-sydb-codeseg-load-trace.patch`). ROM XIP code is named by
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
- **Loader code-segment reuse:** the loader (`E32Image::CheckAlreadyLoaded`) reuses an existing
  code segment when root name, UIDs, module version and security info match, and does not look
  at the file. Code segments of exited processes can linger for a long time here (seen: a dead
  `dafs.exe` segment reused 40 s later), so an updated EXE with the same name can run the old
  code. `rerun`, `update-agent` and MCP `app_rerun` therefore stamp a new module version (minor
  = time-based) into the E32 header and fix the header CRC (CRC-32 from 0 over `iCodeOffset`
  bytes, with the CRC field set to 0xc90fdaa2 first). Plain `push` doesn't stamp.
- Thread suspend/resume is not possible from the agent: `RThread::Suspend` on another
  process's thread is a PlatSec process-isolation violation (KERN-EXEC 46 panics the caller,
  even with ALL caps). Use `dbg interrupt` for now, or the phase-2 driver later.
- Requests to the agent are serialized. While gdb has the CPU stopped, agent requests time out.
- Events come from the agent (`agent.hello`, `agent.exit` for processes started with run/launch)
  and from serial0 (`thread.panic`, `thread.killed` for any thread, `phone.idle`).
- Names and fields are latin-1; paths with spaces are fine (%XX escaping on the wire).
