# Tooling ideas

Missing tools that would have saved time, written down after long debugging loops. Reviewed
periodically by the project owner; implemented items move to "Done" with a pointer to the tool.

Format: one entry per idea: **what it is**, *which loop it would have shortened* (with a date), and
a rough cost.

## Open

- **Exec-call argument audit for the 9.2 personality**: list every 9.2 exec call whose argument
  types differ from ours (by value vs by reference, extra/missing args), by comparing the N95
  EUSER stubs' register set-up with our `execs.txt`. *2026-11-05: `TimerAt` passing `TTime&` cost
  the boot 140 s and was only found by stack-walking a spinning thread.* Cost: one day.
- **Undefined-instruction catcher**: report every UNDEF the guest takes with the instruction word
  and the module, so emulator decoding gaps (like the non-canonical `CLZ`) show up at once.
  *2026-11-05: the AknIconServer crash was first seen as a kernel fault with a confusing "far".*
  Cost: small (kernel trace in the undef handler or a QEMU TCG plugin).
- **DOS event log**: log every DosServer event raised (id, value, raising DSY) in the serial log, next
  to the `SYBPROP:` lines, so the ISI reply → DSY → DOS event → sae key chain shows in one run.
  *2026-11-05: the "no SIM" fix needed reading NokiaDSY and sae by hand to see that our 0x1C meant
  "card present" and that SEC cause 0x16 is ignored.* Cost: small (a `syb_gdb brk` preset on
  DosSrv's raise function, or a kernel-side hook on the DosServer session).

- **ISI reply-layout extractor**: for a client handler of a response, decode the ISIMSG field
  accessors it calls (`#165`/`#255`/... take a field id that maps to a fixed offset, `#303` finds a
  subblock) into "offset → accepted values", so a fake-modem reply can be written in one go.
  *2026-11-05: the RTC status reply `19 0B` took four boot-and-probe rounds because the field ids
  (0 → [10], 18 → sb+2) looked like raw offsets.* Cost: small (table of the ISIMSG accessor
  switch statements + a disassembly walk in `tools/isi.py`).

- **UI state probe**: one command that reports the foreground window group/app, whether the
  keyguard/screensaver/autolock is active (Avkon/SysAp P&S keys) and the last screen-update time
  (DisplayDriver ctl 9), so "screen frozen" vs "still locked" is decided from data, not from
  screenshots taken during a slow redraw. *2026-11-06: the keyguard investigation took 7 boots
  of key/screenshot guessing; the answer came from the code and the absence of ctl 9 lines.*
  Cost: small (extend `SYBPROP:` to the Avkon/SysAp categories + a `syb_report` section).

## Done
- `tools/syb_gdb.py` fixes (2026-11-05): stack reads clipped at the end of the supervisor stack (they
  failed silently, so `--stack` and kernel-symbol probes looked broken; the kernel .sym mapping was
  fine), user call site + 9.2 exec name at kernel-mode stops (replaces the "KERN-EXEC call-site
  logger" idea), probe address confirmation and gdb output on "no hits".
- `tools/syb_run.py` + `tools/syb_report.py` (2026-11-05): one-command boot/screenshots/keys and a
  structured report incl. the "likely blocked" IPC wait list.
- `tools/syb_gdb.py` (2026-11-05): leave tracer with module filter, breakpoints with kernel
  expressions (kernel DWARF loaded), current-thread names, symbolised sampling.
- `tools/isi.py` + `platform/isi/replies-bb5.txt + models/<id>/isi-replies.txt` (2026-11-05): ISI names, unanswered-request
  summary, reply table as data.

- `tools/syb_sym.py` (2026-11-05): address → module/export/function/literal, symbol search,
  callers of an export across all modules, string search with references, word dumps,
  per-function decompile.

## Ask the modem log first (2026-11-06)
The "frozen idle" hunt spent many runs on screensaver/keyguard timing before looking at unanswered
modem requests. Idea: `syb_run` report section "unanswered ISI requests" (resource, msg id, first
time, count, sending channel owner) so a missing fake-modem reply shows up on the first run.

## Log app error notes (2026-11-06)
"Carousel: Memory full" needed a manual hunt: a conditional breakpoint `User::Leave` with
`$r0 == -4`, then breakpoints on Hitchcock's EGL error paths. Idea: `sydb errnotes`, which logs every
Eikon error note as an event (app, error code, leave site). Breakpoints on `CEikonEnv::HandleError`
/ `CTextResolver::ResolveError`, or `User::Leave` with an error filter, plus the caller from LR. A
cheaper first step is `sydb dbg leaves CODE --thread T` (conditional Leave breakpoint and
backtrace as one command).

## Freeze and snapshot on panic ("fork on exception"), 2026-11-06
Hunting Carousel's USER-EXEC 3 (an integer divide by zero in Hitchcock that only happens with
some timing) took many runs: every breakpoint or watchpoint changed the timing and the bug went
away. Symbian has no fork(), but three things would give the same effect:
1. **Freeze on panic/exception** (kernel side, fits the phase-2 debug LDD): a DKernelEventHandler
   for EEventKillThread / EEventHwExc that, for a watched process, suspends the thread before it
   dies, prints registers, about 1 KB of stack with symbolised return addresses (SYBCODE/XIP map)
   and the faulting module, and raises a sydb event. Then `sydb dbg attach` sees the live state.
   It also catches User::Panic / RaiseException (USER-EXEC 3) through EEventKillThread.
2. **Snapshot on panic** (host side): the daemon sees `thread.panic` for the watched process,
   stops QEMU (QMP `stop`) and saves a snapshot (`savevm panic-N`; needs a qcow2 overlay for
   the drives). The moment can then be reloaded and inspected with gdb as often as needed,
   like a forked copy.
3. **Record/replay** for races: QEMU `-icount shift=auto,rr=record` and then `rr=replay` makes a
   failing run deterministic, so breakpoints no longer change the outcome.
1 is the cheapest and most useful first step; 2 builds on the daemon events we already have.
**Done 2026-11-06** as `sydb catch` + `sydb snap`/`fork` (docs/sydb.md "Catch, snapshots and
forks"), host side through gdb on cold paths (User::Panic, DThread::Die) plus QMP migration, with
no kernel change. Record/replay (3) is still open.

- **Disassemble a RAM-loaded (ROFS) DLL** (2026-11-07, 5730 NOKIATSY): `tools/e32load.py` does not read
  byte-pair compressed images. Workaround used: `elf2e32_next --dump=hc --e32input=X`, then turn the
  hex code dump into a binary and `objdump -b binary -m arm -M force-thumb` it (no import names).
  Wanted: byte-pair support in e32load.py, or `tools/fw/probe.py code NAME` doing the extract +
  decompress + ELF with import names in one step.
- **Find what start-up waits on**: the manual loop was syb_report on a log cut before the shutdown,
  then requests completed with -33/-3 to sysstart, then the next ISCPEER line. A `syb_report.py
  --until T --who sysstart` view (open requests of one client at time T, with the modem messages they
  caused) would make it one command.

- **Byte-pair compressed E32 images** (`0x102822AA`): `tools/e32load.py` reads only deflate/none, so
  ROFS executables of Symbian^3 (N8) cannot be loaded or symbolised; the composition research needed
  a throwaway decompressor. Add byte-pair decoding to e32load (and so to syb_sym for ROFS code).
- **Ghidra `fn` on N8 core DLLs** (`syb_sym.py fn gce.dll ...`) produced no `ghidra/*.c`; check the
  headless import for the N8 core image.
- **elf2e32_next `--libpath` with several entries looped forever** (it never advanced past the first);
  fixed in patches/elf2e32_next/0002-libpath-search.patch for the container build. The host build in
  ~/work/elf2e32_next is unpatched, so src/khr/build.sh links every import library into one directory.
- **`sydb wait-screen [--changed] [--stable MS] [--timeout S]`**: block until the screen changes and is
  stable, print the elapsed time. Timing UI steps (tap, then wait) was done with a scratch script that
  polled screenshot hashes; scripted flows also raced the first-boot wizard without it.
- **TLB/flush statistics on demand** (QEMU HMP command or QOM property): ASID context hits/misses,
  domain drops, large-page full flushes per index. Today they are printed to stderr every 5 s with
  `SYBORG_ASID_TLB_STATS=1`, and the large-page counter does not exist yet (docs/perf.md).

- 2026-11-09 (N8 Settings delay): `tools/ipc_latency.py` added (slow and open IPC requests from a KIPC-traced
  serial.log). Still missing: a way to keep the KIPC bit on without `sydb strace` (a `sydb trace ipc on|off`),
  catch thread filters that work for kernel-side breakpoints (thread names resolve to '?'), and a
  "who is this thread blocked on" query (wait object of a thread, from the kernel's NThread state).

- 2026-11-09 (5800 Settings root cause): IPC tracing needs message/session IDs,
  stable thread identity across renames, and notification-vs-synchronous labeling.
  `ipc_latency.py` currently pairs FIFO by client name/opcode, so cross-server
  collisions and gaps can produce misleading durations. A modem audit should flag
  mismatched service identifiers even when `deliver` exists. A request-semaphore
  wait alone must not be displayed as proof of an idle app. See the review in
  `docs/devices/nokia-n8-rm596/settings-delay.md`.

- 2026-11-10 (701/808 bring-up): a waiting thread's *user* call stack. In the flexible model gdb reads
  only the current process, so a thread blocked in User::WaitForRequest had to be caught with a hardware
  breakpoint inside EUser's WaitForRequest loop conditioned on its NThread (TheScheduler.iCurrentThread,
  0xC8000C28 on the syborgfx udeb kernel), then woken. A `sydb ustack THREAD` (switch to the thread's
  address space, dump and symbolise its stack from the saved user SP) would have made it one command.
  `tools/sydb/gdb/threads-blocked.gdb`'s "nstate" reads NThreadBase+77, which on this kernel is
  iTimer.iState (always 0); only its "nwait" (+40) is right. Also missing: `sydb mem` (free/total RAM
  pages; read here from TheMmu.iRamPageAllocator by hand) and a pending-sync-IPC list per thread
  (the scratch pairing script paired file-server messages wrongly).

- **ngage_sess: catches filtered by thread, and a cheap "who returned this error".** (2026-11-12, N-Gage
  Bluetooth) Finding where a -11 came from took many boots: a conditional catch on User::Leave or
  LeaveIfError is evaluated on every hit (~0.5-1 s each), which makes boot crawl, and the condition
  can't name a thread. Wanted: `--catch 'VA=LABEL?thread~SocketServer'` evaluated in the daemon from the
  current-thread word without a full register read, and an "exec return value" trace (kernel exec
  handler results != 0 per thread) to see failing kernel calls without breakpoints.
