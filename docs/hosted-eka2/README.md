# Hosted EKA2: the real kernel logic as a Linux process, ROM code native (evaluation, 2026-11-12)

> **Now built.** The design as implemented, its diagram, the choices made and the ways forward:
> `src/hosted/README.md`. Future designs (speed, one process per Symbian process, SMP, JIT/AOT for CPUs
> without AArch32): `future.md`. Status and measurements: `roadmap.md`. Shortcuts: `docs/hacks.md`.
> The text below is the evaluation written before any code (some guesses in it turned out otherwise:
> e.g. the N95's EUSER has no SWP, and the first build runs everything in one Linux process).

The idea: instead of QEMU full-system emulation (what we run today, also on an Android phone,
`docs/research/android-port.md`), host the EKA2 kernel on Linux the way Symbian's own Win32 emulator
kernel did (`kernel/eka/nkern/win32`, `memmodel/emul/win32`), but execute the phone's unmodified ARM32
ROM binaries natively as AArch32 user code on an AArch64 Linux: first `qemu-system-aarch64 -M virt`,
then a Google Pixel 3a. Static research only; nothing built or run. Tags: [verified] = read in our tree
or a cited source, [inferred] = best guess.

| File | Content |
|---|---|
| `cpu-and-device.md` | Pixel 3a facts: AArch32 at EL0/EL1, hypervisor/KVM, bootloader, Linux support, kernel config |
| `architecture.md` | options (a) hosted, (b) KVM, (c) native, (d) today's QEMU; the recommended design for (a) |
| `qemu-setup.md` | the `virt` development machine, kernel config, rootfs, debugging, reference tests |
| `roadmap.md` | phases P0-P6 with exit and kill criteria, effort, risks; status and measurements |
| `nanokernel.md` | the nanokernel's Linux platform layer as built, bugs found, the kernel layer |
| `tests.md` | every hosted test and what it pins |
| `future.md` | future designs: speed steps, one host thread, one process per Symbian process, SMP, JIT/AOT |

## Verdict in six lines

1. **Feasible, and the only route to native speed on the Pixel 3a.** The SDM670's Kryo 360 cores are
   Cortex-A75/A55 class, the last Arm cores with AArch32 at every exception level [verified: Arm
   comparison table, FreeBSD list analysis]; 32-bit Android apps run on it (Pixel 7 was the first
   64-bit-only Pixel [verified: Android Developers blog]). A 32-bit Linux process on arm64 can map
   `[0x10000, 0xFFFFF000)` [verified: `TASK_SIZE_32`], so the ROM at `0x80000000`, DLL data at
   `0x90000000` and even the kernel data at `0xC8000000` fit at their fixed addresses.
2. **KVM (option b) is dead on the phone:** Linux boots at EL1 under Qualcomm's hypervisor; `/dev/kvm`
   needs EL2 [verified: Qualcomm docs, Marc Zyngier on LKML]. On QEMU `virt` it adds nothing over (d).
3. **Native (option c) is a BSP port** (display, touch, storage, interrupts, no modem; cf. symbian-hd2,
   which still lacks audio, networking and shutdown after years) and would not reuse our personalities,
   stand-ins or sydb. Not recommended.
4. **(a) is a kernel port, not a wrapper.** Symbian's Win32 emulator replaced ~7,900 lines
   (`nkern/win32` 2,176 + `kernel/win32` 1,425 + `memmodel/emul` 4,340) and kept ~44,000 lines of
   portable nanokernel and kernel C++ [verified: line counts]. Our Linux variant is the same shape plus
   three things the Win32 emulator never needed: trapping the ROM's `svc` instructions, one Linux
   process per Symbian process with the kernel state shared between them, and an E32 image loader
   instead of `LoadLibrary`. The personalities (exec numbering, shims), the ISI fake modem and most
   stand-ins carry over as code; the QEMU-side host GPU, sound and sydb need new host back ends.
5. **Two performance traps decide it:** every `User::Heap()` is a fast exec, so execs must be patched
   into direct calls (ROM stub `svc #n; bx lr` -> `b trampoline`), with seccomp `SIGSYS` only as the
   safety net; and `swp` (used by EUSER's request completion and locks) is emulated by Linux through an
   undef trap and is off by default (`abi.swp`) [verified: EPL `uc_exec.cia`/`uc_utl.cia`, kernel docs].
6. **Recommendation:** run P0 (hardware facts on a real Pixel 3a, 1-2 days) and the bounded P1-P2
   experiment (hosted nanokernel, then the first unmodified ARM32 Symbian exe making exec calls, 3-5
   weeks). Commit to P3-P6 (3-6 months more) only if the P2 exit criteria hold; otherwise stay with
   (d), which already runs on Android at 1.7x the desktop's CPU per clock
   [verified: android-port.md measurements].

## Strategic caveats

- Hardware window: AArch32 user mode disappears from phones (Pixel 7+, Cortex-X3/A715 have no
  AArch32; 2022-23 SoCs keep it only on some cores, Linux "asymmetric 32-bit" [verified: kernel doc]).
  Option (a) targets 2017-2021 hardware and x86/arm64 PCs with a JIT-free `qemu-user`-like fallback
  never (a 64-bit host without AArch32 cannot run it at all). (d) runs anywhere.
- Fidelity: the hosted kernel has no MMU protection between user code and kernel data (as in the
  Win32 emulator) and no real ASID/domain behaviour; kernel-side bugs that today's QEMU exposes as
  faults become silent corruption. The QEMU+EKA2 environment stays the behavioural reference.
- The phone's own RAM-loaded kernel-side drivers that we keep today (checked by `kabi.toml`) would have
  to link against a Linux-built kernel; keep them out of scope until P4 shows which are needed.

## First three steps

1. P0 on the Pixel 3a (stock or rooted): `getprop ro.product.cpu.abilist`, run a static
   armhf binary from `adb shell`, read `dmesg | grep -i "EL[12]"` or the boot log for the exception
   level, test `/proc/sys/abi/swp`, `cp15_barrier`. Exit: 32-bit EL0 confirmed, EL2 confirmed absent.
2. Build the `virt` reference machine (`qemu-setup.md`): arm64 kernel with `COMPAT`, `ARMV8_DEPRECATED`,
   `SWP_EMULATION`, `CP15_BARRIER_EMULATION`, a busybox rootfs, an armhf cross toolchain; run a 32-bit
   test that maps `0x80000000` and `0xC8000000` `MAP_FIXED` and executes `swp`/`svc` under a seccomp
   filter keyed on `instruction_pointer`.
3. P1: `nkern/linux` (scheduler lock, futex parks, timer thread, DFCs) built with the armhf toolchain
   from the EPL nanokernel sources + `nkern/win32` as the template; two threads and a semaphore.
