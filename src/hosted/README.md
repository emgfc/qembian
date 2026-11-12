# Hosted EKA2: the Symbian kernel as a Linux process

The real EKA2 kernel (the EPL sources, unchanged where possible) runs as an ordinary **32-bit ARM Linux
process**. The phone's **unmodified ROM code** (EUSER, the servers, the S60 UI, the apps) runs inside it
natively as AArch32 user code; only exec calls, interrupts and devices are rerouted. There is no
instruction emulation of Symbian code: the CPU runs it directly.

Status (2026-11-12): the Nokia N95 8GB user side (S60 3rd FP1, Symbian 9.2) boots to its home screen in
the test VM (QEMU aarch64 `virt`, TCG) in about 20-25 s, with a live screen and keypad in a window:

    python3 tools/hosted/phone.py                       # boot the N95, show it, keys work; close = stop
                                                        # Ctrl+T: Linux `top` beside it (Symbian threads = Linux threads)
    python3 tools/hosted/htest.py                       # all hosted tests (docs/hosted-eka2/tests.md)
    python3 tools/hosted/htest.py --rom nokia-n95-8gb-rm320 --env PROBE_MS=4e20 kn_efile   # headless boot, probe at 20 s

Further reading: `docs/hosted-eka2/` (the evaluation and the original design: `architecture.md`; the
nanokernel layer: `nanokernel.md`; phases and measurements: `roadmap.md`; tests: `tests.md`; the future
designs: `future.md`). Every shortcut, hardcoded value and guess: `docs/hacks.md`, section "Hosted EKA2".

## The picture

```
 host (PC: x86-64 or arm64 Linux)                         later: the phone itself (Pixel 3a, mainline Linux)
 ┌────────────────────────────────────────────────────────────────────────────────────────────────┐
 │ tools/hosted/phone.py (PyQt6 window)  ◄── mmap ──  build/hosted/htest/phone.shm (1 MB file)     │
 │     draws frames, writes key events ──────────────►   header | key ring | XRGB pixels           │
 │ tools/hosted/htest.py: builds everything, runs QEMU                    ▲                        │
 └────────────────────────────────────────────────────────────────────────┼────────────────────────┘
                                                                          │ ivshmem BAR (PCI 00:05.0)
 QEMU aarch64 `virt` (TCG today; KVM on an arm64 host with AArch32 EL0)    │
 ┌────────────────────────────────────────────────────────────────────────┼────────────────────────┐
 │ Linux 6.18 arm64 (COMPAT: 32-bit processes), busybox-free init (src/hosted/init)                │
 │ ┌──────────────────────────────────────────────────────────────────────┼─────────────────────┐  │
 │ │ ONE 32-bit ARM Linux process = the whole Symbian machine            │                     │  │
 │ │                                                                      │                     │  │
 │ │  0x00400000  live EXE data window (copied in/out on process switch)  │                     │  │
 │ │  0x70000000  RAM-loaded code (ROFS/C: DLLs, EXEs)    0x78000000 DLL static data            │  │
 │ │  0x7ff00000  exec trampolines (EUSER's svc stubs patched to jump here)                     │  │
 │ │  0x80000000  the phone's core ROM (XIP, read-only, mapped from the composed image)         │  │
 │ │  0xC6000000  I/O window: device models (hboard: nvmem, fb, keyboard, sound, modem)  ──────┘  │
 │ │  0xC8000000  kernel data/heap         0xD0000000  the hosted kernel's own image             │  │
 │ │                                                                                             │  │
 │ │  host threads:                                                                              │  │
 │ │   nanothread x N ─ one Linux thread per Symbian thread; exactly one runs at a time; a       │  │
 │ │                     switch = futex hand-over (wake next, wait on own)                       │  │
 │ │   scheduler thread, tick thread (1 kHz, suppressed when idle), IRQ thread (device lines)    │  │
 │ │   helpers: sound pump, modem timers, display (copies rows, drains the key ring)             │  │
 │ │                                                                                             │  │
 │ │  user code ── bl stub ── trampoline ── HostedExecDirect ── EKA2 exec tables (personality)    │  │
 │ │  device access ── HostedIoRead/Write ── device model ── HostedRaiseIrq ── IRQ thread ──      │  │
 │ │      StartOfInterrupt (suspend running nanothread: SIGUSR2) ── ISR ── DFC ── EndOfInterrupt │  │
 │ │  CPU exception ── signal ── exception record ── trampoline ── Exc::Dispatch (panics, traps)  │  │
 │ └─────────────────────────────────────────────────────────────────────────────────────────────┘  │
 └──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

## What is where

| Directory | What | Origin |
|---|---|---|
| `rt/` | freestanding runtime: raw syscalls, clone threads, futexes, signals, `_start`, static constructors | original |
| `nkern/linux/` | the nanokernel's platform layer: scheduler hand-over, interrupt lock, suspend/resume by signal, tick thread, exec dispatch and stub patching (`vectors.cpp`), exceptions, ARM11 instruction emulation, profiler, dumps and watchdog | ported from Symbian's `nkern/win32` (EPL notices kept) + original |
| `nkern/nksa/` | glue for Symbian's standalone nanokernel suite (`nk_sa`) | original |
| `kernel/linux/` | the kernel's platform: boot (`HostedBootEpoc`), exception traps, safe copies, exec tables, personality shims, built-in kernel modules (`kmodules.cpp`) | ported from `kernel/win32` + original |
| `memmodel/linux/` | the memory model: chunks over mmap, processes, EXE/DLL static data, RAM code segments, IPC copies, physical windows | ported from `memmodel/emul/win32` |
| `variant/` | the hosted board: I/O bus, interrupts, NVMemory, framebuffer, keyboard, sound sink, fake modem host, display shared memory, the test variant | original (modem core shared with QEMU) |
| `include/` | thin header overrides (`#include_next`): exec tables, `syborg.h` register access through the bus, `mmboot.h` | original |
| `init/` | the test VM's PID 1: mounts the 9p shares, runs tests, prints `HTEST` lines | original |
| `linux/hosted.config` | the test VM kernel's extra config on top of arm64 defconfig (COMPAT, SWP/CP15 emulation, seccomp, memfd, 9p; the ivshmem BAR comes through defconfig PCI sysfs) | original |
| `tests/` | `t_*` runtime, `nk_*` nanokernel, `kn_*` kernel/board/phone tests | original |

Unchanged EPL sources compiled in: `nkern/*.cpp`, the 32 generic `kernel/*.cpp`, `klib`, `common`, EXSTART,
ELOCD, ECOMM, EUART, EKEYB, EPOINTER, PIPELIB, our baseport drivers and stand-ins (`src/standins`) as
built-in modules (`htest.py` KMODULES, one partial link each).

## Choices made, and why

1. **Hosted, not emulated, not virtualised.** The Win32 emulator kernel proved EKA2 can run as a host
   process (~8k lines of glue around ~44k lines of portable kernel). Hosting gives native ROM-code speed
   on a phone where KVM is impossible (the Pixel 3a's Linux runs at EL1 under Qualcomm's hypervisor,
   no EL2) and a full board port would be years (docs/hosted-eka2/architecture.md).
2. **Old phones first: 32-bit ARM execution required.** The ROM code is ARMv5/ARMv6 (N95: ARM1136). We
   run it as AArch32 EL0 code, so the host CPU must still execute 32-bit ARM: Cortex-A53/A55/A57/A72/A73/
   A75/A76 class (2015-2021 phones, the Pixel 3a's SDM670, Raspberry Pi 3/4/5) and the QEMU test VM.
   CPUs without AArch32 (Pixel 7+, Cortex-X3/A715+, Apple) cannot run it: that needs the JIT/AOT route
   (`docs/hosted-eka2/future.md`). The first target firmware is the N95 (framebuffer UI, no GPU
   compositing, the best-tested phone of the project).
3. **One Linux process for the whole Symbian machine (for now).** All Symbian processes share one
   address space, as on Win32; process-local data (EXE `.data/.bss` at 0x00400000, DLL static data) is
   copied in and out on a process switch (the moving model done with memcpy). The planned design is one
   Linux process per Symbian process with kernel state in shared memory (future.md); one process got us
   to a home screen much sooner.
4. **One host thread per nanothread, one running at a time.** The EKA2 scheduler keeps full authority;
   a switch is a futex hand-over. Simple, debuggable (gdb sees every Symbian thread), the Win32 model.
   Cost: ~109k hand-overs in a 20 s boot (`roadmap.md` speed section).
5. **Futexes are process-shared (no `FUTEX_PRIVATE_FLAG`)** although everything is one process today:
   the scheduler lock, fast mutexes and thread events must keep working when Symbian processes become
   separate Linux processes sharing the kernel's memory, and when the SMP kernel runs nanothreads on
   several CPUs at once (future.md). Shared futexes take the slower key path in Linux (an mm lookup per
   call; [inferred] 10-30% per futex call). Switching to private ones is a one-line change if a
   measurement ever says it matters before the split.
6. **Exec calls are direct calls.** EUSER's `svc #n; bx lr` stubs are patched at load into branches to
   trampolines that call the kernel's exec dispatcher on the caller's stack (realigned to 8 bytes). The
   seccomp SIGSYS trap stays as the safety net for unpatched `svc`s. 80x cheaper than the trap.
7. **Devices are register-level models of our QEMU syborg devices.** The phone's drivers and our
   stand-ins compile unchanged (`syborg.h` `ReadReg/WriteReg` go through the I/O bus when hosted); the
   fake modem is one core shared with the QEMU device (`syborg_modem_core.c`, dual licensed by the
   project). The same driver code therefore runs on the ARM board and hosted.
8. **Kernel-side ROM binaries are replaced by hosted builds of the same sources** (picked by file name
   when the loader asks): the phone's own kernel DLLs are prelinked to its EKERN and cannot run here.
9. **The display leaves the VM through shared memory** (ivshmem), as the QEMU guest's framebuffer did:
   no copies through a socket, the window maps the same file.
10. **Correctness over speed while bringing up**: every hosted bug gets a regression test (`tests/`),
    every shortcut a line in `docs/hacks.md`.

## Ways to make it better

Ranked by the speed advisory of 2026-11-12 (details and measurements to take first: `future.md`):

1. **Measure where the 75% off-CPU time goes** (wake-up latency vs the phone's own timers), then: all
   Symbian threads on one CPU (`--smp 1`, CPU affinity), private futexes while single-process, longer
   tick suppression / tickless idle, device interrupts run on the calling thread.
2. **All nanothreads on one host thread** with user-level context switches (the ARM kernel's own model):
   no futex hand-overs, no suspend signals. 5-10 days; only if hand-overs still dominate after (1).
3. **One Linux process per Symbian process**: real isolation, no static-data copying on process switch,
   kernel data in a shared memfd (the reason for shared futexes).
4. **SMP**: the `nkernsmp` nanokernel with one host thread per virtual CPU (future.md).
5. **The Pixel 3a**: the same process on mainline Linux, DRM/KMS display, evdev keys, ALSA sound.
6. **Phones without AArch32 (arm64-only) and x86-64**: a translator for the ROM's ARM/Thumb code, either
   a JIT (dynarmic-style) or ahead-of-time recompilation of the ROM and ROFS images into native code
   modules before boot, with the JIT only for code that appears at run time. Design sketch in future.md.
7. **GPU phones** (N8, 701, 808): the host EGL/GLES path of the QEMU board re-hosted in-process.
