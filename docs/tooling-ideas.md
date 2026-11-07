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
- `tools/isi.py` + `platform/isi/replies-bb5.txt + devices/<id>/isi-replies.txt` (2026-11-05): ISI names, unanswered-request
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
