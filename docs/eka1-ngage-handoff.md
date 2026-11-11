# Hand-off prompt: N-Gage QD (EKA1) boot on the `ngage` QEMU board

> Historical (2026-11-10): a hand-off prompt written when the boot stopped before the logo.
> The current state, tools and next steps are in [eka1-ngage.md](eka1-ngage.md).

Paste this into a fresh agent. It's self-contained.

---

## Your task

Get the original Nokia N-Gage QD firmware (RH-29, V 04.10, Symbian 6.1 / EKA1, S60 1st
Edition) to show its **boot splash / logo animation** on our QEMU board `ngage`, as fast as
possible.

Concretely: find why the S60 start-up stalls before anything is drawn, and give a concrete fix
or the next experiment. Advice is welcome too: a faster route, a wrong assumption, a missing
model.

Repository: this one (git, branch `master`). Read first:

- `docs/eka1-ngage-questions.md`: the question log (Q1-Q37+). Every board model or hack answers
  a numbered question, with evidence marked [V] (verified in ROM code or a run) or [I]
  (inferred). This is the main knowledge base.
- `docs/eka1-ngage-roadmap.md`: the plan (P0-P6) and the ROM facts.
- `docs/hacks.md`, section "N-Gage QD (EKA1) bring-up board": every declared workaround.
- `src/qemu-ngage/hw/arm/ngage.c`: the board, about 1,700 lines, one file.
- `src/qemu-ngage/nos-standin/nos_standin.S`: the guest-side stand-in stub for the cellular
  software (NOS).
- `eka1/tools/ngage_run.py`: the experiment runner. **Use it for every run.**

## Rules (project owner's)

- Local commits only, never push. Don't commit firmware bytes, screenshots or extracted
  files. `eka1/output`, `eka1/extracted` and `build/` are git-ignored or untracked; keep it
  that way.
- GPL sources (TriX, EKA2L1, oFono) are naming references only: copy no code.
- Systematic method: turn each unknown into a numbered question in
  `docs/eka1-ngage-questions.md`, answer it from ROM code, runs, web or other projects, and
  mark [V]/[I]. Change the board only from an answer, and name the question in the code
  comment.
- Don't touch the owner's uncommitted modem work (`src/qemu-syborg/*`, `tools/isi.py`,
  `tools/device.py`, `*.mmp`). The `ngage` board builds in its own tree and never rebuilds
  the shared Syborg QEMU.
- Every workaround is declared (log line at start-up plus an entry in `docs/hacks.md`).

## Build and run

```sh
scripts/build-qemu-ngage.sh                     # incremental, ~10 s; own tree build/qemu-ngage/
python3 eka1/tools/ngage_run.py --secs 150 --name myrun --objects --shot build/ngage/myrun.png
```

Runner options (all print to stdout; the logs are in `build/ngage/<name>.{log,serial,trace}`):

| option | what it does |
|---|---|
| `--objects` | the EKA1 "ps": kernel containers (processes, threads, chunks, servers, LDDs/PDDs, libraries) by name |
| `--stack THREAD` or `PROC::THREAD` | ROM return-address candidates on a thread's user stack, named from the export tables (Thumb marked T). Stale frames exist: trust the live SP (DThread +0x158). |
| `--trace VA=NAME[;gdb cmd;...]` | logging breakpoints: r0-r3/lr at each hit, run continues. Hot functions make the run crawl. |
| `--break VA[:cond]` | stop at the first hit; print registers and stack |
| `--mon 'x/8wx 0x...'` | QEMU monitor commands at the end of the run (virtual addresses work) |
| `--shot FILE.png` | screendump at the end |
| `--debug unimp,int,exec,nochain -- -dfilter A..B` | QEMU exec trace limited to a range |

Every board log line carries the guest `pc=`/`lr=` (`ngage:` catch-all, `ngage-cbus:`,
`ngage-intc:`, `ngage-timer:`, `ngage-nos:`, `ngage-lcd:`). The kernel's debug output (trace
port) goes to `<name>.serial`.

ROM and analysis:
- `eka1/output/rom/ROM.bin` is the core image, virtual 0x50000000. Physical: offsets
  0..0xEFFFFF on flash chip 0 (0x00000000), the rest on chip 1 (0x02000000).
- `eka1/output/rom/manifest.json` maps files to virtual addresses.
- `eka1/analysis/user-exports.tsv` holds the names of every user-side DLL export (from the S60
  1.2 SDK `.lib` files).
- `eka1/analysis/ekern-align.tsv` plus `eka1/tools/ekern_evidence.tsv` hold the kernel export
  names that are known (ekern export table at 0x50342068, 506 ordinals).
- A Ghidra project with the ROM and these names is served over REST on `127.0.0.1:8089`
  (`get_function_by_address?address=`, `decompile_function?address=`).
- Disassemble with
  `objdump -D -b binary -m arm [-M force-thumb] --adjust-vma=0x50000000 --start-address=.. --stop-address=.. eka1/output/rom/ROM.bin`.
  User-side code is often Thumb (EFile, EFSRV, EStart, Starter, apps); kernel-side code is ARM.

## Latest result (2026-11-10, Q39-Q44)

**The blue NOKIA boot logo has been displayed**, with a debugger-only diagnostic.
The initial blocker is now narrowed to the C: flash filesystem:

- EStart's initial flash mount returns -20, then it falls back to RAM C:.
- Starter 0x509610cc checks `RFs::Drive(C:)` and requires media type 6 (flash).
  The board returns type 5 (RAM). Starter overwrites its startup reasons with
  (0,210), selects resource 10, and skips SplashScreen.
- Letting that single check accept RAM selects (4,100), starts SplashScreen,
  and renders NOKIA. Evidence: `build/ngage/q43_live.png`,
  `q43_accept_ram.{trace,out,log}`. A later fault occurs in EFile::Main;
  animation and complete startup are **not** verified.
- Item 12 values 1 and 2 did not fix this. Value 105 tells EStart to format
  flash instead of taking the RAM fallback, but `q44_format` has not reached
  Starter after 125 s and ends in the fatal loop. Next repeat with
  `--break 0x5031e340` to capture the format-path fault.

Reproduce the logo diagnostic (no ROM file or board-source changes):

```sh
python3 eka1/tools/ngage_run.py --secs 140 --name logo-diagnostic \
  --trace '0x509610ea=Q43_accept_RAM_C_diagnostic;set *(unsigned*)$sp=6' \
  --objects --shot build/ngage/logo-diagnostic.png
```

This explicitly misreports the media type at one call site for diagnosis;
it is not a flash implementation or a permanent fix. The notes below describe
previous observations and are superseded by this section where they conflict.

## Where the boot is (2026-11-10)

Working, in order (all modelled from ROM evidence):

1. **Bootstrap.** Two Intel CFI flash chips, SDRAM at 0x0A000000, clock lock bits.
2. **CBus** to the power chip (UEM, device 1) and device 4.
3. **MD5 integrity check.** A declared workaround (H-ngage-md5), because the NOS code is
   scrambled in the flash file.
4. **Timers.** 6 timers at 0x0C120A00 + 0x20·i, sources 0-5. Timer 0 = 34.67 MHz tick,
   timer 1 = one-shot timer queue.
5. **Interrupt controller.** 0x0C120100, 4 banks; an interrupt name table maps names to
   sources (Q30).
6. **Kernel debug port** (0x0DE00000).
7. **NOS stand-in.** The cellular software (NOS, on the same ARM) is scrambled, so it's
   replaced by `nos_standin.S`, copied over the NOS entry point.
   - It registers variant interface slot 3 (NOS data reads, sync and async with DFC
     completion) and sets the NOS status word to 3.
   - It installs the power model.
   - It provides the 2 NOS exports `nosdriver.ldd` uses.
   - Every call traps to a board device at 0x0C1F0000 and is logged as `ngage-nos:`.
8. **File systems.** EFile mounts the drives. The MMC socket times out "no card" once timer 1
   works.
9. **First processes.** EStart starts EwSrv, FbServ and Starter.
10. **LCD.** The interface at 0x0C121200 is a DCS command bus (Q37). The panel answers ID1
    0x83. The display model renders the panel's memory writes, and also (H-ngage-fb) shows the
    screen chunk directly:
    - address `[[0x800066EC]+24]`, virtual 0x5A090000, physical 0x0A000000;
    - a 32-byte header, then EColor4K pixels, 176×208.

`--objects` after ~120 s:
- **processes**: EKern, EFile, EwSrv, FbServ, Starter, SharedDataServer, DosServer, AppRun
  (SysAp), AppRun (Startup);
- **screen**: white (the window server's clear; content 0x0FFF).

## The current problem

**Starter (system-critical) crashes and the kernel halts. The splash screen never starts.**

Facts [V]:
- The crash is `RArrayBase::Count()` on a NULL array, called from Starter's `RunL` →
  `CBaBackupSessionWrapper::NewL` (Bafl) → `User::Leave(-1)` → cleanup of the half-built
  wrapper.
- The backup server ("BackupServer") lives in the **Eikon server** (`EikSrv.dll`,
  `EikSrvs.exe`). That process is never created.
- `EikSrvc.dll` starts `eiksrvs.exe` on demand ("EikAppUiServerStartSemaphore").
- Process creations (trace of `RProcess::Create` 0x503B3514): EFile, EStart, EwSrv, Starter,
  then 2× AppRun (SysAp, Startup) from `EikDll::StartAppL`. **No SplashScreen, no Phone, no
  EikSrvs.**
- Starter's resource `Z:\System\Programs\Starter_Arm.RSC` has several start lists:
  - a full list: `SPLASHSCREEN.EXE`, SelfTestServer, EInfoServer, randsvr, DbRecovery, Sae,
    SysAp, Phone, Watcher, ScreenSaver, Autolock, Startup, …;
  - shorter lists.
  - We get the shortest: **SysAp + Startup only**. So Starter believes this is a non-normal
    start (charging, alarm or test?).
- Starter's strings include SharedData keys `sys.swsr`, `sys.wd2sr`, `sys.rfss`, `state.val`,
  `state.src`, plus `C:\system\bootdata\FirstBoot.dat`, `Z:\system\rfs\ScriptInit.txt` and
  `S60SplashScreenGroup`.
- EStart prints "auxstart 0" on the debug port.
- EStart uses `helper.ldd` (HelperDrv):
  - DoControl 0 returns the variant's start-up mode. It's now 4: CBus dev 1 reg 0x15 bit 4,
    since 2026-11-10. It was 9 before.
  - DoRequest 1 is an asynchronous NOS read of **item 12** (2 bytes). The stand-in answers 0.
    The value is EStart's request result.
- Changing the mode to 4 didn't change Starter's list. An experiment with item 12 = 1 / 2
  (`NGAGE_ITEM12` env override in the board) is running.

Open hypotheses:

1. Starter selects its list from a start-up reason that the **NOS** provides: item 12 via
   EStart/HelperDrv, or a SharedData key (`sys.wd2sr` = "WD2 start-up reason"?). Our stand-in
   answers 0 everywhere, so Starter picks a minimal/abnormal list.
   - Look at how EStart turns item 12 / the mode into "auxstart N" and what it publishes.
   - Look at how Starter reads `sys.swsr`/`sys.wd2sr`/`state.val` and indexes the
     `Starter_Arm.RSC` lists.
2. The Eikon server should be running before Starter's backup-session code. Who normally starts
   it (SysAp/Startup through EikSrvc, or the splash screen)? Why hasn't it started here?
3. Something the NOS normally answers over ISI (PhoNet via `nosdriver.ldd`; DosServer is up)
   is needed first. No PhoNet channel traffic has been seen yet (Q36).

## Useful addresses

| what | address |
|---|---|
| ekern fatal-fault routine (r0 = category descriptor, LR = caller) | 0x5031e340 |
| current thread | `[0x80006740]` |
| containers | processes 0x80000398, threads 0x8000039c, chunks 0x800003a0, servers 0x800003b8 |
| variant (`Kern::Variant()`, ecust.dll object) | `[0x8000038c]`; vtable 0x503458C4; +0x100 Get(slot), +0xFC Register, +0x90 NOS status, +0xD0 start-up mode (cached at +0x14) |
| `helper.ldd` channel vtable | 0x5037F91C (DoRequest 0x5037F0AC, DoControl 0x5037F644) |
| `lcd.ldd` channel vtable | 0x504081AC (DoControl 0x5040410C: functions 0x1001-0x100D, 0x2001-0x2006, 0x3011) |
| `User::Leave` | 0x503A16FC |
| `RProcess::Create` | 0x503B3514 |
| Starter.exe | 0x509601D0-0x50962FAC (Thumb) |
| EStart.exe | 0x503E9770-0x503EA97C (Thumb) |
| EikSrvc.dll | around 0x5063D2B4 ("eiksrvs.exe") |

## What would help most

- The fastest path to the splash screen and logo animation: which value, start-up reason or
  missing server to fix first, with evidence (ROM code addresses).
- Any known facts about S60 1.x Starter start-up modes (normal, alarm, charger, test), the
  `sys.*` SharedData keys, or DCT4 "WD2 start-up reason" codes.
- If you find a wrong assumption in the board (timer clocks, interrupt semantics, CBus
  values), say which question number it contradicts.
