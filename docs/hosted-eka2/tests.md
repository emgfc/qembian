# Hosted EKA2: the test machine and the kernel test suite (htest)

State 2026-11-12: built and running. All in QEMU; no phone hardware yet (P0 on the Pixel 3a is
postponed). Everything here is ours: no firmware, no proprietary binaries.

## Pieces

| Piece | Where | Built by |
|---|---|---|
| `qemu-system-aarch64` (stock QEMU 11.1.3, same pinned tarball as the syborg build, no syborg board) | `build/hosted/qemu/install/bin/` | `scripts/build-hosted-qemu.sh` |
| arm64 guest kernel: Linux 6.18.63 longterm (pinned in `sources.lock`), `defconfig` + `src/hosted/linux/hosted.config` (COMPAT, SWP/CP15/SETEND emulation, seccomp, memfd, 9p, virtio-gpu/input/snd), no modules | `build/hosted/linux/arch/arm64/boot/Image` | `scripts/build-hosted-kernel.sh` (~5 min on 16 cores) |
| **hrt**: freestanding runtime for 32-bit ARM Linux processes (raw EABI syscalls, raw clone threads, futex, signals with the ARM compat `ucontext`, seccomp trap-by-address filter, printf, test checks) | `src/hosted/rt/` | `tools/hosted/htest.py` |
| test programs (32-bit ARM, `t_*.c`, one process each) | `src/hosted/tests/` | same |
| aarch64 `/init` of the test guest (mounts, sets `abi.swp=1`, `abi.cp15_barrier`, runs `/t/*` with a time limit, prints `HTEST-*` lines, powers off) | `src/hosted/init/init.c` | same (static, the cross gcc's glibc) |

Toolchain: `clang --target=armv7a-linux-gnueabihf -march=armv7ve -marm -mfloat-abi=soft` for the 32-bit code,
linked by the aarch64 binutils' `ld -m armelf_linux_eabi` (no armhf cross toolchain or libc needed);
`aarch64-suse-linux-gcc` (or `aarch64-linux-gnu-gcc`) for the kernel and init.

## Running

```
scripts/build-hosted-qemu.sh && scripts/build-hosted-kernel.sh     # once
python3 tools/hosted/htest.py                  # build, boot, run all tests (~3 s), exit 1 on a failure
python3 tools/hosted/htest.py t_svc t_tls      # a subset
python3 tools/hosted/htest.py -v               # with the kernel console
python3 tools/hosted/htest.py --cpu max --cp15 2 --smp 1
python3 tools/hosted/htest.py -j 4             # split over 4 QEMUs in parallel
python3 tools/hosted/htest.py --matrix -j 3    # 1/2/4 CPUs and -cpu max, all in parallel (~6 s); a table
```
Run `--matrix` before committing kernel changes: the single-CPU deadlock of 2026-11-12 only showed
with `--smp 1`. Per-run consoles: `build/hosted/htest/console-<cpu>-smp<N>-<shard>.log`.
Consoles: `build/hosted/htest/console-<cpu>-smp<N>-<tests>.log` (one per run). Machine: `-M virt,gic-version=3 -cpu cortex-a55 -smp 4 -m 1024`,
initramfs only, serial console, no display yet.

Kinds of tests: `t_*.c` / `t_*.cpp` link the runtime only; `nk_*.cpp` also link the hosted nanokernel
(docs/hosted-eka2/nanokernel.md). Every fixed bug gets its own regression test (CLAUDE.md rule).

Writing a test: `src/hosted/tests/t_NAME.c` with `int main()`, checks with `HCHECK(cond, fmt, ...)`,
measurements with `htest_note(...)`, `return htest_done();`. A test is one 32-bit process; it may fork,
make threads (`hthread`), install signal handlers and seccomp filters. Exit code 0 = pass.

## What the tests established (QEMU TCG, cortex-a55, Linux 6.18) [verified]

| Test | Result |
|---|---|
| `t_hello` | the runtime: argv, syscalls, 64-bit division helper, printf. Linux puts a 32-bit process's stack at ~0xffa00000-0xffb00000 |
| `t_map` | every EKA2 region maps at its fixed address in a 32-bit process: user local data 0x00400000, 2 GB-model shared data 0x40000000-0x80000000, **ROM 0x80000000**, **global data 0x90000000**, RAM drive 0xA0000000-0xC0000000, **kernel data 0xC8000000**, flexible super page 0xF0000000 |
| `t_shared` | a memfd mapped at 0xC8000000 in two processes is one memory (the kernel-data model), a futex in it wakes across processes, `process_vm_readv` reads the other process's copy at the same address (IPC descriptor copies) |
| `t_svc` | a seccomp filter on the instruction pointer turns every `svc` in the stub range into SIGSYS **whatever r7 holds** (exit, exit_group, the ARM set_tls 0xf0005, -1 checked), svcs elsewhere still reach Linux, a Linux syscall inside the handler works, r0-r3 in and r0/r1 out through the `ucontext`. Cost under TCG: SIGSYS round trip ~34 us, getpid ~9 us, direct call ~20 ns: the stub-patching plan (architecture.md) stands |
| `t_swp` | SWP/SWPB give the right results, CP15 DMB/DSB/ISB do not fault (abi.cp15_barrier=1), LDREX/STREX work. **QEMU executes SWP itself** in AArch32 user mode (no SIGILL even with `abi.swp=0`), so Linux's SWP emulation is *not* exercised here: it must be measured on the phone (P0) |
| `t_thread` | raw clone threads + futex baton: 4 threads, one at a time, 2000 hand-overs, no overlap (the Win32-emulator scheduling model with futexes). ~17 us per hand-over under TCG |
| `t_tls` | TPIDRURO (c13,c0,3) reads what ARM `set_tls` (0xf0005) set, TPIDRURW (c13,c0,2) is user-writable, both per thread across switches, both visible in a signal handler. The ARM `get_tls` call (0xf0006) does not exist on arm64 compat (ENOSYS). A handler's TPIDRURW change survives sigreturn (Linux does not restore it): the hosted kernel must save/restore it itself if a handler touches it; a new host thread starts with TPIDRURO 0, not its creator's (regression 2026-11-12: the display thread passed for a nanothread and hung on the first key) |

| `t_time` | the ARM generic timer (CNTVCT, CNTFRQ 62.5 MHz on `virt`) is readable from 32-bit user mode, monotonic, agrees with CLOCK_MONOTONIC; ~570 ns per read vs ~9 us for `clock_gettime` under TCG |
| `t_unaligned` | unaligned LDR/LDRH/STR are true unaligned accesses (ARMv6 U=1 / ARMv7 semantics, what the N95's ARM1136 EKA2 setup gives); unaligned LDRD and LDM are fixed up by the kernel (no SIGBUS); VFP usable at EL0. ARMv5 rotated loads (N73) are **not** reproduced |
| `t_ctors` | static constructors run before `main` (regression) |
| `nk_basic`, `nk_lxsuspend`, `nk_idle`, `nk_kill` | the hosted nanokernel: see nanokernel.md |
| `nk_exc` | hardware exceptions on a nanothread reach its exception handler: an ARM and a Thumb load from 0x140, the record's signal/address/registers, the handler's r0 and pc changes take effect, r4-r7, flags and Thumb state survive, the kernel is unlocked afterwards |
| `kn_board` | the hosted board without a ROM: the I/O bus (unmodelled pages are plain memory), the peripheral window (linear 0xC6000000 = physical 0xC0000000), driver physical RAM (alignment, exhaustion, zero size), the NVMemory model with syborg_nvmem.c semantics (registers at `base + (reg << 2)`, transfers, status = sectors or -1, interrupt raised by EXECUTE and lowered by reading STATUS, past-the-end / oversized / unknown direction / zero size / unmapped buffer all -1) and the drive image never written back (private mapping); the syborg-snd sink (rate, watermark, EOS, invalid ring); the syborg-keyboard model (ID, empty FIFO reads -1, interrupt only when enabled and non-empty, order, overflow drops at 64) and the phone window's key ring into it (pressed/released entries; regression: the first ring API returned -1 for empty and pressed entries have bit 31 set) |
| `kn_modem` | the fake modem on the hosted board, no ROM, no LxInit (a plain host thread): reply table parsing (comments, blank/CRLF/indented lines, short lines skipped, lengths capped at 64), ID register, request -> reply framing (header, tid echo, payload), unknown request -> log frame, invalid handle/channel refused, no interrupt for frames of the guest's own send, RX_POP on an empty queue, channel close purges queued replies, SMS input errors |
| `kn_devlock` | regression (2026-11-12, start-up hung at the start-up tone): a nanothread drives syborg-snd 20000 times (START on an invalid ring raises the interrupt at once, register reads in between) while the sound ISR reads and acknowledges it; with the device lock taken only under the kernel lock the ISR spun on the lock its interrupted thread held (watchdog) |
| `kn_efile` (`--rom`) | the phone's user side: efile.exe mounts Z: (ROFS window), C: and E: (seed images), starts EStart.exe (a second EXE with .data at the same 0x00400000: per-process data swapped on process switch), the ISC stand-ins load as built-in modules, the fake modem (shared core) answers, start-up goes on to the System Agent and the window server (probe at 15 s; processes recorded by a kernel event hook, __DEBUGGER_SUPPORT__ as on ARM) |

Not covered by QEMU and left for the phone (P0): SWP emulation cost, CP15 barrier mode 2 (hardware),
real exec/trap costs, the SoC's 32-bit EL0 support. The `virt` machine answers correctness questions only.
