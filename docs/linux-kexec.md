# Linux "kexec" from Symbian on the virtual N95: roadmap

Goal: start Linux from a running Symbian on our QEMU N95 the way it would be done on a real N95
(app + kernel driver take over the machine), as a rehearsal for the real phone. Status 2026-11-06:
plan only. Background: rapuyama-linux (a Nokia E6 loader that boots Linux, ARM1176) and
the Linux 5.15 Nokia tablet tree (`n810-5.15`, OMAP2420/ARM1136); neither is part of this repository.

## What carries over to the real N95 and what does not

| Part | Emulator | Real N95 | Carries over? |
|---|---|---|---|
| User-side loader app (reads zImage/DTB/initramfs, talks to the LDD) | 9.2 user ABI (as `src/n95apps/dsbtest`) | same | **yes, same binary** |
| Loader LDD logic (alloc physical RAM, copy images, identity page, caches, MMU off, jump) | built against **our** kernel | must be built against **Nokia's 9.2 EKERN** (other headers, ordinals) | **source yes, binary no**: keep its kernel API surface tiny (≈8 calls: `Epoc::AllocPhysicalRam`, `DPlatChunkHw::New`, `Kern::ThreadRawRead`, cache sync, `NKern::DisableAllInterrupts`, …) so the 9.2 ordinals can be mapped by hand from Nokia's `ekern.exe` (`tools/syb_sym.py`) |
| Trampoline (cache clean, MMU off, Linux entry state) | Cortex-A8 = ARMv7 today | ARM1136 = ARMv6 | only if we also run **ARM1136** in QEMU (step 6), else write both v6/v7 paths |
| Linux kernel + initramfs + handoff rules (image placement, DTB, reserved memory) | | | yes |
| Device drivers (irq, timer, UART, display) | Syborg / PrimeCell | OMAP2420 (INTC, GPTIMER, UART, RFBI panel) | **no** — so spend nothing on Linux drivers for Syborg |
| Getting the LDD loaded at all | free (our ROM, our kernel) | needs a hacked phone / signing route | no (separate problem) |

Consequence: on the emulator, make Linux's hardware as cheap as possible and put the effort into
the loader/handoff, which is the part that transfers. That part is also exactly what is hardest to
debug on the phone (a failed handoff is a frozen or rebooting phone with no output); here it gets
gdb at every step and repeatable runs.

Middle option, if the Linux side should transfer too: port OMAP2420 core blocks from QEMU 0.9.1's
n800 model (interrupt controller, GP timers, UARTs, PRCM/CM/control module registers; that model
booted Linux) onto our board at their OMAP addresses (RAM is 0..512 MB, no overlap). Linux then
runs its real OMAP2 drivers with an N95-like DT. Several days, because OMAP2 init expects sensible
clock/power registers; the panel (RFBI command mode) and USB stay real-phone work. Do it after
step 5, not before.

## Roadmap

0. **Toolchain** (½ day). Host has no ARM Linux cross compiler and no `ld.lld` (clang only). Use one
   prebuilt Bootlin toolchain (e.g. `armv6-eabihf--musl--stable`: kernel and static BusyBox; ask
   before downloading) or install LLVM `lld`/binutils for `make LLVM=1`. Exit: `vmlinux`/`zImage`
   of the 5.15 tree builds with a small config.
1. **Linux-friendly devices on our board** (½ day, QEMU only). Linux has no Syborg drivers. Add
   standard ones at free addresses of `src/qemu-syborg/hw/arm/syborg.c`: PL011 UART (own chardev,
   e.g. `-serial file:linux.log`), SP804 timer, a GIC (`arm,cortex-a9-gic`-style, works with any
   CPU in QEMU) or PL190 VIC — all supported by multiplatform ARMv6/v7 Linux (ARCH_REALVIEW /
   ARCH_VERSATILE drivers). Symbian never touches them. Also: a mask-all of the Syborg interrupt
   controller and a stop of the Syborg timer must happen before the jump (step 4).
2. **Standalone Linux on that board, no Symbian** (1 day). `qemu ... -kernel zImage -dtb
   syborg-linux.dtb -initrd initramfs.cpio` with a hand-written DT (memory, the three devices,
   `chosen` bootargs `earlycon=pl011,0x... console=ttyAMA0`). Multiplatform ARMv6+v7 config,
   uniprocessor, no modules, static BusyBox initramfs. Exit: shell prompt in `linux.log`, timer
   ticks. This separates "Linux/DT is wrong" from "handoff is wrong" — never debug both at once.
3. **Loader LDD + app, returning probe** (½ day). LDD in our ROM (like the stand-ins), app on C:
   (dsbtest pipeline + `_reg.rsc`). Controls: info (MIDR, SCTLR, TTBR, CPSR), alloc+map a physical
   block, copy chunk, read back. Exit: values printed, phone keeps running. (On the real phone this
   is the plan's milestone 1.)
4. **One-way handoff with a bare-metal payload** (1 day). Identity-mapped trampoline page (write a
   section entry VA==PA into the page table, as the E6 driver does — doable from an LDD on Nokia's
   kernel too), mask IRQ/FIQ, quiesce Syborg intc/timer/DMA-ish devices (audio, fb DSB vsync),
   clean+invalidate caches (v7 set/way on Cortex-A8; v6 `c7,c14,0` only on ARM1136), clear
   SCTLR.M/C plus FA/TR (E6 lesson), jump. Payload: 20 instructions that print "MMU OFF OK" on the
   PL011 and spin. Exit: string in `linux.log`; gdb confirms PC in the payload.
5. **Kexec Linux** (1 day). Same handoff into the zImage from step 2. Place zImage/DTB/initramfs
   in one allocated contiguous block, high enough that decompression (to the 128 MB-aligned base
   + 0x8000) cannot hit the DTB/initramfs; or use the uncompressed `Image` at its link address.
   DT memory = all RAM except the block (Symbian is gone, its memory is free). Exit: BusyBox
   shell, `uname -a`, `cat /proc/cpuinfo`, repeatable.
6. **Make the rehearsal faithful** (optional, 1–2 days): build our Symbian kernel for ARM1136
   (`variant.mmh` has the `__CPU_ARM1136__` macros commented out) and run QEMU `-cpu arm1136`, so
   trampoline and Linux run the exact ARMv6 code paths of the phone; optionally 128 MB RAM.
   Risk: breaks the working N95 boot; do it on a branch with a ROM per CPU.
7. **Real-phone track** (outside the emulator): the loader LDD rebuilt against 9.2 EKERN, an
   installation route on a hacked phone, then an OMAP2420 bring-up
   (OMAP2420 DT, UART/USB console, RFBI panel). Everything from steps 3–5 is reused.

## Not wasting tokens

- **One variable at a time.** Step 2 (Linux without Symbian) before step 5; a 20-instruction
  payload before Linux; the returning probe before any one-way jump.
- **Separate, small outputs.** Linux on its own serial file; earlycon on from the first line;
  read only the last ~40 lines or a grep, never whole logs. Symbian serial stays as it is.
- **One-command runs with a summary**, like `syb_run`: e.g. `tools/linux_run.py` (standalone
  boot → last lines + "reached init?" + PC symbol at stop) and a kexec variant that drives the
  menu keys, waits for the marker and reports. No hand-typed QEMU command lines in the loop.
- **gdb at checkpoints, not tracing.** Breakpoints at the trampoline jump, the payload, Linux
  `stext`/`start_kernel`/`panic` with `vmlinux` symbols; on a hang print PC/LR as symbols. Never
  `-d in_asm,int` (megabytes); at most `-d guest_errors,unimp`.
- **Known pitfalls checklist before the first jump** (from the E6 notes): SCTLR FA/TR bits, cache
  clean before MMU off, decompressor overwriting owned RAM, pending IRQs from the old interrupt
  controller, VFP/NEON state, `r0=0, r1=~0, r2=DTB`, ARM (not Thumb) entry.
- **Stop rule:** the same symptom 3 runs in a row → stop, write down facts vs hypotheses, change
  the method (add a probe/tool), and log the missing tool in `docs/tooling-ideas.md`.
- **Build hygiene:** kernel build output to a log file, show only errors; incremental builds;
  one config file in the repo (to be created), not edited by hand each run.
