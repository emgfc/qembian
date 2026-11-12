# Hosted EKA2: phased roadmap, exit and kill criteria

Effort is one experienced person with agents, full time, in calendar weeks; ranges are honest guesses
[inferred]. Every phase ends with a written result in this folder; a kill criterion met sends the
work back to (d) (today's QEMU, incl. Android) or, for a non-Qualcomm phone with EL2, to (b).

| Phase | Content | Exit criteria | Effort | Kill criteria |
|---|---|---|---|---|
| **P0 feasibility on real hardware** | On a Pixel 3a (stock + adb, or rooted): `getprop ro.product.cpu.abilist`; run a static armhf binary; `cat /proc/cpuinfo`; boot log / `dmesg` for "started at EL1/EL2"; on a pmOS or Mobian boot (`fastboot boot`, no flash): `/proc/sys/abi/swp`, `cp15_barrier`, a 32-bit test that `mmap`s `0x80000000`, `0x90000000`, `0xC8000000` `MAP_FIXED`, executes `swp`, a CP15 `dmb` and a `svc #0x800001` under a seccomp filter keyed on `instruction_pointer`; `getconf PAGE_SIZE`; display/touch/evdev/DRM present | 32-bit EL0 works; the three mappings succeed; `swp` and `svc` are trapped as designed; `/dev/kvm` absent (documents option b as dead); pmOS/Mobian boots with display and touch | 1-2 days | 32-bit processes refused by the mainline kernel on this SoC, or a fixed mapping impossible, or no Linux image boots with a display: stop, keep (d) |
| **P1 hosted nanokernel** | `nkern/linux` from `nkern/win32` + the generic `nkern/*.cpp`, built with `arm-linux-gnueabihf-g++`; scheduler lock, futex parks, timer thread (1 ms), DFC threads, `NKern::Sleep`, fast mutexes, nanokernel timers; a 64-bit nucleus only as launcher; runs on the `virt` machine (`qemu-setup.md`) | a test exe with two nanothreads and a semaphore, timers firing at 1 ms, round-robin at one priority, a DFC queue, all under the EKA2 scheduler's decisions (trace shows the same reschedule sequence as the Win32/ARM kernel for the same program) | 2-3 weeks | the Win32 scheduling model cannot be reproduced with futexes/signals without touching `nkern/*.cpp` beyond `#ifdef` glue |
| **P2 first unmodified ARM32 Symbian exe** | `kernel/linux` (exec dispatch, personality tables from `exec.map`, exceptions via signals), minimal `memmodel/emul/linux` (ROM mapping, kernel data memfd, chunks, one process), trampoline patching of EUSER stubs by the composer, seccomp safety net; the ROM's EUSER + a tiny ROM-resident test exe (our own, built with GCCE as today, placed in the composed ROM) | the test exe runs from the ROM at `0x80000000+`, allocates on `User::Heap()` through fast exec F01, creates a thread, waits on a semaphore, `User::After`, `RDebug::Print` arrives on the trace socket; exec count per second measured with and without stub patching; `swp` sites counted | 3-5 weeks | patched-stub exec cost > 1 us or SIGSYS path needed for > 5 % of execs after patching; or EUSER needs a kernel service that only the MMU can give (none expected: the Win32 emulator proves EUSER's needs are logical) |
| **P3 processes and client/server IPC** | one Linux process per Symbian process; the agent thread; shared kernel data across processes; global chunks; IPC descriptor copies (memfd alias); `RServer2`/`RSessionBase`, `RMessage2` read/write, request completion across processes, thread/process death and cleanup | two ROM exes (our own) as client and server exchange descriptors both ways 100,000 times; a client death mid-message is handled (KERN-EXEC panics as in the reference); the exec/IPC traces match the syborg reference for the same binaries | 4-6 weeks | cross-process kernel state cannot be kept coherent without a lock per exec (performance), or thread creation/kill across processes is unreliable under signals |
| **P4 F32 and system start-up** | RAM-loaded code segments (memfd at the fixed code address, the loader path), local media PDD over drive images, ROFS media stand-in, `ESTART`, the phone's `efile.exe` loader, then the real boot: `domainSrv`, Starter/SSM, the fake modem (compiled in), `ECOM`, `FBSERV`; no display yet | the N95 ROM (sym92 personality) boots to `100058f4/1 = 3` (start-up done) headless with every Nokia server of the reference boot started; exec trace of `efile` start matches the reference call-for-call; a sum of unanswered requests equal to the reference | 6-10 weeks | the loader/codeseg model needs per-process code addresses that the multiple model does not have (then the moving-model path, Win32-style data copying, would be required: an extra 4-6 weeks, decide then) |
| **P5 WSERV with a framebuffer** | LCD stand-in with a DRM/KMS (virt: `virtio-gpu`) back end, keys/touch from evdev, sound to ALSA (optional), `sydb` `hosted` session type: log, screenshot, keys, strace, catch, gdbserver | the N95 home screen, Carousel and Switch off (`tests/device-smoke.sh` steps) on the `virt` machine; screenshots match the reference within tolerance; the N8 (flexible model, ARMv6K EUSER with TPIDRURW) boots to its home screen without the GPU path | 4-8 weeks | WSERV/alf depend on a display driver behaviour only an MMU-backed shared chunk gives (unexpected) |
| **P6 the Pixel 3a** | pmOS/mainline (sdm670-mainline tree) on the phone, the same user-space tree, DRM panel + touch, ALSA; measure: boot time, app start, exec cost; then the N8/808 host-GPU path through Mesa freedreno | N95 home screen on the phone's display with touch/keys at native speed; boot time measured against the Android TCG build (expect >= 5x faster [inferred]); N8 home screen; a written comparison | 3-6 weeks (+4-8 for the GPU path) | panel/touch on mainline unreliable on a given unit (aftermarket displays break touch [verified: ferdiOS note]) with no Halium fallback; or native speed < 2x the TCG build (then the signal/trap overheads dominate and the design is wrong) |

Total to a phone home screen: 24-40 weeks; with the GPU path and the 808: 30-50 weeks.

## Main risks (ordered)

1. **The semantics tail (P3-P4).** Everything the ARM kernel does in `.cia` (exception frames, user
   callbacks `NKern::QueueUserModeCallback`, `User::Leave` frames, `TRAP` fast execs, thread kill
   from another process, trap handler on exception) must be re-done with signals and `ucontext`.
   The Win32 variant shows it is finite (1,425 + 2,176 lines) but each piece hides a week.
2. **Exec cost.** Patched stubs make it a call; anything left on the SIGSYS path costs microseconds.
   `User::Heap()`, `TrapHandler`, `PushTrapFrame` are per-call fast execs. Count them in P2.
3. **`swp` and CP15 barriers.** Every `swp` is an undef trap under Linux emulation. If the count per
   second is high (request completion is one per completed request), patch those sites too.
4. **Phone kernel-side drivers.** Any phone LDD/PDD we keep today (kabi-checked) would need the hosted
   kernel to export the 9.2 vtable ABI from a Linux-built ELF. Decide per driver in P4; the
   fallback is a stand-in.
5. **Fidelity.** No kernel/user protection, no ASID/domain effects, Linux signal timing. Bugs found on
   the hosted machine must be reproduced on the syborg reference before they count as firmware facts.
6. **Hardware window.** AArch32 user mode exists on the 3a and on 2022-23 SoCs' little/mid cores only
   (Linux asymmetric 32-bit); Pixel 7+/X3-A715 class cannot run it. (d) stays the portable path.

## What each exit delivers

- P0: a one-page fact sheet about the phone (cheap, useful for (b)/(c) decisions too).
- P2: a measured answer to "is a hosted EKA2 fast and sane on this hardware", with the trampoline
  composer feature usable on its own for an exec-cost study.
- P4: the first headless native Symbian boot; the exec/IPC trace tooling works on both machines.
- P5/P6: a native-speed N95/N8 on the Pixel 3a with sydb; the QEMU machine stays the reference and the
  only option for 64-bit-only hosts.

## First real phone environment (decided 2026-11-12)

The hosted kernel targets a real phone user side (unmodified ROM; drivers replaced by stand-ins, the ROM
recomposed as today), in QEMU first.
1. **Nokia N95 / N95 8GB first** (S60 3rd FP1, Symbian 9.2, ARM1136 = ARMv6, framebuffer UI without
   compositing). Why it fits best: the **multiple memory model** gives every process the same fixed
   addresses (ROM 0x80000000, process data 0x00400000+, global 0x90000000), which maps 1:1 onto one Linux
   process per Symbian process with the kernel data shared (htest t_map, t_shared); it is the best-tested
   phone here (sym92 personality, seeds, smoke test, sound, SIM, SMS); ARMv6 semantics are closer to an
   ARMv8 AArch32 core than ARMv5. Its user code is still largely ARMv5-built: SWP sites must be counted
   (trap cost) and patched if hot.
2. **Nokia N73** (S60 3rd, Symbian 9.1, ARM926 = ARMv5TE, **moving** memory model, `syborgmv`): later, as
   a compatibility check. The moving model relocates the running process's data (run vs home address),
   which is the Win32-emulator style of data copying; plus pre-ARMv6 unaligned-load rotation semantics.
3. **Nokia 5800** (S60 5th, touch, flexible model, no GPU compositing): only if the N95 path fails; later
   phones (N8, 701, 808) need the GPU path and compositing.
Before P4: htest cases for the ARMv5/v6 behaviours the N95 ROM relies on (unaligned accesses, SWP in hot
paths, LDREX/STREX, VFP), run on the virt machine and on the phone.

## N95 boot status (2026-11-12) [verified, kn_efile]

The unmodified N95 user side runs on the hosted kernel up to the system servers: efile.exe (drives Z:,
C:, E:), EStart.exe, domainSrv, SecEnvInit, IntegrityCheckServer, EInfoServer, centralrepositorysrv,
LanguageSelection (~54k exec calls in 8 s under TCG). What made it possible:
- the hosted board (src/hosted/variant/hboard.cpp): I/O bus with device models (NVMemory), interrupts,
  driver physical RAM, physical windows (I/O, ROFS flash, RAM);
- built-in kernel modules from their own sources (htest KMODULES; a module can name its .mmp): ELOCD,
  MEDINT, medrofs, MEDNVMEMORY, mednvmem2, the ISC stand-ins, custom, sosadlserver, d_display, secldd,
  ehwa(omap), dvfs_interface, mobilecrashdriver, light ldd/pdd, PIPELIB. Each module is one partial link
  with only what other objects use left global (modules from one source keep their own code and data);
- per-process EXE data (memmodel/linux mprocess.cpp, mutils.cpp): every EXE's .data/.bss in its own
  memory file, mapped at the run address on process switch, reachable through an alias for IPC.

**The fake modem (done 2026-11-12):** the QEMU device was split into a modem core shared by QEMU and the
hosted board (src/qemu-syborg/hw/arm/syborg_modem_core.c/.h, dual licensed GPL-2.0-or-later OR EPL-1.0 by
the project) and two hosts: syborg_modem.c (QEMU: properties, MMIO, timers, NIC, migration) and
src/hosted/variant/hmodem.cpp (I/O bus windows, interrupts 15/16, a timer thread; no network or sound
yet). iscpeer.inl reaches the registers through ModemRead/ModemWrite (volatile accesses on ARM, the I/O
bus hosted). With replies the boot goes on to the System Agent and the window server (kn_efile).

**Display, variant HAL, exceptions (2026-11-12):** LCD.DLL (baseport fb) built in on a syborg-fb register
model (hboard.cpp: size from the profile, pixels in the driver's video RAM: HostedFbPixels); the test
variant answers the variant HAL as our Syborg variant (assp.cpp); hardware exceptions on nanothreads go
to the kernel (ncthrd.cpp: signal -> exception record on the thread's stack -> trampoline ->
Exc::Dispatch), so kernel-side faults on bad client descriptors return errors as on ARM. FbServ, the
window server and EComServer run (~143k exec calls at 15 s).

**RAM-loaded code and the start-up chain (2026-11-12):** RAM code segments (mcodeseg.cpp DoCreateRam: RAM
code region 0x70000000, DLL data region 0x78000000, the loader writes code and the .data template, EXE
data copied into the process area when loaded, DLL data per process as ROM DLL data; export directory,
exception descriptor); built-ins ECOMM (fixed copy: pointer/zero comparison), EUART1, EKEYB, EPOINTER.
Fixes found on the way, each pinned by kn_efile: zero-length IPC copies from address 0 (loader, -38);
TExcTrap::Exception did not leave the kernel level the exception path entered (completion callbacks never
ran: FbServ/ECom/window server deadlock); modules whose .mmp names baseport specific/ got the real
syborg.h. Start-up runs EStart, the window server, FbServ, ECom, mobile crash, C32/Etel, FeatMgr, mpm (~170k
exec calls by 60 s).

**The Nokia splash screen (2026-11-12):** with the device's own LCD.DLL build (lcd_n95 from the profile:
native-landscape panel; htest takes [kernel.lcd] and .mmp MACROs), built-in modules reloading with fresh
.data/.bss, and ROM DLL static data mapped at its fixed address, start-up runs Starter, SplashScreen,
AknIconSrv, ApsExe, EikSrvs, AknSkinSrv and the DRM servers (~810k exec calls by 60 s under TCG), and the
framebuffer shows the Nokia splash (kn_efile writes it to build/hosted/htest/vmout/<run>/kn_efile-fb.ppm).

**Next:** run on to the idle screen (Phone, SysAp, Startup; DcfRepSrv panics CDCFREPSRV 1000), a live
display window instead of a dump, the keyboard device model (keys), speed (several times slower than
the ARM QEMU board under TCG).

## Speed (2026-11-12, priority: speed, then a live display, then the home screen)

Measured by the time each process starts (kn_efile notes `started: NAME at N ms`), QEMU TCG cortex-a55:

| Change | SplashScreen starts | EikSrvs starts | exec calls in 60 s |
|---|---|---|---|
| SIGSYS trap per exec (baseline) | 16.0 s | 29 s | 0.81 M |
| direct exec calls (patched stubs) | 6.0 s | 8.7 s | 1.08 M |
| + tick suppression (IdleTime/Advance) | 5.2 s | 7.0 s | 1.07 M |
| + lone `svc` stubs patched too | 4.9 s | 5.9 s | 1.07 M |
| + EXE data copied on switch, not remapped | 4.6 s | 5.6 s | 1.07 M |
| ARM board (QEMU syborg, IPC trace build) | 3.7 s | 4.2 s | - |

Start-up is 5.2x faster than this morning (EikSrvs 29 s -> 5.6 s) and within ~1.4x of the ARM board
(intentengine 16 s vs 11.7 s). Tools: kn_efile `started: X at N ms`, `htest --env PROFILE=1` (sampling
profiler, nkern/linux/hprof.cpp: region split and hottest pcs, symbolise with llvm-addr2line on t/kn_efile),
counters in the probe (thread switches, interrupts/suppressed ticks, idle). Where the rest goes (20 s at
1.07 M exec calls): the process is on the CPU ~25% of the time; ~109k thread switches, each a futex hand-over
between Linux threads (TLxEvent Set/Wait = ~27% of the CPU samples, plus wake-up latency); the interrupt lock
~16%; ROM code ~24%. A speed advisory (2026-11-12) ranks the next steps: measure wake-up latency first,
then one CPU for the whole machine, private futexes, longer tick suppression, interrupts on the calling thread;
all nanothreads on one host thread only if hand-overs still dominate (future.md sections 1-3).

Fixed on the way: the direct path must realign the stack (EUSER calls stubs with sp 4 mod 8; clang code
assumes 8 and computed `d + 6` as `d | 6`, the modem's IMEI reply came out shifted and the DRM server waited
for ever); ARM11 CLZ encodings emulated (AknIconServer). Next measures: process switches (EXE data remap,
two mmap per switch), thread hand-over cost, then the display.

## The home screen (2026-11-12)

The unmodified N95 user side reaches its idle (home) screen on the hosted kernel within ~20 s under TCG:
active standby with clock and date, shortcut bar, softkeys. The last blocker was the start-up tone: Startup
waits for it to finish, and the N95 plays it through the ehwa stand-in into syborg-snd, which the hosted
board now models as a sink without output (variant/hsnd.cpp: the QEMU device's registers, rings consumed at
the playback rate, watermark/EOS interrupts); sybsnd.h's register accessors go through the I/O bus when
hosted (they dereferenced the registers directly).

## Live display (2026-11-12)

`python3 tools/hosted/phone.py` boots the N95 on the hosted kernel in the test VM and shows its screen in a
window (about 25 s to the home screen under TCG). The VM gets an ivshmem device whose memory is a host file
(build/hosted/htest/phone.shm); variant/hdisplay.cpp copies the changed framebuffer rows into it and bumps a
frame counter, the viewer maps the same file. Closing the window (or Ctrl-C) stops the VM. Keys go the other
way: the window writes scan codes (the device keymap, the sydb phone window's host keys: arrows, Enter = OK,
F5/F7 = soft keys, digits, Backspace = C) into a ring in the same memory, the VM feeds them to a syborg-keyboard
model that the unmodified EKEYB reads (hkeyb.cpp); on the home screen "1" opens the dialer. A start-up stall at the start-up tone in about one boot of three was a deadlock:
the sound ISR reading syborg-snd while the interrupted nanothread held the device lock (fixed, kn_devlock).

**After the home screen (2026-11-12, phone window):** the first home screen is drawn at ~20 s, then the phone
stays busy until ~63 s: efile and its local-drive thread with sisregistryserv (the boot-time drive and SIS
registry scans), no timeout. In a 60 s boot only ~14 s are CPU time of the process and ~4 s idle: the rest is
thread hand-overs (~475k, 22 us each under TCG in nk_basic's ping-pong, plus the waits between them) and
interrupt round trips. One vCPU (`--smp 1`) changed nothing, so it is the switch cost itself: the
single-host-thread design (future.md section 3) is the fix for this phase, and native hardware cuts the
per-switch cost by an order of magnitude [inferred]. The null thread spun at 100% after boot (fixed: idle =
WFI in NKIdle; now ~14% at rest, mostly the Supervisor thread, the display copy and lxtop).

