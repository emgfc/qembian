# Devtools bridge: an LLM-driven debug/dev bridge for the virtual N95

Design note (2026-11-06). **Implemented as sydb, see [sydb.md](sydb.md)** (phases 0/1, the IPC
part of phase 2 via the kernel's KIPC trace, the basic phase-3 debugger, TUI). The autostart
ended up as hack H-sydb-autostart. Builds on [app-dev-debugging.md](app-dev-debugging.md) (which settled
"no AppTRK, gdbstub + guest launcher + JSON wrapper"); this note turns that into a complete bridge
whose main user is an LLM (MCP server), with a human TUI as an extra. Tags: [verified] = seen in
this repo / the firmware / the kernel sources, [inferred] = best guess, not checked.

## 1. Architecture in one picture

```
 host ──────────────────────────────────────────────────────────────────────────────────────
  tools/devtools/            one Python process = session + MCP server (stdio) + CLI (JSON)
   ├─ session   : boots/keeps one QEMU (--keep-drives), owns its run dir, event bus
   ├─ serial0   : tail of the kernel debug port (RDebug, SYBPROP/SYBSTUB/SYBCODE/STRACE lines),
   │              regex "wait for" events, syb_report sections on demand
   ├─ agentlink : line protocol to the guest agent over serial1 (chardev unix socket)
   ├─ qmp       : keys, screenshots, HMP `x`/`xp` live memory reads, savevm/loadvm
   ├─ gdb       : ONE persistent gdb (python API), kernel DWARF + app DWARF + XIP symbol ELFs
   └─ symbols   : syb_sym (ROM XIP), app ELF ↔ run address (SYBCODE line / LDD codeseg list)
         │ serial0 (file/pipe)   │ serial1 (unix sock)   │ QMP sock   │ gdbstub :1234
 QEMU ───┼──────────────────────┼───────────────────────┼────────────┼─────────────────────
  syborg board: serial0..3 at c0006000..c0009000 (all four have chardevs [verified syborg.c])
 guest ──┼──────────────────────┼───────────────────────┼────────────┼─────────────────────
   kernel (ours): Kern::Printf ──┘        edevtools.ldd (phase 2): strace table swap, IPC filter,
                                          codeseg/process/thread lists, exit capture, ring buffer
   user : devagent.exe (ours, ALL caps, XIP in ROM or C:\sys\bin) ── RBusDevComm unit 1 ──┘
          put/get files, run/kill, ps, apps list/launch, WSERV window groups, keyguard,
          inactivity reset, P&S read/write, RProcess::Logon exit events
```

Three transports, each for what it is best at; none of them new QEMU code:

| Channel | Used for | Why |
|---|---|---|
| **serial1** (`-serial unix:...devtools.sock,server,nowait`) | agent control + file transfer | exists in the board and the BSP already (UART PDD units 0–3 [verified `syborg_serial.cpp` DoCreate]); `RBusDevComm` on `ECOMM.LDD`/`EUART1.PDD` unit 1 needs no C32 and no kernel code [inferred: d32comm.h is in the SDK, the LDD/PDD are in our kernel side per architecture.md]; portable: on a real phone the same agent talks over RFCOMM/TCP instead |
| **QMP** (exists) | keys, screenshots, live memory reads (`human-monitor-command x/xp`), snapshots | no guest involvement; `x` reads *virtual* memory of the current CPU context, kernel data (`0xc8000000`+) and XIP ROM are global in the multiple model, so kernel structures can be walked **without stopping the VM** [inferred for `x` on a running VM; mapping verified] |
| **gdbstub** (exists) | breakpoints, step, registers, memory of the stopped thread | already works with kernel DWARF (`syb_gdb.py`) [verified] |

Rejected transports: semihosting (QEMU has it, but it is CPU-trapping, slow per call and ties the
guest to QEMU), a custom MMIO "debug channel" device (nice later for DMA bulk transfer and
host-initiated interrupts, but serial1 + QMP cover everything in phases 0–3; keep as an option if
file transfer over the 64-byte UART FIFO turns out too slow — [inferred] 0.5–2 MB/s under TCG),
shared memory (needs the custom device anyway), hostfs (the old baseport had one at `c000a000`;
the modern board has no such device [verified: `src/qemu-syborg/hw/arm`]; porting it plus an
ESTART drive entry is more work than the agent's `put`).

## 2. Capability table

| Capability | Mechanism | Effort | Depends on |
|---|---|---|---|
| Boot / keep a session, wait for "idle" | session manager around `run-modern.sh --keep-drives`, serial0 tailer; "idle" = `SYBPROP` startup state line (`101f8766/41`) [verified n95-startup-animation.md] | 0.5 d | nothing |
| Put EXE on the phone while it runs | agent `put PATH SIZE` + raw bytes → `RFile` on `C:\sys\bin` (agent has TCB: `--capability=all`; the loader does not check signatures [inferred, app-dev-debugging §3]) | in agent v1 | agent |
| Run / rerun / kill an EXE | agent `run PATH [ARGS]` = `RProcess::Create` + `Logon` + `Resume`; `kill PID` = `RProcess::Kill` (PowerMgmt cap); rerun = kill (if alive) + put + run | in agent v1 | agent |
| Exit / panic of a started process as an event | `RProcess::Logon` → `exit pid type reason category`; for processes not started by us: kernel `Thread %O Panic/Killed` lines are already on serial0 under KPANIC [verified sthread.cpp:711] | in agent v1 | agent, serial0 |
| List installed apps, launch any | `RApaLsSession::GetAllApps/GetNextApp` (uid, caption, full path) and `StartApp` / `TApaTask::BringToForeground` (`apgcli.h`, `apgtask.h` in the SDK [verified present]) | 0.5 d | agent |
| List processes / threads (user view) | agent `TFindProcess`/`TFindThread` + `RProcess::Open` → id, name, exit status | 0.5 d | agent |
| List processes / threads (kernel view: state, wait object, priority, stacks) | **phase 0:** host walks `K::Containers[EProcess/EThread]` over QMP `x` with struct offsets dumped once from the kernel DWARF [verified `sglobals.cpp:61`, offsets via gdb on `_syborg_ekern.exe.sym`]; **phase 2:** LDD `ps`/`threads` controls (like rm_debug's `GetList`) | 1 d / 1 d | QMP / LDD |
| Attach / stop / continue a thread | gdb: break-anywhere + `--when-thread` style filter in a persistent gdb (`TheScheduler.iCurrentThread`) [verified syb_gdb.py]; "stop this thread" = `RThread::Suspend` from the agent (cheap, no gdb) | 1 d | gdb, agent |
| Breakpoints, single step, registers, memory | persistent gdb (python API, MI not needed); hw breakpoints are address-based and unlimited under TCG [verified tools.md]; step in user mode with a thread filter (re-step while the current thread differs) | 2 d | gdb |
| Debug symbols for our apps | build `-O0 -g` variant (build.sh is `-O2` today [verified]); keep `obj-*/X.elf`; run address from `SYBCODE:` trace line (new kernel patch, one `Kern::Printf` in `DCodeSeg::Create` [verified scodeseg.cpp:729 prints it under KDLL]) or from the LDD codeseg list; `add-symbol-file X.elf -s .text run` | 0.5 d + 0.5 d | kernel patch / LDD |
| Symbols for Nokia XIP code | `syb_sym` describes any address today [verified]; for gdb backtraces generate per-module symbol ELFs with `tools/xip2elf.py` [verified exists; use inferred] and `add-symbol-file` on demand for the modules on the stack | 1 d | syb_sym |
| strace (exec calls) per thread | **table swap:** each thread's `iNThread.iSlowExecTable/iFastExecTable` pointer selects its table [verified sexecpers.cpp:29]; build a third ("traced") 9.2 table at init whose entries point to generated wrappers that log `S0a ChannelRequest(a0..a3)` + result into a kernel ring buffer, then call the real handler; `strace on PID` = swap the pointers of that process's threads (LDD control). Zero cost for untraced threads. Wrappers keep up to 7 args (extra-arg execs push r4–r6 [inferred]) | 2 d | LDD, kernel patch |
| strace (IPC messages) per process | kernel `KIPC` prints `MsgAc: M:fn client->server`, `MsgCo` [verified sipc.cpp:793, syb_report regexes]; add a per-process flag check so only traced processes print (or go to the ring buffer); opcode names per server = optional host table | 0.5 d | kernel patch |
| RDebug capture per process | already on serial0 [verified n95-logging.md] but untagged; kernel patch: prefix user `DebugPrint` with the thread name under a switch, or route through the LDD ring buffer with pid | 0.5 d | kernel patch |
| Send keys, type text | QMP `send-key` (exists) [verified] | 0 | QMP |
| Screenshot | QMP `screendump` (exists); add crop/downscale/`changed-since` (hash) so the LLM pays for pixels only when they changed | 0.5 d | QMP |
| "Screen updated" event | DisplayDriver stand-in ctl 9 / DSB post already traced (`SYBSTUB:` lines) [verified tooling-ideas] → event `screen.updated` | 0.2 d | serial0 |
| UI state: focused window group, window list | agent = `wgprobe` logic on request (`GetFocusWindowGroup`, `WindowGroupList`, names) [verified src/n95apps/wgprobe] | 0.2 d | agent |
| Keyguard unlock / keep unlocked | agent: `RAknKeyLock::DisableWithoutNote()` (`aknkeylock.h` in the SDK [verified present]; behaviour [inferred]); fallback: the known key chord F5+F9 via QMP [verified n95-controls.md] | 0.2 d | agent |
| No screensaver / no autolock | agent: `User::ResetInactivityTime()` every 10 s (public API, sure to work) [inferred effect on Screensaver/Autolock]; plus `RProperty` writes to the screensaver/keyguard P&S keys if the internal headers are found | 0.1 d | agent |
| Backlight on | `CHWRMLight::LightOnL` (`hwrmlight.h` in the SDK [verified present]) — note our light driver is a stand-in, the fb never blanks (H-lcd-alwayson) so this only matters for SysAp's state | 0.1 d | agent |
| P&S / CenRep read/write | agent `pget/pset CAT KEY` (`RProperty`), `crget` (`CRepository`, `centralrepository.h`) | 0.3 d | agent |
| Files: ls/get/put/rm anywhere | agent (`RFs`, TCB) — replaces `drive.sh` while the phone runs | in agent v1 | agent |
| Rerun without reboot | agent kill+put+run (seconds); **instant reset**: QMP `savevm`/`loadvm` of a post-boot state needs qcow2 drives (`-drive format=qcow2`); today's drives are raw [verified run-modern.sh] | 0.5 d | QMP, drive format |
| Install SIS/SISX (later) | a patched `installserver.exe` on C: + agent calling the SW installer launcher API (`SwiUI::RSWInstSilentLauncher`, header not in the public SDK [verified: not found in the SDK]) — or just `put` the files the SIS would install | 2–4 d | install server |
| Human TUI | `textual` app on the same session: screen pane (PNG), filtered serial pane, ps pane, command line with the MCP verbs | 2 d | everything above |

## 3. Transport and on-target design decisions

**Transport: serial1 now, custom MMIO device only if needed.** Add `-serial unix:RUN/devtools.sock,
server,nowait` as the second `-serial` in `run-modern.sh` (serial0 stays the kernel debug port).
Guest side: `RBusDevComm` unit 1 (`User::LoadLogicalDevice("ECOMM")`, `LoadPhysicalDevice
("EUART1")`, `Open(1)`); the UART PDD serves unit 1 at `c0007000` IRQ 6 [verified]. Framing: text
lines for control, `put NAME SIZE` followed by SIZE raw bytes (with a CRC32 trailer). Keep the
agent's transport behind one class (`MLink`) so the real-phone build swaps in an `RSocket` (Wi-Fi)
or RFCOMM link.

**On-target: hybrid — a user agent for everything user-side, a small LDD for everything the user
side cannot see, the host for anything that needs the CPU stopped.**
- The agent (`src/n95apps/devagent`, built like `wgprobe`, `--capability=all`) is the portable part:
  file transfer, process control, app list/launch, WSERV, keyguard, P&S. Autostart: S60's Startup
  List Management — a `STARTUP_ITEM_INFO` resource (`startupitem.rh` in the SDK [verified]) in
  `C:\private\101f875a\import\[UID].rsc`; the ROFS has that directory with one entry
  (`[A000028F].rsc`) [verified rofs1-tree.txt], so Starter does read it [inferred]. Place the
  `.rsc` with `drive.sh` once (or compose into the ROM). Fallback if the import list is
  ignored: the LDD creates the process kernel-side the way `EXSTART` creates `efile.exe`
  (`Kern::ProcessCreate` on an XIP exe [verified exstart.cpp]); then the agent must be in our ROM
  region (`rom/` iby entry) instead of on C:.
- The LDD (`src/standins/edevtools/edevtools.cpp`, same `kern_ext.mmh` build as the stand-ins
  [verified lightldd.mmp]) is the non-portable part: strace table swap, codeseg list
  (`DCodeSeg` run addresses), kernel process/thread/server/session lists, ring buffer for
  strace/IPC/RDebug records, thread exit hook (`DKernelEventHandler`, `EEventKillThread` [inferred:
  standard EKA2 API]). The agent reads the ring buffer and forwards it; the host never needs gdb
  for tracing.
- rm_debug (`kernel/eka/drivers/debug/rmdebug`, ~11 k lines [verified]) is the Symbian^3 run-mode
  debug driver and would give breakpoints/stepping/process events from inside the guest. Not
  recommended now: gdbstub does the same with zero guest code, and rm_debug expects its
  `DebugSecurityServer` user side. Revisit only for the real-phone case (where it would still have
  to be rebuilt against Nokia's 9.2 kernel, see §7).

**Host-side kernel walking (phase 0 `ps`):** dump the needed offsets once (`gdb -batch
_syborg_ekern.exe.sym -ex 'print &((DThread*)0)->iName'` …) into `build/n95/kstruct.json`; at
run time read `K::Containers[EProcess]` → `DObjectCon::iObjects/iCount` → each `DProcess`
(`iName` is a `HBuf` = length word + bytes, exactly what `syb_gdb.hbuf()` reads [verified]),
over QMP `x` (running) or gdb (stopped). Same for threads (`iNThread.iNState`, `iWaitObj`,
`iPriority`), servers (`DServer::iName`, sessions), chunks. This is what the LDD later returns
natively; the host walker remains useful when the guest is hung.

**Symbols:** three sources, one resolver (`symbols.describe(addr, pid)`):
1. our kernel: DWARF already loaded;
2. XIP ROM: `syb_sym` index (name only) and `xip2elf` symbol ELFs for gdb frames;
3. RAM-loaded code: run address from `SYBCODE:` / LDD codeseg list → our ELF (`add-symbol-file
   -s .text`), or `e32load.py --base` for Nokia ROFS binaries (import names only).
The data/bss of a RAM-loaded exe is per process in the multiple model [inferred, app-dev-debugging
§2]: gdb reads of globals are only right while that process is current (filter on
`TheScheduler.iCurrentThread`).

## 4. Phased roadmap

**Phase 0 — quick wins, 1–2 days, no new guest code except one trace line**
1. `tools/devtools/` skeleton: `session` (boot with `--keep-drives`, serial0 tailer with
   `wait(regex, timeout)`), `qmp` wrapper (keys/type/screenshot with crop+downscale+change hash),
   `kstruct.json` offsets + `ps`/`threads` over QMP `x`.
2. Kernel patch 0007: `SYBCODE: <name> code=<run> size=<n> data=<run>` in `DCodeSeg::Create`
   (always on, like `SYBPROP`).
3. Persistent gdb: `attach/break/continue/step/regs/mem/bt` with thread filter; app ELF
   auto-loaded from the `SYBCODE` line; build.sh gets a `DEBUG=1` (`-O0 -g`) variant.
4. MCP server (stdio) exposing the above; every result a small JSON object; raw logs stay in
   `build/runs/<label>/`. Deploy still `drive.sh` with QEMU off; launch via menu keys or a
   `_reg.rsc` (as today).

**Phase 1 — guest agent v1, 2–3 days**
- `devagent.exe` over serial1: `put/get/ls/rm`, `run/kill/ps/logon`, `apps/launch/fg`, `wg`
  (focus/window list), `unlock/keepawake`, `pget/pset`. Startup-list autostart. Host `agentlink`
  + MCP tools `deploy`, `run`, `kill`, `apps`, `ui.state`, `sys.keepawake`.
- Result: build → put → run → exit/panic event in < 10 s without a reboot.

**Phase 2 — kernel debug LDD, 3–5 days**
- `edevtools.ldd`: ring buffer; strace table swap (generated wrappers from `exec92.map`, names
  included); per-process IPC trace flag; codeseg list; kernel ps/threads/servers; thread exit
  hook with exit info; RDebug tagging. Agent forwards records as `trace.events(pid, since)`.
- Optional: own UART1 driver with DMA if `put` throughput is a problem.

**Phase 3 — debugger polish, 3–5 days**
- XIP symbol ELFs for gdb (`xip2elf`), locals/args via DWARF, `panic`/`exception` auto-capture
  (break on `K::PanicCurrentThread` and the exception handler, report thread + symbolised chain +
  user call site, as `syb_gdb` prints today), watchpoints, thread-aware stepping.
- qcow2 drives + `snapshot save/restore` for instant "fresh idle phone".

**Phase 4 — later**
- SIS install (hacked install server + silent launcher API, or an installer-free `put` of the
  package contents), TUI (`textual`), real-phone transport (§7), MobileCrash fix for Nokia-format
  crash reports.

## 5. MCP tool list (sketch)

All tools return compact JSON; no tool ever returns a transcript. Long data goes to a file whose
path is returned, with a `head` of a few lines. Times are guest-relative seconds.

| Tool | Args | Returns |
|---|---|---|
| `session.start` | `rom?, keep_drives?, trace_mask?, snapshot?` | `{label, boot_s, state: idle|booting}` |
| `session.wait` | `event: idle|regex, timeout` | `{matched, line, t}` |
| `session.status` | – | `{uptime, state, pending_ipc_count, last_panic}` |
| `session.stop/restart/snapshot` | `tag?` | `{ok}` |
| `log.tail` | `filter (regex/prefix), since, max=50` | `{lines:[...], more:n}` |
| `log.report` | `section: panics|exits|pending_ipc|stubs|props` | the `syb_report` section as JSON |
| `app.build` | `name, debug=true` | `{exe, elf, errors:[...first 20]}` |
| `app.deploy` | `path, dest=C:\sys\bin` | `{bytes, ms}` |
| `app.run` | `path, args?, wait_exit=false` | `{pid, started_t}` or `{exit:{type, reason, category}}` |
| `app.rerun` | `name` | kill+deploy+run in one |
| `app.apps` | `filter?` | `[{uid, caption, path, running}]` |
| `app.launch` | `uid|caption` | `{pid, wgid}` |
| `proc.list` | `view: user|kernel, filter?` | `[{pid, name, threads, exit}]` |
| `proc.threads` | `pid` | `[{tid, name, state, wait_on, prio, pc?}]` |
| `proc.kill/suspend/resume` | `pid|tid, reason?` | `{ok}` |
| `dbg.break` | `loc (addr|'Mod::fn'|'file:line'), thread?, cond?, once?` | `{bp, addr, symbol}` |
| `dbg.wait` | `timeout` | `{stop:{bp, thread, pc, frames:[...], regs, user_site}}` |
| `dbg.step/next/finish/continue` | `thread?` | same stop record |
| `dbg.regs/mem/read/write` | `addr, len | expr` | hex + symbolised |
| `dbg.bt` | `thread?, locals=false` | `[{fn, module, file:line?, args?}]` |
| `trace.strace` | `pid, on|off, filter?` | `{ok}`; records via `trace.events` |
| `trace.events` | `pid?, kinds: exec|ipc|rdebug|exit, since, max` | `[{t, kind, thread, text}]` |
| `trace.panics` | `since` | `[{t, thread, category, reason, user_site}]` |
| `ui.key/type` | `keys|text, delay?` | `{ok}` |
| `ui.screenshot` | `region?, scale=0.5, only_if_changed=true` | `{path, changed, hash}` |
| `ui.state` | – | `{focus:{wgid, name}, groups:[...], keyguard, screensaver, last_update_t}` |
| `sys.keepawake` | `on|off` | `{ok}` |
| `sys.prop` | `get|set, category, key, value?` | `{value}` |
| `fs.ls/get/put/rm` | `path` | listing / bytes written |
| `sym.describe` | `addr, pid?` | `{module, symbol, offset, source?}` |

Token rules baked in: `max` defaults of 20–50 lines, screenshots downscaled and skipped when
unchanged, stop records carry at most 8 frames, errors are one line.

## 6. Risks

- **Agent autostart** relies on the Startup List import directory [inferred]; fallback is the
  kernel-created XIP process (needs the phase-2 LDD, so plan the LDD skeleton early).
- **`put` into `C:\sys\bin`** needs TCB; the loader is assumed not to check an exe's own
  signature [inferred]. A cheap test is to run `wgprobe` with `--capability=all` first.
- **Serial1 throughput** (byte-wise FIFO I/O, interrupt per 64 bytes): fine for 100 KB exes,
  maybe not for multi-MB SIS payloads → DMA in our own UART driver or a custom MMIO device.
- **gdb while the guest runs**: breakpoints stop the whole machine; timers (1 ms tick, watchdogs
  in Starter/SysAp) keep counting on the host clock — long stops can trigger time-outs in the
  phone software [inferred]. Mitigation: `-icount` or `-rtc clock=vm` for debug sessions.
- **strace wrappers**: execs with `KExecFlagPreprocess` receive the looked-up kernel object, and
  extra-arg execs pass more than 4 args; the wrappers must be generated from the real table
  flags (`exec92.map` + `EpocSlowExecTable` flags) [verified table layout, wrapper ABI inferred].
- **Kernel-structure walking** depends on the current kernel build's DWARF; regenerate
  `kstruct.json` on every kernel build (hash check).
- **Snapshots** need qcow2 drives and a device-model that supports migration (our Syborg devices
  need `vmstate` descriptions; they likely lack them) [inferred] — plan 1 day for that or skip.
- **syb_gdb kills every `sybtool` QEMU**: the bridge must own its QEMU under a distinct `-name`
  and the old tools must not be run during a session (document, or teach them to refuse when
  `devtools` holds the lock file).

## 7. Real-phone portability (later)

Carries over: the agent (`MLink` on `RSocket`/RFCOMM instead of `RBusDevComm`), the line
protocol, the host session/MCP layer, the apparc/WSERV/keyguard/P&S code, the strace *record
format*. Needs the hacked install server (or a cap-granting signing hack) to run an ALL-caps
agent [inferred]. Does **not** carry over: QMP (keys/screenshots → agent must do them: WSERV
`SimulateRawEvent` needs SwEvent, screenshot via `CWsScreenDevice::CopyScreenToBitmap`),
gdbstub (no in-process debugger on the phone without a kernel driver), `edevtools.ldd` (built
for our kernel; Nokia's EKERN 9.2 has a different export table and no DDK here — a port is a
separate project), `SYBCODE`/IPC kernel traces, host kernel walking.

## 8. Recommendations

1. Build phase 0 now: the host session + persistent gdb + MCP + `SYBCODE` line pay back
   immediately, including for the current keyguard/redraw investigation (`ui.state`, `proc.threads`
   on a hung phone over QMP `x`).
2. Transport = serial1 with `RBusDevComm`; no new QEMU device until bulk transfer hurts.
3. Agent first, LDD second: the agent removes reboots from the loop; the LDD is what makes
   strace and clean process lists possible, and it is also where the autostart fallback lives.
4. Keep gdb as the only breakpoint/step engine; do not port rm_debug or TRK.
5. Design every tool result for an LLM: small JSON, events instead of polling, screenshots only
   on change. The TUI is a thin view over the same session, not a second implementation.
