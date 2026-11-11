# sydb: qembian's dev/debug bridge to the virtual phone

adb-like bridge. Its main user is an LLM (through the MCP server); the CLI is the human interface.
Design and roadmap: [devtools-bridge.md](devtools-bridge.md). Status 2026-11-06: phase 0/1 work, and
phase 3's basic debugger works. Done: session daemon, agent (files, processes, apps, UI, keyguard,
P&S), CLI, MCP server (43 tools), live agent update, persistent gdb with kernel DWARF and app ELF
symbols placed from the kernel's `SYBCODE:` line. IPC strace via the kernel's KIPC trace works. Not yet:
exec-call strace (phase-2 LDD), XIP
symbol ELFs for Nokia code in gdb backtraces, snapshots. SIS install: `sydb sis install` (below).

## Pieces

| Piece | Where | What |
|---|---|---|
| daemon | `tools/sydb/daemon.py` | one per session dir (an instance's directory, or any `--session DIR`). It starts QEMU (`run-modern.sh -name sydb`; syb_run/syb_gdb kill only `sybtool` QEMUs, so they don't collide with it) and serves JSON on `ctl.sock`. The session dir holds: serial0 → `serial.log` (timestamped), serial1 → `agent.sock`, QMP → `qmp.sock`, gdbstub → tcp port in `gdb.port`, and the phone drives in `drives/` |
| CLI | `tools/sydb/sydb.py` | `sydb <cmd>`; `--json` for raw replies; `--instance NAME` or `--session DIR` picks the phone (below) |
| instances | `tools/instance.py` | the user's phones: create, clone, rename, delete, set, list (below) |
| MCP | `tools/sydb/mcp.py` | stdio JSON-RPC, 43 tools (`session_*`, `log_tail`, `events`, `fs_*`, `app_*`, `proc_*`, `ui_*`, `sys_*`, `dbg_*`, `qemu_hmp`); registered for Claude Code in `.mcp.json` |
| agent | `src/n95apps/devagent/devagent.cpp` | `devagent.exe`, All-TCB, serial unit 1 (`RBusDevComm`), line protocol (header of the file) |
| TCB helper | `src/n95apps/devagent/dafs.cpp` | `dafs.exe`, ALL caps, EUSER/EFSRV only. Moves files into `\sys\`/`\resource\`, and does the agent self-update |

Build the agent pair with `src/n95apps/devagent/build.sh` (`--debug` = `-O0 -g`).

## Which phone: instances and sessions

A **session** is a directory the daemon runs a phone in: `drives/` (C: `nvmemory.img`, E: `nvmemory2.img`,
`rtc-offset`), snapshots, catches, logs, sockets. A **device instance** is a session directory with an
`instance.toml` (device model v2, [device-model-v2.md](device-model-v2.md) 2.3): one named virtual phone
with a model, one of the model's firmwares, a ROM variant, settings (`gl`, `audio`, `scale`: the
defaults of `sydb phone` / `start` for it) and optional `[overrides]` of run-time profile values
(`board`, `gl.host`, `modem`, `[[overrides.standins]]` config; anything composed into the ROM is
refused). Instances live in `build/instances/<name>/` (`$QEMBIAN_HOME/instances` when set); instances
of one model and firmware share one composed ROM.

```
tools/instance.py list                                   # your phones (primary, running)
tools/instance.py create n8-test nokia-n8-rm596@025.007  # a model + firmware (or a firmware's id: nokia-n8-rm596-v25)
tools/instance.py clone n95 n95-copy                     # config + drives (sparse/reflink copies), no snapshots
tools/instance.py set n95 settings.audio wav | set NAME rom_variant audio-exp | set NAME firmware FW --keep-drives|--wipe-drives
tools/instance.py rename OLD NEW | delete NAME --yes | check | show NAME | get NAME rom|dir|device|drives
```

A sydb command picks its phone, in this order: `--instance NAME`; `sydb phone NAME`; `--session DIR`
(when DIR has an `instance.toml` it is that instance); else the **primary** instance of
`$SYBORG_DEVICE` (default: `models/registry.toml` `default`, the N95), made as
`build/instances/<device id>` at its first start. One instance per device is primary
(`instance.py set NAME primary true`), so `SYBORG_DEVICE=ID sydb ...` keeps opening the same phone.
An instance sets `SYBORG_DEVICE` and `SYBORG_INSTANCE` for everything it starts, so `tools/device.py`
in the daemon, `run-modern.sh` and the other tools load its device with its overrides. A session
refuses another phone's drives (the `device` mark file); `--fresh` wipes them.

Agents and tests: a throw-away session (`--session build/sydb-<name>`) or an ephemeral instance
(`instance.py create NAME DEVICE --ephemeral --replace`, what `tests/device-smoke.sh` does), never
the user's instances (`instance.py list`: the ones not marked ephemeral).

## Quick start

```
src/n95apps/devagent/build.sh
tools/sydb/sydb.py --session build/sydb-x start --seed build/seeds/nokia-n95-8gb-rm320   # a phone past first boot
tools/sydb/sydb.py --session build/sydb-x start          # later: reuse <session>/drives
```
`start` (re)installs the agent when seeding (`sydb install` does it alone, session stopped). It
then waits for the idle screen and the agent (the boot hook starts it about 1 s into the boot).

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

## Autostart: the EStart hook

Nokia's EStart starts a few programs by bare name right after mounting the drives (`HALSettings.exe
INITIALISE` on 9.2 and later, `LanguageSelection.exe` on 9.1), and the loader searches C: before Z:.
`sydb install` puts `boothook.exe` on C: under that name (read from the firmware's EStart): it starts
`c:\sys\bin\devagent.exe` (unless one is already running), then runs the real `z:\sys\bin\<name>`
with the same command line and returns its exit value. The hook carries EStart's SID (0x10272C04)
because the real HALSettings accepts INITIALISE only from EStart. The agent comes up about 1 s into the boot, before the window server; commands that
need a server that is not up yet return its connect error.

One mechanism on every phone, deliberately not configurable. Why Starter's dynamic startup list
(used before) was dropped, and the alternatives: docs/agent-autostart.md.

| Command | What |
|---|---|
| `sydb install` / `uninstall` (session stopped) | copy/delete `devagent.exe`, `dafs.exe` and the hook (`HALSettings.exe` / `LanguageSelection.exe`) on C:; `install` also removes an older install's agent record from Starter's `start.dat` |
| `sydb update-agent` | replaces the agent (and `dafs.exe`, the hook) on a running phone |
| `sydb startup add|rm|check|list PATH [w0 w1 w2]` (running, 9.1-9.3) | Starter's dynamic startup list (`c:\private\100059C9\start.dat`, DES-ECB records `path;recovery;type;w2;base64(MD5(exe));}`, `tools/sydb/startlist.py`) through the StarterServer requests an installer makes; kept for tests of installed-app start-up, not used by the agent |

## Installing .sis / .sisx: `sydb sis install`

```
sydb sis install game.sisx [--drive E] [--lang N] [--keep] [--timeout S]
```
Copies `build/n95apps/sisinst.exe` to `C:\sys\bin`, the package to `E:\sydb\` (`C:\data\sydb\` without
an E:), runs it and waits. `sisinst.exe` (`src/n95apps/devagent/sisinst.cpp`, All-TCB) calls the native
installer API `Swi::Launcher::Install` (sishelper.dll) with its own UI handler instead of the phone's
installer UI: every question the install server asks comes to it, is logged with its parameters and
answered yes:

| Question | Answer |
|---|---|
| install?, upgrade?, uninstall? | yes |
| capabilities to grant | yes (the list is logged) |
| language | `--lang` if offered, else English, else the first |
| drive | `--drive` if it has room, else the drive with the most free space (sizes logged) |
| options | all selected |
| text (licence, notes) | logged in full (200-character pieces), continue |
| security warning (signature result), OCSP result | yes (result logged; a package the install server refuses stays refused) |
| question: incompatible package, overwrite a file | yes |
| missing dependency, dependency break, applications in use | continue / close them |
| errors (cannot overwrite, capabilities cannot be granted, ...) | logged |

Large packages: copy them into the stopped session's drive image (disk to disk, no serial link),
then install them in place on the phone:
```
sydb stop
sydb drive put E ~/games/ 'E:\installs'      # files of the directory; creates E:\installs
sydb start            # or: sydb phone --install
sydb sis install 'E:\installs\'               # every .sis/.sisx in it, a summary at the end
```
`sydb drive ls|put|get|mkdir|rm LETTER ...` maps the letter to the session's image through the
profile's `[[board.drives]]` (N8: E: = `nvmemory2.img`, 1 GB) and refuses while the session runs. Items of
`sis install` may be local files or directories (copied over the agent link, deleted after unless
`--keep`), phone files or phone directories (installed in place, left there).

The log is printed, kept as `<session>/sis/NAME.log` and in the serial log (`SISINST:` lines, also when
the helper dies half-way). Exit 0 = installed; otherwise the result is named (-10254 security error,
-10149 no space, -46 permission denied, -3 a step was refused). Interface: Symbian Foundation appinstall
(EPL) `msisuihandlers.h`/`launcher.h` (`~/work/symbian/oss.FCL.sf.mw.appinstall`), import library from
`src/n95apps/devagent/sishelper.def` (the legacy, pre-USIF ordinals). Tested on the N8 Delight (Belle
Refresh, whose install server is patched to accept any signature): a self-signed test package and an S60
3rd Edition example (which raised the "incompatible" question); the app runs. Not yet tried on 9.2-9.4
phones (their sishelper.dll has 85 exports; ordinals 3 and 16 are expected to be the same), and a stock
install server still refuses packages without a trusted signature (result -10254).

All our on-phone programs (`src/n95apps/devagent`) construct drtaeabi's `TCppRTExceptionsGlobals` in
their thread entry, as the SDK's EXE startup does: without it the first leave in any thread of the
process died in `__cxa_allocate_exception` (seen in the installer's `!UISS` thread).

## Host GPU settings

`sydb gl` prints the host GPU device's settings and statistics (kicks and time in the device,
compositions, asynchronous read-backs, fills, live surfaces and textures, GL errors); `sydb gl set
msaa=0|2|4|8|16 filter=auto|nearest|linear compose=host|guest [--save]` changes them while the phone
runs. Defaults come from the profile's `[gl.host]` (docs/devices.md). `SYBORG_GL_STATS=1` in the
environment of `sydb start` prints the timings to `qemu.err` every 5 s.

## Catch, snapshots and forks (time travel)

`sydb catch` freezes or dumps the phone the moment a thread dies the way a rule says, and
`sydb snap`/`fork` save the whole machine and resume it later, any number of times, as separate
sessions. Built 2026-11-06 to hunt a timing-dependent Hitchcock crash: breakpoints and
watchpoints on hot paths changed the timing and hid it, while the catch breakpoints are on cold
paths and caught it in the first run.

Limitation (2026-11-07): the host GPU device (syborg-gl) saves nothing into a snapshot. Devices
whose screen is drawn on the host (the N8: OpenVG through src/khr, composition through gcebe) come
back from `snap restore` / `fork` with a black screen and silently dropped draws, because the guest
still refers to host surfaces and textures that no longer exist. Fix designed in
docs/gpu-host-review.md 1.1 (save the objects and their pixels, or an epoch register the guest
checks); not built while nobody uses snapshots on the N8. Devices without host drawing are fine.

```
sydb catch add --kind panic --thread Carousel --action freeze --snapshot   # rule 1
sydb catch add --category KERN-EXEC --reason 3                             # any thread, dump only
sydb catch add --at 0x8252d4a8 --max 2 --action freeze  # break rule: a cold kernel/user address (dump each time)
sydb catch list | hits | show ID | rm [ID] | resume
sydb snap save [NAME] [--stopped] | snap list | snap rm NAME
sydb fork catch-1 [--into DIR] [--run]     # resume the snapshot as build/sydb-fork-catch-1, paused
sydb start --fresh --paused                # QEMU stopped before the first instruction: arm catches/breaks, then sydb resume
sydb start --fresh --catch-panics         # the same with a freeze+snapshot catch on the first panic, resumed; prints the hit on timeout
sydb --session build/sydb-fork-catch-1 dbg attach    # ... the very moment, as often as needed
sydb restore NAME                          # the same into this session
sydb pause | resume
```

- **Triggers:** hardware breakpoints through the persistent gdb on `User::Panic(const TDesC16&, TInt)`
  (EUser ordinal 650, its address taken from the device's syb_sym index: N95 0x803a9f8a). That is user mode, before the panic exec call, so the thread is still alive
  with exact registers; `User::RaiseException` (USER-EXEC 3) and CBase/USER panics end there. The
  other breakpoint is `DThread::Die(TExitType, TInt, const TDesC8&)`, the kernel's single exit path,
  which also sees exits the kernel decides (KERN-EXEC 3 for exceptions, KERN-EXEC 0, kills by other
  threads). There, user registers come from the kernel's own `ThreadUserContextTables` (exact for
  exceptions; for exec calls r4-r11 and pc). A Die that follows a caught User::Panic is skipped.
- **Paused starts:** gdb puts its breakpoints into the target only when *it* resumes it, so
  `sydb resume` continues through gdb whenever gdb is attached and holds the machine (a QMP `cont`
  behind its back would run with no breakpoints and catch nothing). A `--at` rule is inserted in
  the same stop as the panic/die breakpoints.
- **Break-rule registers** are the kernel's at that address: an argument already moved off `r0`
  is in the frame (`fp`/`sp`), not in the dump's `r0`.
- **Rules:** kind (panic, kill, terminate, exit = any abnormal exit, any), thread regex on
  `process::thread`, exact category, exact reason, action dump (continue) or freeze (hold; inspect
  with `dbg`, then `catch resume`), `--snapshot`, `--once`. A stop that matches no rule continues at
  once. Only thread deaths stop the machine, so normal running is unaffected.
- **Dump** (`<session>/catches/NNN-thread.{txt,json,stack.bin}`, event `catch.hit`): registers, `at`
  and `caller` symbolised, and a call-stack scan. The scan takes the words on the user stack
  whose preceding instruction is a BL/BLX/`mov lr,pc` and symbolises them against ROM modules (syb_sym)
  and RAM code segments (`SYBCODE` lines), followed by the log tail.
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

`sydb phone NAME [--fresh] [--scale S] [--keep]` boots instance NAME (`SYBORG_DEVICE=ID sydb phone`:
the device's primary instance; `--fresh` = empty drives, a factory-fresh phone) and opens its window
in one command; the session ends when the window closes, and also when
the window's process dies any other way (the daemon watches it; QEMU dies with the daemon), unless
`--keep`. Scale, sound and GLES default to the instance's settings (scale 1, sound on the host
speakers, GLES on the host GPU); `--scale F`, `--audio`, `--gl` override them. The window is an emulator window like the Android emulator's: the live screen
(read straight from guest RAM, which sessions back with a shared file `/dev/shm/sydb-<hash>.ram`
(`status` → `ram`); the window follows the fb registers for base, pitch, rotation, partial band and
blanking, ~60 frames/s, pixel-identical to QEMU's screendump; sessions serve no VNC), the phone's keys with real press/release (hold Menu = task
switcher, hold Power = switch off), the host keyboard (its keys show pressed on the phone while held), and a side toolbar: Power, Vol +/-,
**Landscape (media slide)** (taps F13, the phone switches its layout, the view turns so it stays
upright; Ctrl+Left/Right toggles it too), **Keypad slide** (shows the number keys; no slide event to
the phone yet), Shot, Snapshot, Pause, Reset. **Multimedia** (F20), **Gallery** (F21) and the media
keys send the N95's own key codes. The N95 that `scripts/run-gui.sh` and the old `sydb --device`
ran is the instance `n95-gui` (ROM variant `audio-exp`, which plays the startup jingle; `--install`
adds the sydb agent to its C:). `sydb phone` starts a session if none is running, with sound on the
host speakers (`--audio host`: `$AUDIO` or pipewire). `sydb start` keeps sound in
`<session>/audio.wav` unless the instance's `settings.audio` or `--audio` says otherwise. GLES runs on the host
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
sydb dbg symbols app [build/n95apps/obj-app/app.elf]   # ELF at the run address from SYBCODE
sydb dbg break Name --thread app         # hw breakpoint; other threads hitting it are resumed
sydb dbg continue --wait 20              # -> {"reason":"breakpoint-hit","at":"Name(...)","thread":...}
sydb dbg bt | regs | mem 0x406ef4 32 | eval 'TheScheduler.iCurrentThread->iPriority'
sydb dbg step | next | stepi | finish | interrupt | delete [N] | detach | cmd 'info breakpoints'
```
- `SYBCODE: <file> code=<run>+<size> data=... bss=... ep=...` is printed by our kernel for every
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
