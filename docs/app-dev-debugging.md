# Developing and debugging our own apps on the virtual N95

Question (project owner, 2026-11-05): we want to "vibecode" WSERV/Avkon apps (C++, Rust, Zig)
against the N95 user side running on our kernel, with real debugging. Own launcher + debug stub,
AppTRK revival, raw QEMU gdbstub, or a hybrid? Tags: [verified] = checked in this repo/firmware,
[inferred] = best guess, not checked.

## 1. The inner loop an AI agent needs

| Step | Today | Target | What costs tokens |
|---|---|---|---|
| edit | source in `src/n95apps/<app>` | — | nothing |
| build | GCC 14 symbianelf / CSL 4.4 + elf2e32 → `.exe` (first exe being set up by another agent) | < 5 s | compiler errors only |
| deploy | `scripts/n95-drive.sh put c app.exe /sys/bin` with QEMU **off** [verified: tools.md] | < 1 s off-line; live transfer later | nothing |
| launch | reboot (~10 s to idle [verified: memory/roadmap]) + navigate the menu by QMP keys, or a guest launcher | < 2 s after boot, 1 command | key scripts + waiting |
| observe | `serial.log` (RDebug::Print already reaches it [verified: n95-logging.md]), `syb_report.py` (exits, panics, pending IPC), QMP screenshot | report in 1 call | **screenshots** (image tokens) and raw logs (20 k lines per boot [verified: gui-234609]) — always filter |
| debug | `syb_gdb.py brk/leaves/sample` with kernel DWARF, thread names, user call site at exec stops [verified: tools.md] | one structured probe per iteration | raw gdb transcripts; a probe that misses and needs re-running |
| iterate | 30–60 s per round (boot included) | 15 s | repeated boots |

Rules of thumb for token efficiency: never paste a raw serial log or gdb transcript; the agent wants
one compact answer per probe ("panic KERN-EXEC 3 in myapp.exe::Main at myfn+0x1c, locals …").
Polling (sleep/screenshot loops) is the single biggest waste; prefer an event ("process exited",
"RDebug line matched") to a timer. Screenshots are for layout checks, not for "did it start".

## 2. Options

| | (a) Own launcher + stub | (b) AppTRK revival | (c) Raw QEMU gdbstub | (d) Hybrid: gdbstub + structured wrapper |
|---|---|---|---|---|
| What it is | Guest-side `applauncher.exe` listening on serial1 (or a kernel LDD channel): start exe by name, forward RDebug/exit codes/panics; host side in `tools/` | Nokia's TRK debug agent (Carbide.c++ era) running in the guest, talking to a host implementing the TRK protocol | `qemu -s`, gdb against the whole machine | Keep (c), add a host tool (`syb_gdb`-style) with high-level commands returning JSON, auto symbol loading from kernel traces |
| In the ROM? | nothing needed | **No.** Core ROM: `trkdummyapp*.exe` ×4 (4.9 KB placeholders); ROFS: `TrkApp.exe` 0x690 bytes = stub [verified: core-files.txt, rofs1-verbose.txt]. The real agent (`TrkApp` + `TrkKernelDriver` LDD) came as a SIS with Carbide, not in the firmware | works today (`tools/syb_gdb.py`) [verified] | same |
| Transport | serial1–3 exist on the board (`c0007000`…), QEMU creates 4 serial devices, only serial0 is wired [verified: syborg.c, run-modern.sh]; Nokia's `c32exe`, `Ecuart.csy` are in the core ROM, our `ECOMM.LDD`/`EUART1.PDD` on the kernel side [verified] → a plain `RComm` serial path exists **or** a tiny custom LDD on serial1 (no C32 dependency) | TRK on S60 3rd used USB (`Ecacm.csy`) or Bluetooth (`btcomm.csy`) [verified they exist; no USB/BT on Syborg]. Would need the serial variant (TRK had one, 2006-era) | TCP :1234 | TCP :1234 |
| Effort | 1–2 days: launcher exe (RProcess::Create, Rendezvous/Logon, RDebug relay) + host script; +1 day for live file transfer over the same channel | 5–10 days **plus** sourcing the SIS/sources: TRK's kernel driver is built against Nokia's kernel; on our kernel it must be rebuilt from the Symbian Foundation `debugsrv`/TRK sources or replaced by `kernel/eka/drivers/debug/rmdebug` (present in our tree [verified: ls]) — i.e. a port, not a revival | 0 days | 2–3 days: symbol auto-load, `app` sub-commands, JSON output, kernel trace line |
| Unlocks | start/stop by name, exit code and panic category in one line, RDebug stream per app, no menu navigation, no `_reg.rsc` for tests | source-level step/break in-process, process-aware, IDE support (Carbide, 2008) | kernel + user breakpoints, memory, registers, backtrace, kernel DWARF; whole-machine view | everything (c) does, plus app symbols, locals, break-at-app-symbol, compact output; process control through (a) |
| Does not give | breakpoints/stepping (needs c/d) | GCC 14 DWARF: TRK carries no symbols — the host (Carbide) resolved them from `.sym` files; a host of our own would need the same loader-address bookkeeping as (d) anyway [inferred] | process awareness (see below), app symbols, start-by-name | in-process stepping across pre-emption is still "global" (see risks) |
| Risks | platsec: launcher needs caps to create processes? (`RProcess::Create` needs none; the target exe's own caps are checked by the loader) [inferred]; serial1 not yet wired in our variant | 2006 protocol, no public spec for the Nokia variant, kernel driver port; nobody will use Carbide | gdb has no notion of a Symbian process/thread; stops land in any thread; memory not mapped in the current context reads as garbage; symbols for RAM-loaded code must be placed by hand | moderate: depends on a trace line and gdb scripting; thread-filter already exists (`--when-thread`) [verified] |
| Token efficiency | high (one line per event) | medium (if wrapped) | **low** (gdb chatter, manual address juggling, re-runs) | high (JSON, one probe = one answer) |

### The address-space problem with raw gdb (c)

- Kernel: multiple memory model [verified: architecture.md]. XIP ROM code at its link address
  (`0x80000000`+) and the kernel at `0x80003000`/`0xc8000000` are global, which is why
  `syb_gdb.py` works today with one `add-symbol-file` [verified].
- RAM-loaded EXEs/DLLs (anything from C:, E:, ROFS) are relocated by the loader to a run address
  in the user code region (example from the kernel trace: `70014000`) [verified: e32load.py
  docstring]. In the multiple model this address is unique per code segment and the same in every
  process that loads it [inferred from `CodeSegsByAddress` being keyed by run address,
  scodeseg.cpp:729], but the pages are only mapped in the processes that have the segment
  attached; a breakpoint by address fires in the right process (TCG hw breakpoints are
  address-based [verified: tools.md]), but gdb reads of that memory while another process is
  current return the wrong thing.
- Therefore symbol loading needs: the ELF with DWARF (our own build, before elf2e32) and the
  run address. `tools/e32load.py --base` already builds a relocated ELF for RAM-loaded images
  [verified]; for our own apps the simpler route is `add-symbol-file app.elf -s .text <base>`
  with the base = run address + (link code base), i.e. the DWARF is ours and needs no
  reconstruction.
- The run address today is only printed under `KDLL` (mask bit 10, `0x400`) as `CodeSeg @%08x
  Added By Address %08x` (no name; the name is in the earlier `DoCreateRam %C` line)
  [verified: scodeseg.cpp:729, mcodeseg.cpp:340, nk_trace.h]. KDLL is chatty; we want one
  dedicated line.

### Why not TRK (b), in short

Not in the firmware (only stubs), transport is USB/BT, the kernel-side half is Nokia's and would
have to be re-implemented on our kernel, and the result is a 2006 protocol with no symbol handling
of its own. Everything it would give, (d) gives with less work because QEMU already has the kernel's
view of every thread.

## 3. Recommendation: (d) on top of (a), phased

Keep the gdbstub as the only debug transport; add a guest launcher for process control and
output, and make the host tool "app-aware". No TRK.

### Phase 1 (about 1 day): app-aware probes with what exists

1. **Kernel trace line** (patch `patches/kernelhwsrv/platform/0007-debug-trace-codeseg-load.patch`, same
   style as 0004 `SYBPROP:`): in `DCodeSeg::Create` (scodeseg.cpp, after "Added By Address") print
   `SYBCODE: <filename> code=<iRunAddress> size=<iSize> data=<data run addr>` under an
   always-on switch (or under `KPANIC` like the other N95 lines) so it costs nothing to
   enable. XIP segments can be skipped (their address is the ROM link address).
2. **`tools/syb_gdb.py app`** sub-commands (host only, no new guest code):
   - `app symbols NAME ELF`: wait for the `SYBCODE:` line in the serial log, then
     `add-symbol-file ELF -s .text base [-s .data …]` and record the mapping in
     `build/runs/<label>/symbols.json`.
   - `app brk NAME 'fn(args)'` → breakpoint at the app symbol with `--when-thread NAME`, prints
     a JSON object: thread, frame list (symbolised via DWARF), `info locals`, `$r0-$r3`, and the
     user call site for exec stops (already there).
   - `app panic NAME`: breakpoint on the kernel's thread-exit path filtered to the app, reports
     exit type/reason/category (the kernel already has these in `DThread`; `syb_report.py`
     parses the exit lines from the log [verified: tools.md]).
   - All output as one JSON blob, raw gdb text only in `build/runs/gdb-*.txt`.
3. **Deploy** while QEMU is off: `scripts/n95-drive.sh put c app.exe /sys/bin` into the
   persistent C: image (`build/n95/drive-c.img`) [verified: tools.md]. Keep `--keep-drives` for
   scripted runs so the app survives.
4. **Launch** for tests without a menu entry: a one-off "run this on boot" path is simplest —
   the launcher of phase 2; until then, either the existing N95 file manager via QMP keys
   (slow, token-hungry) or an autostart via `scripts/n95-drive.sh put c` of a `_reg.rsc` +
   menu navigation. Recommended: skip straight to phase 2 for launching.

### Phase 2 (1–2 days): guest launcher on serial1

- `applauncher.exe` (our own, in `src/n95apps/launcher`, built like the test exe): opens serial1
  (`RComm` through C32/`Ecuart.csy` if our variant exposes port 1; otherwise a 200-line LDD
  `elaunch.ldd` on the Syborg serial1 registers — no C32, no caps, [inferred] cheaper and more
  robust), then runs a line protocol: `run NAME [args]`, `kill NAME`, `ls`, `put NAME size` (raw
  bytes to `C:\sys\bin`), `get`. Replies are one line each: `started NAME pid=… `,
  `exit NAME type=<Kill|Terminate|Panic> reason=… cat=…` (from `RProcess::Logon`),
  `rdebug NAME: …` (not needed: RDebug already lands in serial0; the launcher only tags process
  lifetime).
- Host: `tools/n95app.py run|put|kill|watch` over `-serial unix:build/runs/<label>/serial1.sock`
  (add a second `-serial` to `scripts/run-modern.sh`), reusing `syb_run.py --keep-drives`.
- Start: a `_reg.rsc` in `\private\10003a3f\import\apps` is only needed for a menu/Shell entry
  [inferred: standard S60 3rd]; the launcher starts the exe by path with `RProcess::Create`, no
  install, no SIS — fine for tests and for Avkon apps too (an Avkon app is an exe with
  `CApaApplication`; started directly it appears as a window group) [inferred]. The launcher
  itself must be started once per boot: the kernel cannot create user processes for us, so
  either give it a `_reg.rsc` on C: and start it by menu once, add it to Starter's start list
  (a byte patch via `romcompose.py --patches`, or a C: copy of the start-list resource), or let
  our ROM composer put it into the kernel-side file list as an autostart exe like `EXSTART`
  does for our own shell [inferred].

### Phase 3 (2–3 days): MCP-shaped tool

Wrap `n95app.py` + `syb_gdb.py app` behind one process with the commands `build`, `deploy`,
`run`, `wait_exit`, `break`, `backtrace`, `locals`, `screenshot(region)`, `log(filter)`.
Whether it is an MCP server or a CLI returning JSON does not matter for token cost; what matters
is that every call returns one small structure and never a transcript. Live transfer (phase 2
`put`) removes the reboot from the loop: build → put → run → observe in < 10 s.

### Platsec and capabilities [inferred unless noted]

- PlatSec enforcement is on in our ROM header [verified: architecture.md]. The loader checks the
  E32 header capabilities of the exe we build (elf2e32 `--capability`); for a test exe with no
  caps nothing is checked. WSERV/Avkon apps need no caps for drawing.
- Executables must live in `\sys\bin` of a drive; `C:\sys\bin` is searched before `Z:`
  (so a replacement `installserver.exe` on C: would take precedence). No SIS, no signing: the loader does not verify signatures, only the
  install server does.
- A launcher calling `RProcess::Create` needs no capability; `kill` of another process needs
  `PowerMgmt` — give the launcher `ALL -TCB` when building (elf2e32), the loader accepts any
  caps on a ROM/C: exe.
- Our kernel's rm_debug driver sources are present (`kernel/eka/drivers/debug/rmdebug`) if a
  guest-side debug agent is ever wanted; not needed for (d).

## 4. Open questions / check first

1. Is serial1 reachable from the N95 user side: does our variant register a second port with
   `ECOMM.LDD`/`EUART1.PDD`, and does `Ecuart.csy` open `COMM::1`? If not, the small LDD route.
2. Confirm that `C:\sys\bin\x.exe` wins over `Z:\sys\bin\x.exe` for a name that exists in both
   (the installserver case) and that a new name on C: is found at all (loader search path).
3. Run address: check that the `Added By Address` value equals the address gdb sees for the app's
   code (link code base `0x8000` [verified: rofs1-verbose TrkApp header] + relocation) and what
   the data/bss run address is in the multiple model (per-process; gdb needs the current process).
4. Does `RProcess::Logon` on the N95 EUSER report panic category/reason through our 9.2 exec
   personality (exit-info exec calls are shimmed?) — otherwise read the kernel's thread-exit trace.
5. gdb and GCC 14 DWARF 5 for ARMv5 EABI: `add-symbol-file` of the pre-elf2e32 ELF with
   `-s .text` only; verify `info locals` works with `-mapcs-frame`/no frame pointer (prefer
   `-O0 -g -fno-omit-frame-pointer` for debug builds).
6. Avkon apps started by path (no `_reg.rsc`): does `CEikonEnv` construction need the app's
   registration from `apparc`? If it does, test exes stay plain WSERV clients and Avkon apps get
   a `_reg.rsc` put into `C:\private\10003a3f\import\apps` off-line.
7. Does QEMU's gdbstub keep hardware breakpoints across the ASID switch when the app's page is
   unmapped in the current process (expected yes under TCG; verify once with a breakpoint set
   before the app starts).
