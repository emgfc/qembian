# Emulator options: QEMU 0.9.1 vs. modern QEMU vs. dynarmic/Unicorn

Recorded 2026-11-05 from a discussion with the project owner.

## What we need from the emulator

Option 1 (our EKA2 kernel + the N95 user side) and option A (OMAP2420 emulation) both run a
**real kernel**, so the emulator must provide a whole machine:

- **MMU** with ARM page tables. Our kernel uses the multiple memory model with real address spaces.
- **Privileged CPU state:** SVC/IRQ/abort/undefined modes, exceptions, SWI dispatch into the
  kernel, CP15 and VFP state.
- **Devices:** the Syborg board's interrupt controller, timers, framebuffer, keyboard, pointer,
  serial ports and NVMemory disk, with their register layouts. Option A needs OMAP2420 devices.

## Why dynarmic (or Unicorn) can't replace QEMU by itself

Dynarmic, the ARM JIT used by EKA2L1 and Yuzu, translates **user-mode CPU code only** and leaves
memory, exceptions and devices to the host program. EKA2L1 can use it because it does not run a
kernel: it reimplements the kernel and servers in C++ (HLE), which is the approach this project
deliberately did not choose. Using dynarmic here would mean writing our own MMU, exception model
and every device, which amounts to a new system emulator.

Unicorn is a QEMU-derived CPU library. It has no devices and only limited system-mode support, so
it has most of the same problems.

KVM-style hardware acceleration does not exist for 32-bit ARM guests on an amd64 host, so a modern
QEMU's TCG is the practical speed ceiling.

## How tied we are

- **Not tied to QEMU itself.** Almost everything built so far sits above the emulator: the
  kernel's 9.2 personality, the ROM composer, the Nokia stand-in drivers, the fake modem and the
  analysis tools. They depend on the Syborg board's device interface, not on QEMU code.
- **Tied to QEMU 0.9.1-symbian in one place.** The Syborg devices are implemented there, many of
  them as Python 2.6 plugins (`build/qemu/install/share/qemu/plugins/syborg_*.py`). That is also
  a large part of why it is slow, and it is why running still needs the Debian 6 container.

## Options

1. **Port the Syborg board to a modern QEMU (recommended).** Re-implement about 10 small devices
   in C as a QEMU machine, taking the memory map from `syborg.dts`. Modern TCG is much faster, gdb
   support is better, and running no longer needs Python 2.6 or the legacy container. Option A
   also benefits, since modern QEMU still has OMAP and N800 code.
2. **Unicorn:** CPU only, no devices, limited system mode. Not a fit.
3. **A custom emulator on dynarmic:** a large project that only makes sense if the project ever
   moves to a hybrid HLE design, which is not the current plan.

Replacing the Python plugins inside QEMU 0.9.1 with C devices was considered as a cheaper step,
but the measurement below shows they are not where the time goes; it would mainly remove the
Python 2.6 dependency.

## Python device plugins in QEMU 0.9.1-symbian: measured, not the bottleneck

Plugins (`build/qemu/install/share/qemu/plugins/`): interrupt controller, timer, RTC, serial,
framebuffer, keyboard, pointer, NVMemory front end (the disk itself is C, `libnvmemmory.so`), a USB
test device. About 1,100 lines of Python in total, so rewriting them in C would be quick.

Measurement (2026-11-05): `perf` cannot attach across the rootless container's user namespace, so
QEMU was sampled from the host with gdb (`set sysroot /proc/<pid>/root`, unstripped binary from
`build/qemu/qemu-symbian-svp/arm-softmmu/`), 60 backtraces during the busy first minute of a
quiet hybrid boot (`scripts/hybrid-run.sh` image, panic-only trace):

| Where | Samples |
|---|---|
| Python (`PyEval*`, `PyObject_*`) | **0** |
| TCG CPU loop (`cpu_exec` → `tb_find_slow` translation-cache misses, `tlb_fill` / `get_phys_addr` ARM page-table walks, `__ldb_cmmu` code fetches through the soft MMU, `tlb_flush`) | 33 |
| generated code / no usable frames | 27 |

Once the guest stalls, 47 of 50 samples sit in `select()` in QEMU's main loop: QEMU idles correctly.

Conclusion: CPU time goes into QEMU 0.9.1's TCG and soft MMU. The multiple memory model changes
address space on every context switch, and this QEMU flushes and refills its TLB each time.
Rewriting the Python plugins would not speed up the boot noticeably. The exception is the serial
port: with heavy kernel tracing (millions of lines) every byte goes through
`syborg_serial.py`, so trace-heavy runs are slower partly because of Python. The panic-only trace
used by `hybrid-run.sh` avoids that.

What would help, in order of effect:
1. **A modern QEMU** (option 1 above): a far better TCG with a larger translation cache and much
   cheaper TLB handling.
2. Keeping kernel tracing off unless needed. For trace-heavy debugging, a C serial device would
   help.
3. Kernel-side, in our build: check whether the Syborg kernel can use ASIDs instead of full TLB
   flushes on ARMv6/v7 (only useful if QEMU 0.9.1 models ASID-tagged TLBs, which is doubtful).
