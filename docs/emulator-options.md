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

## Plan: porting the Syborg board to a modern QEMU

Not started; recorded 2026-11-05 for later.

**Work items**
1. **Machine:** a new QEMU board with the `syborg.dts` memory map (512 MB RAM at 0, devices from
   0xC0000000). It uses the existing `cortex-a8` CPU (VFP on), loads the raw ROM at physical
   0x10000, and adds the reset stub that jumps there, as `arm_load_kernel` does in 0.9.1.
2. **Devices in C** (modern qdev/sysbus API; register semantics copied from the 0.9.1 Python
   plugins and C files):
   - Required: interrupt controller (32 lines), timer ×2, RTC, serial ×4, framebuffer (32 bpp
     onto a QEMU display surface, size from the board config), keyboard (PC scancodes FIFO),
     pointer (absolute), NVMemory block device (C: drive, file-backed), platform/control block.
   - Later: hostfs, snapshot, NAND, virtio-net, virtio-audio.
   Mainline QEMU is believed to have once carried C Syborg devices (around 0.12–1.x); check git
   history for a starting point.
3. **Packaging:** pin a modern QEMU release in `sources.lock`; keep the board as patches under
   `patches/qemu-modern/`; build it on the host or a modern container. The run side then needs no
   Python 2.6 or Debian 6.
4. **Validation:** text-shell ROM to ESHELL; hybrid ROM to the same stage as on 0.9.1; then compare
   boot times.

**Effort:** about 2–4 focused days. Most of the risk is in matching register semantics exactly
(interrupt status/clear, timer reload, framebuffer format) and in Cortex-A8 CP15 details.

**Why it should be faster** (QEMU 0.9.1's TCG is where the CPU goes, see the measurement above):
- larger translation cache, direct block chaining and better code generation (fewer
  `tb_find_slow` misses)
- per-mode TLBs with dynamic sizing and a victim TLB, and an inlined lookup fast path: the
  multiple memory model's TLB flush on every task switch costs far less (`tlb_fill` /
  `get_phys_addr` dominated the 0.9.1 profile)
- better translation of ARMv6/v7, VFP and exclusives

No hardware acceleration exists for 32-bit ARM guests on x86 hosts, and MTTCG does not help a
single-core guest. The speed-up is unmeasured; validate with a minimal spike (the text-shell ROM's
time to ESHELL on both).

**Side benefits:** a modern gdbstub; TCG plugins for tracing without serial output; record/replay;
a modern base for option A (OMAP2420/N800 machine code still exists in QEMU).

## Phase −1 results: quick wins on QEMU 0.9.1 (2026-11-05)

Benchmark: `scripts/measure-boot.sh` (hybrid ROM, panic-only trace, host-side timestamps of
serial milestones; results appended to `build/logs/boot-times.tsv`).

| Configuration | WSERV starts | HWRM light request |
|---|---|---|
| 0.9.1 as built | 10.0 s | 66.8 s |
| + TLB invalidate-by-MVA flushes only the affected pages (instead of the whole TLB) | 10.0 s | 66.7 s |
| + `-tb-size 256` | 10.0 s | 66.8 s |
| 4 KB soft-MMU pages (`TARGET_PAGE_BITS 12`) | no kernel output at all (boot breaks; not investigated) | |

QEMU uses 100% of one host core throughout, yet the milestone times do not move: this part of
the boot is **paced by guest timers and timeouts** (fake-modem waits, start-up delays, an idle
loop that keeps the CPU busy), not by emulation speed. Consequences:
- neither the quick wins nor a modern QEMU will shorten the timer-paced stretches; a modern
  QEMU helps the CPU-bound work (drawing, heavy kernel tracing, application start-up);
- **snapshots** (resume a booted state) are the bigger iteration win, so VMState is part of
  every new device from the start.
The per-page invalidate is kept for reference as `patches/qemu-unused/0001-per-mva-tlb-invalidate.patch`;
the legacy build stays unmodified.

Also seen: with the panic-only trace the hybrid boot stops progressing after ~80 s (no
RightsServer/Startup milestones within 30 min), while traced runs get further. A timing-dependent
problem in the N95 bring-up, tracked separately.

## Phase 0 results: Syborg board on QEMU 11.1.3 (2026-11-05)

Code: `src/qemu-syborg/hw/arm/` (new GPL-2.0-or-later files, written from the guest register
descriptions and the 0.9.1 plugin behaviour; NVMemory written from the guest driver only, the
EPL plugin/library were not copied). Build: `scripts/build-qemu-modern.sh` (host-native, tarball
pinned and signature-checked in `sources.lock`). Run: `scripts/run-modern.sh [--gui] [--qmp SOCK]
[--gdb|--gdb-wait] [--dtb FILE] ROM.img`; QMP helper `tools/qmp.py` (screendump PNG, typing,
keys, savevm/loadvm). Devices: interrupt controller, timer, RTC, 4 serial ports, framebuffer
(32 bpp), keyboard (set-1 scancodes), pointer (absolute), NVMemory (`-drive if=mtd`), platform
(DTB at +0x1000). All have VMState. Not modelled: hostfs (its server exits −44), snapshot,
virtio, NAND.

Results:
- Text-shell ROM: same serial output as 0.9.1; ESHELL on screen; `dir` typed via QMP lists the
  freshly formatted C: drive.
- N95 hybrid ROM, benchmark (`scripts/measure-boot.sh --qemu modern`):

| Run | WSERV | HWRM light | RightsServer DRM | Startup UI phase |
|---|---|---|---|---|
| QEMU 0.9.1 | 10.1 s | 66.7 s | never (stalls) | never |
| QEMU 11.1.3 | 11.3 s | 62.0 s | 97.8 s | 149.8 s |

  The modern run reaches the same screen as the best traced 0.9.1 runs (status pane over the
  splash) in ~2.5 minutes; 0.9.1 stalls after ~80 s when untraced (likely an emulation or timing
  problem specific to 0.9.1; not investigated further).
- Idle CPU: the Syborg idle routine used the ARMv6 CP15 wait-for-interrupt, a NOP on ARMv7, so
  modern QEMU spun at 100%. Baseport patch `qemu-baseport/0004` adds the ARMv7 `WFI`; both QEMUs
  now idle at 0%.
- Wall time to the milestones is still dominated by guest timers/timeouts, so raw speed-up is
  modest on this boot; the CPU-bound parts (and snapshots, next) are where the new QEMU pays off.
