# Hosted EKA2: the nanokernel on Linux (P1)

State 2026-11-12: the **unmodified** EKA2 nanokernel runs as a 32-bit ARM Linux process on the QEMU
aarch64 `virt` machine, passing `nk_basic`, `nk_lxsuspend`, `nk_idle` and `nk_kill` on 1, 2 and 4 CPUs
(htest `--matrix`); Symbian's own standalone suite `nkernsa` builds and runs as `nk_sa` (below).

## What is compiled

| Part | Source | Lines |
|---|---|---|
| generic nanokernel, unchanged | EPL tree `kernel/eka/nkern/{nkern,nklib,nkerns,sched,dfcs,nk_timer}.cpp` | ~3,900 |
| Linux platform layer (replaces `nkern/win32`) | `src/hosted/nkern/linux/`: `nk_plat.h`, `ncsched.cpp`, `ncthrd.cpp`, `ncutils.cpp`, `ncglob.cpp` (ported from the Win32 files, EPL notices kept), `lxhost.cpp` (host primitives), `atomics.cpp` (`e32atomics.h` on compiler atomics), `nkhost.cpp` (KPrintf, fault hooks, heap, system timer, watchdog), `hosted_prefix.h` | ~1,500 |
| runtime | `src/hosted/rt` (hrt: raw syscalls, clone threads, futex, signals, static constructors) | ~400 |

Compiler: clang `--target=armv7a-linux-gnueabihf -march=armv7ve -marm -mfloat-abi=soft`, the EKA2 headers
with `gcce/gcce.h`, `-D__EPOC32__ -D__GCCE__ -D__KERNEL_MODE__ -D__HOSTED__` and no `__CPU_ARM`: the
nanokernel takes its portable C++ paths (no `.cia`). Flags: `tools/hosted/htest.py kernel_flags()`.

## Model (same as Symbian's Win32 emulator kernel)

- Every nanothread is a host thread (raw `clone`, 128 KB stack). Thread records live in never-freed
  pages (the registry the dump and reaper walk); a dead thread's stack is unmapped by the next thread
  creation once Linux has cleared its CLONE_CHILD_CLEARTID word (`nk_kill` checks 51 threads leave the
  live-stack count unchanged). It runs only while the EKA2 scheduler has
  picked it; otherwise it sleeps on its **schedule lock** (an auto-reset futex event, `TLxEvent`).
  A context switch = set the next thread's event, wait on your own (`SwitchThreads`).
- **Interrupts** are host threads that are not nanothreads (`CreateLinuxThread(EThreadEvent, ...)`).
  `StartOfInterrupt()` takes the interrupt lock (`LinuxInterrupt::Mask`) and **suspends** the current
  nanothread; `EndOfInterrupt()` either resumes it or preempts it (marks `EResume` and wakes the
  scheduler thread, which picks the next thread). The system timer is such a thread: 1 ms ticks into
  `NTimerQ::Tick` (timeslices, timers, `NKern::Sleep`).
- **Suspend** (Win32 `SuspendThread`): `LxSuspend` sends SIGUSR2; the handler parks the thread on a
  sequence futex and keeps its `ucontext`; `LxResume` releases it. The saved context is what the
  **diversion** edits (`ApplyDiversion`: pc -> `DivertHook`, an ARM routine that saves all registers,
  runs `NThread::Diverted`, and returns to the interrupted pc, Thumb-aware): forced exit, user-mode
  callbacks.
- The current host thread's record (`SLxThread`) is found through **TPIDRURO** (c13,c0,3, set with the ARM
  `set_tls` call); TPIDRURW is left to the Symbian user side (ARMv6K+ EUSERs keep their thread data
  there). Raw host threads (`hthread`) start with TPIDRURO 0, so `LxSelf()` is NULL on them (t_tls).
  (Until 2026-11-12 the record was in TPIDRURW.)
- Disabling interrupts = taking the interrupt lock (`NKern::DisableAllInterrupts`), as in Win32.
- **Idle**: when no thread is ready (the initial thread may block, as on ARM), `Reschedule` waits on
  an idle futex that every `EndOfInterrupt` pokes, running pending IDFCs, until a thread is ready.
  (The Win32 scheduler asserts there: its null thread never blocks.)
- **Time**: `hnow_ns` and `NKern::FastCounter` read the ARM generic timer (CNTVCT) directly from user
  mode, ~16x cheaper than `clock_gettime` under TCG and a few ns on hardware.

## Debugging

- A crash in any host thread prints `HOSTED FAULT` with registers and stack words; symbolise with
  `llvm-addr2line -f -C -i -e build/hosted/htest/t/<test> <pc> <lr>`.
- Every host thread has an alternate signal stack: a stack overflow is reported (`STACK OVERFLOW
  (guard page)` and the frame chain of the faulting code) instead of a silent SIGSEGV.
- An `NKFault` prints a frame-pointer backtrace (everything is built `-fno-omit-frame-pointer`).
- On a test timeout htest's init sends SIGQUIT: the process prints `HOSTED SIGQUIT` and the dump
  below for every host thread before it is killed.
- `HostedStartWatchdog(seconds)` (nkhost.cpp): if the test's progress counter stops, every host thread
  prints its pc/lr/suspend state and the scheduler state (`HOSTED DUMP`), then the test exits. That found
  the first deadlock in one run.

## Bugs found so far (each has a regression test)

| Bug | Symptom | Test |
|---|---|---|
| hrt `_start` did not run static constructors | `TheScheduler` zero, first tick crashed in `SDblQue::Add` | `t_ctors` |
| suspend handshake slept on the count itself | resume + re-suspend before the parked thread ran: both sides waited forever (one CPU only) | `nk_lxsuspend` (run `--matrix`) |
| no idle path: every thread blocked -> `SelectThread` assert | nkernsa's initial thread sleeps in `init_fast_counter` | `nk_idle` |
| an interrupt suspended a thread that was exiting (it had just handed over); no ack ever came | `nk_kill` hung once on 2 CPUs | `nk_exitrace` (interrupt storm vs 400 exits/kills) |
| `NKFault` called `CurrentContext()`, which faults again in a bad state | endless recursion, silent stack overflow | `nk_fault` |
| resume into a kill diversion asserted suspend count == 1 (Win32 too); an interrupt may suspend the new current thread first | NKFault under the storm on one CPU | `nk_exitrace` |
| `EndOfInterrupt` released the interrupt lock before making the scheduler thread current (Win32 order); a second interrupt thread adopted the preempted thread and then saw the current thread change | assert `pC==current` once in ~12 matrix runs | `nk_exitrace` |

## The first Nokia code (`nk_rom`)

`htest --rom DEVICE` (default the N95) shares the device's composed core ROM read-only over 9p; only
addresses (EUSER's export directory, its exec stub block from romscan) go into the initramfs. `nk_rom`
loads the ROM at 0x80000000 (read-only, as on the phone) and calls EUSER exports by ordinal from a
nanothread: `User::StringLength`, `Mem::Compare` run as they are (Thumb code); `User::TickCount`,
`NTickCount`, `FastCounter` go through EUSER's own ARM stubs into the trap and `Dispatch` and their
results come back through EUSER. Without a ROM the test is SKIPped.

## Exec calls (P2 start)

`src/hosted/nkern/linux/vectors.cpp`: `Dispatch` (from the Win32 version: fast exec 0 =
WaitForAnyRequest, fast table under the kernel lock, slow table with claim/release of the system lock
and preprocessing, invalid numbers to the invalid handler) and `HostedExecTrapInit(lo, hi)`, which
routes every `svc` in the ROM stub range to it through SIGSYS (seccomp on the instruction pointer;
the handler runs on the thread's stack, r0-r3 from the signal context, r0 back).
`nk_exec` checks all of it, under round robin too. Cost under TCG: ~50 us per exec through the trap,
~0.6 us as a direct `Dispatch` call (80x): patching the EUSER stubs into direct calls is the plan, the
trap stays as the safety net. Win32's `Dispatch` did not leave the kernel on an invalid exec number;
ours does (checked).

## nkernsa (Symbian's standalone nanokernel suite) = `nk_sa`

All of `kerneltest/e32test/nkernsa/*.cpp` unchanged except `nirqx.cpp` (SMP interrupt controller) and
`tlsf.cpp` (its heap is set up from a ROM super page; ours is used), plus
`src/hosted/nkern/nksa/nksa_hosted.cpp` in place of `arm/armutils.cpp/.cia` (fast counter, timer
period, `RequestComplete`, a zeroed stand-in ROM header for its trace mask). It starts with
`BenchmarkTests()`, which time loops for about a second each: slow under TCG (`SLOW` in htest.py).

## Next (P1 rest, then P2)

1. Make `nk_sa` pass completely (results above), then add it to the regular matrix.
2. `Stillborn` (thread creation aborted) test.
3. Exceptions from signals into the nanokernel's exception handlers: done 2026-11-12 (ncthrd.cpp HostedNThreadException + HostedExcTrampoline, nk_exc).
4. P2: exec dispatch, ROM mapping, the first ROM-resident exe.

## The N95 ROM, statically (tools/hosted/romscan.py, 2026-11-12) [verified]

`SYBORG_DEVICE=nokia-n95-8gb-rm320 python3 tools/hosted/romscan.py [--module EUser.dll] [--json F]`
on the composed core ROM (RM-320 35.0.001, base 0x80000000, 2,712 modules):
- **EUSER's 262 exec stubs (28 fast, 234 slow) are one contiguous block, 0x803a0348..0x803a0d28**
  (`svc #n; bx lr`, 8 bytes each, numbers in order). The seccomp trap range is that block; the stub
  patcher has a single, regular table to rewrite.
- 25 other modules contain 1-7 words shaped like a stub (`huitk.dll` 7, others 1): private exec calls
  or data; to be checked before patching (the trap range covers only EUSER's block, so a real one
  elsewhere would reach Linux as a syscall: P2 must decide with a wider filter or patching).
- **EUSER has no SWP/SWPB** (it has LDREX/STREX: an ARMv6 build). The SWP-emulation cost worry of
  architecture.md/roadmap.md was based on the EPL sources (ARMv5 `uc_exec.cia`) and does not hold for
  the N95's EUSER. Whole ROM: at most 535 SWP and 84 SWPB sites (upper bounds, data words included),
  mostly in apps (`hmidps60v31.exe` 56, `hdmetafile2.dll` 47); `efile.exe` 2, `EwSrv.exe` 3.
- EUSER has no CP15 barrier MCRs (our EKERN replaces the phone's, which has 70).

## The first user process: Nokia's efile.exe (`kn_efile`, 2026-11-12)

The EPL `EXSTART` extension (unchanged; built into the hosted kernel with renamed entry points, its
static constructors run when the kernel starts it) creates the ROM's secondary process: the N95's own
`efile.exe`. Its code segment comes from the ROM image, its `.data`/`.bss` area is created at its fixed
run address 0x00400000, its first thread enters the ROM entry point the ARM way (creation info block on
the stack, r4 = entry reason; `HostedEnterUser`) and runs EUSER's start-up natively; every exec call
from EUSER's stub block is trapped into the kernel's exec tables in the device's personality (sym92:
kernel config bits from the ROM header + the profile's personality). It gets through **~3400 exec
calls**: heap and chunk set-up, mutexes and semaphores, the loader server and its IPC (sessions,
`MessageIpcCopy` both ways, security info), `User::Leave`/TRAP as C++ exceptions, DLL TLS, timers, the
ROM root directory of the phone's hardware variant. Then the loader is asked for `ELOCD.LDD`, a
kernel-side driver: the phone's copy is prelinked to the phone's EKERN, so the hosted kernel needs
**built-in kernel modules** (EPL elocd + our media stand-ins compiled hosted, picked by name when the
loader asks), as EXSTART already is. That is the next step; `kn_efile` pins everything up to there.

Bugs fixed on the way, each covered by `kn_boot`/`kn_efile`/`nk_lxinit` checks: EXSTART's global DFC
built before the kernel existed; the user entry called Win32-style instead of the ARM register/stack
convention; TPIDRURO not zero at process start (now our record's register: TPIDRURW is the user side's);
the hosted image's `.bss` over 0x00400000 (now linked at 0xD0000000); the EXE data area never created;
`__SUPPORT_CPP_EXCEPTIONS__` missing (Leave = invalid exec); the root directory *list* returned as the
root directory; the kernel's capabilities with undefined bits (every LDD refused); kernel `new` from the
test runtime's heap instead of the kernel heap; the exec personality not selected (bootstrap duties:
config flags, active hardware variant, root directory, now done by `HostedBootEpoc`).

Tools: `HostedTraceExecs = N` prints the first N trapped exec calls; `tools/hosted/execnames.py LOG
[--errors]` names them from the personality's exec map; `EKernelConfigPlatSecDiagnostics` prints
failed capability checks; `HostedFaultHook` lets a test judge a kernel fault.

## The kernel layer boots (`kn_boot`, 2026-11-12)

The whole Symbian kernel links and boots on the hosted platform: the 32 generic kernel files, `klib`,
`common` (all unchanged EPL sources, with our personality patches as on ARM) + `src/hosted/memmodel/linux`
(the emulator memory model ported: `lxwin32.h` maps its VirtualAlloc/Free/Query onto mmap/mprotect;
`mcodeseg.cpp` rewritten for ROM XIP images) + `src/hosted/kernel/linux/kplat.cpp` (the Win32 kernel
platform ported: boot, ASIC glue, exception traps as an ARM setjmp, safe copies, exec tables, threads,
exceptions, RAM drive, 9.1/9.2 personality shims) + a test variant (`src/hosted/variant`). `kn_boot`
maps the N95 core ROM, boots with KBOOT/KPROC/KDLL/KTHREAD tracing: variant, microkernel, kernel heap,
the EKERN process and code segment, object containers, message chunk, power model, the Supervisor,
DfcThread0/1, DShBufThread, RAM drive, TimerThread, system time from the host, then the extensions; a
test extension checks the kernel thread, heap and tick there. Class names of the emulator model are kept
(`DWin32Process`...): kernel headers befriend them by name.

Bugs on the way (regression: `kn_boot` itself): the first trace line comes before the variant exists
(early output now goes straight to the console); EKERN's UIDs came from a PE image on Win32 (now set).

## The kernel layer (P2/P3): compile status 2026-11-12

The generic Symbian kernel (`kernel/eka/kernel/s*.cpp` and friends, 32 files, ~40k lines, unchanged)
compiled for the hosted target: **26 of 32 compile** with the nanokernel's flags plus `_UNICODE` and
three thin header overrides in `src/hosted/include/` (EPL tree untouched; `#include_next`):
`u32exec.h` (takes the ARM branch: swi stubs are valid hosted code), `kernel/execs.h` and `execs.h`
(take the WINS branch: exec tables as plain C arrays; its preprocess handler is named
`Win32PreprocessHandler`). The nanokernel's interrupt-lock object is now `TheInterrupt`, freeing the
name `Interrupt` for the driver-facing class of `assp.h`.

The 6 that do not compile all need the **memory model**: `device.cpp`, `scodeseg.cpp` (`Epoc::RomHeader`,
`TRomHeader`), `sbtrace.cpp`, `sipc.cpp` (`memmodel.h`), `srandombuff.cpp` (`class Interrupt` from a
hosted `assp.h`), `sinit.cpp` (`_UNICODE`, fixed). The Win32 platform to port: `kernel/win32` (1.4k
lines: boot, exec glue, scheduler hooks, utils) and `memmodel/emul/win32` (4.2k lines: process, chunk,
code segment, shared buffers) plus their headers (`memmodel.h` 313 lines).

Memory model design for the N95 (multiple model semantics, see architecture.md): a `DLinuxProcess` per
Linux process; chunks are memfd mappings at the multiple model's fixed addresses (process-local data at
0x00400000+, global 0x90000000+, ROM 0x80000000 read-only, kernel data 0xC8000000 shared); code segments
are ROM images (XIP) first, RAM-loaded later; IPC copies through `process_vm_readv/writev`. First
step: one process (the kernel's own), so that `DThread`/`DProcess`/handles/IPC run inside one Linux
process before the split.
