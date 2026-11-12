# Hosted EKA2: the QEMU `virt` development machine (design, not built)

Purpose: an AArch64 Linux machine on the PC that behaves like the Pixel 3a for our needs (AArch32 at
EL0, 4 KB pages, DRM display, evdev input, ALSA audio, a GLES driver), so every phase up to P5 runs
here, and P6 moves the same user-space tree to the phone. [verified] = QEMU 11.1.3 / kernel facts
known from our tree or cited docs; the rest [inferred] until first boot.

## 1. QEMU command

```
qemu-system-aarch64 -M virt,gic-version=3 -accel tcg -cpu cortex-a55 -smp 4 -m 2048 \
  -kernel build/hosted/Image -initrd build/hosted/rootfs.cpio.gz \
  -append "console=ttyAMA0 root=/dev/ram0 abi.swp=1 abi.cp15_barrier=2 vm.mmap_min_addr=65536" \
  -device virtio-gpu-gl-pci -display sdl,gl=on \
  -device virtio-keyboard-pci -device virtio-tablet-pci -device virtio-mouse-pci \
  -device virtio-sound-pci,audiodev=snd0 -audiodev pipewire,id=snd0 \
  -virtfs local,path=build/hosted/share,mount_tag=share,security_model=none \
  -serial mon:stdio -gdb tcp::1235 -S
```

| Choice | Why | State |
|---|---|---|
| `-cpu cortex-a55` | an A55 (the 3a's Silver cores) models AArch32 at EL0 and EL1; `cortex-a76` would reject AArch32 at EL1 (true to silicon) but EL0 is enough for (a); `max` for experiments with (b) | [inferred: QEMU has `cortex-a55` since 8.x; `-cpu max` AArch32 at all ELs verified by general QEMU use] |
| `-accel tcg` | the PC is x86_64; KVM only on an arm64 host (then `-cpu host` and the host must have AArch32 EL0: no Apple silicon, no X3 class) | [verified: our hosts] |
| `gic-version=3` | matches the SDM670 (GICv3) | [inferred] |
| `virtio-gpu-gl` + `-display sdl,gl=on` | the guest gets a DRM device (`virtio_gpu`) for the LCD back end and, with virgl, a GLES driver (Mesa `virgl`) for the host-GPU path | [inferred] |
| `virtio-keyboard/tablet/mouse` | evdev in the guest, same as the phone's touch and keys | [inferred] |
| `virtio-sound` | ALSA in the guest | [verified: QEMU 11 has virtio-snd] |
| `-virtfs` (9p) | the composed ROM, ROFS image, drive images, kernel `.so` from the host tree without rebuilding the rootfs; `virtiofs` later if 9p is too slow for the ROM mapping (mmap of a 9p file with `cache=loose` works) | [inferred] |
| `-gdb tcp::1235` | host-kernel-level gdb; keep 1234 for the existing syborg sessions | [verified: port convention] |

Time: the `virt` machine with TCG runs AArch32 code through the same JIT as today, so speed numbers
here mean nothing; correctness and the design do. Native speed is measured in P6 only.

## 2. Guest kernel

Mainline 6.x (the sdm670-mainline tree is the one to use from P5 on, so that the configuration is
shared with the phone), `defconfig` plus:

```
CONFIG_COMPAT=y  CONFIG_KUSER_HELPERS=y  CONFIG_COMPAT_VDSO=n (optional)
CONFIG_ARMV8_DEPRECATED=y  CONFIG_SWP_EMULATION=y  CONFIG_CP15_BARRIER_EMULATION=y  CONFIG_SETEND_EMULATION=y
CONFIG_COMPAT_ALIGNMENT_FIXUPS=y
CONFIG_ARM64_4K_PAGES=y  CONFIG_ARM64_VA_BITS_48=y
CONFIG_SECCOMP=y  CONFIG_SECCOMP_FILTER=y  CONFIG_MEMFD_CREATE=y  CONFIG_CROSS_MEMORY_ATTACH=y (process_vm_readv)
CONFIG_DRM=y  CONFIG_DRM_VIRTIO_GPU=y  CONFIG_INPUT_EVDEV=y  CONFIG_VIRTIO_INPUT=y  CONFIG_SND_VIRTIO=y
CONFIG_NET_9P=y  CONFIG_NET_9P_VIRTIO=y  CONFIG_9P_FS=y  (CONFIG_VIRTIO_FS=y)
CONFIG_CHECKPOINT_RESTORE=y (CRIU snapshots, late)  CONFIG_KPROBES/CONFIG_FTRACE (host-side tracing)
```
[verified: the option names exist in `arch/arm64/Kconfig` on this host's 7.2 source; their need inferred.]
Security knobs at boot: `kernel.yama.ptrace_scope=0` for `process_vm_readv` between the Symbian
processes (or `PR_SET_PTRACER` in the launcher), `vm.overcommit_memory=1` (large sparse chunk
reservations), `abi.swp=1`, `abi.cp15_barrier=2` (fall back to 1 if the core lacks CP15BEN).

## 3. Rootfs

Busybox + musl or glibc for aarch64 (the nucleus and tools), the **32-bit armhf** runtime for the
hosted kernel and launcher (static linking avoids a second libc tree), `gdbserver` (armhf build),
`strace`, `perf`, Mesa (virgl / freedreno), `libdrm`, `alsa-utils`, `evtest`. Build with Buildroot
(`qemu_aarch64_virt_defconfig` plus the armhf toolchain as a second toolchain) or an Alpine/pmOS
aarch64 chroot with the `armhf` multilib; pmOS is preferred from P5 on because it is what boots on the
phone. Everything project-specific lives on the 9p share: `build/hosted/share/{rom,drives,kernel,tools}`.

## 4. Toolchain for the hosted kernel

`arm-linux-gnueabihf-g++` (GCC 13/14, distro package) with `-marm -march=armv7-a -mfpu=vfpv3-d16
-mfloat-abi=hard -fno-exceptions -fno-rtti -ffreestanding` for the kernel parts, the EPL `kernel/eka`
headers as today (`__EPOC32__`, a new `__LINUX__`/`__HOSTED__` platform macro next to `__WINS__`);
`.cia` inline assembler is already GCC syntax (GCC_SURGE) [verified: architecture.md build notes].
The GCC 14 symbianelf toolchain does not produce Linux ELF with a libc; the kernel `.so`
needs the Linux one. The ROM user side is not rebuilt: it is the phone's.

## 5. Debugging on the virt machine (sydb-style)

| sydb feature today | On the hosted machine |
|---|---|
| `log`, `events` (serial) | the hosted kernel writes `Kern::Printf` to a Unix socket + file; `sydb log` reads the file over 9p or ssh; same `grep`/`events` parsers |
| `status`, `ps`, window groups | the nucleus answers a small control protocol on a Unix socket (process list, threads, chunks); WSERV queries through the existing devagent |
| `strace MATCH` (IPC) | the `sipc.cpp` trace points (patch 0002) compile in; enabled per process through the control socket |
| `catch` (thread death with call stack) | hook in the hosted `DThread::Die` path: freeze the process group with `SIGSTOP`, dump registers from the `ucontext`, unwind with the ROM symbols |
| `dbg` (gdb, hw breakpoints) | `gdbserver --attach PID` (armhf) per Symbian process, `target remote` through a forwarded port; breakpoints are software or the CPU's hw debug registers (both work in a 32-bit process); kernel symbols from the hosted kernel ELF, ROM symbols from `SYBCODE:` lines as today |
| `snap`, `fork` | CRIU of the whole process group (shared memfds, futexes, seccomp filters are supported by CRIU) [inferred]; before that, the QEMU `virt` machine's own `savevm` snapshots the whole Linux guest, which is good enough for P1-P5 |
| screenshots, keys, touch | `drm` dumb-buffer read-back on the host side of the LCD back end; `uinput` injection |
| `phone NAME` | unchanged front: a session type `hosted` in `tools/sydb` that starts the `virt` machine (or ssh to the phone), the nucleus, and the window |

## 6. The behavioural reference: same tests on both

Keep today's syborg sessions as the oracle. A test is a script that boots, waits on a serial/property
event, sends keys/taps, and compares screenshots or property values (`tests/device-smoke.sh` and the
timed-key reports of `tools/syb_run.py` are the model). The hosted target runs the same scripts with
the `hosted` session type. Comparison points, cheapest first:
1. The exec trace (number, caller, return value) of the first 10,000 execs of `efile.exe` start
   (today's `EXEC` trace from patch 0023 / personality trace) must match call-for-call.
2. The start-up property sequence (`100058f4/1`, Starter/SSM states) must match in order.
3. `IPCRD`/`REQC` traces of a server session (e.g. the file server) for a fixed app start.
4. Screenshots after the smoke test's steps (idle, Carousel/Menu, Switch off) with a hash tolerance.
Differences are bugs in the hosted kernel until proven otherwise; the QEMU machine is not changed to
fit the hosted one.
