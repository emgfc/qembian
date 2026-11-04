# Build log: problems, causes, fixes

Chronological. Each entry gives the symptom, the root cause, the fix, and how the fix was
verified. All fixes live in scripts or the container definition, so nothing is applied by hand.

## Environment facts

- Host: openSUSE Tumbleweed (kernel 7.2.11), x86_64, 16 cores, podman 6.1.2. The Docker
  daemon was not running, so podman (rootless, `--userns=keep-id`) is used.
- Container: `docker.io/debian/eol:squeeze` **i386** (Debian 6.0.10). Packages come from
  archive.debian.org with `AllowUnauthenticated`, because the archive keys have expired.
  **Risk**: apt cannot verify package signatures. The image does not run network services
  and only the project directory is bind-mounted. Ubuntu 10.04 i386 is not on Docker Hub.
- Tools: gcc 4.4.5, Python 2.6.6, Perl 5.10.1, CSL `arm-none-symbianelf-gcc` 4.4.1
  (2009q3-63), GNU ld 2.19.51, sbs 2.15.2, QEMU 0.9.1-symbian.

## 1. Sourcery G++ 2010q1 unavailable
- **Symptom**: `sourcery.sw.siemens.com` does not resolve, and the CodeSourcery/Mentor hosts are dead.
- **Fix**: a Wayback CDX search found `arm-2009q3-63-arm-none-symbianelf-i686-pc-linux-gnu.tar.bz2`
  (also GCC 4.4.1), sha256-pinned in `sources.lock`. A 2012.03 (GCC 4.6.3) build from archive.org is kept as a fallback.

## 2. QEMU build
- Built unpatched on the first try with GCC 4.4.5 + Python 2.6 + SDL 1.2.
- `fdt support: no` in the configure summary is **harmless**. configure only probes for a system
  `-lfdt` (used by the PPC `device_tree.c`). The bundled `libfdt/` is always linked, and that is
  what `devtree.c` (the `.dtb` machine loader) uses.
- The Python plugins are looked up in `<bios_dir>/plugins` (`python-plugin.c`), so they are installed to
  `share/qemu/plugins` and `-L` points there. No `PYTHONPATH` is needed.
- The `nvmemorydevice` plugin calls `sys.exit()` if `libnvmemmory.so` cannot be loaded, so it is built
  from `model-libraries/` and found via `LD_LIBRARY_PATH`.

## 3. Headless running
- **Symptom**: `Could not initialize SDL - exiting` (DirectFB probe) even with `-nographic`.
  The fork always initialises SDL. Fix: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.
- **Symptom**: the shell is invisible on serial. ESHELL renders on the framebuffer (EWSRV/ECONS), and
  `screendump` is unusable because the Python framebuffer passes `screen_dump=NULL`. VNC is `#if 0`'d out.
  Fix: `boot-test.sh` runs SDL against **Xvfb** and grabs screenshots with `xwd | convert`.
- **Symptom**: typed `\` arrives as `#`. The Syborg keymap is UK (`KConvertCode[0x2b]=EStdKeyHash`,
  `[0x56]=EStdKeyBackSlash`). `xdotool type` did not honour an xmodmap remap, so `\` is
  sent as keysym `less` (X keycode 94 → scancode 0x56). [verified: `dir z:\sys\bin\e*.exe`]

## 4. Raptor's bundled Python reports `sys.platform == 'linux7'`
- **Symptom**: `raptor_data module loaded on an unrecognised platform 'linux7'`.
- **Cause**: Raptor builds its own Python 2.7 from source. Its configure derives `MACHDEP` from
  `uname -r`, and the host kernel is 7.x.
- **Rejected fix**: presetting `MACHDEP=linux2`. That skips the configure block that sets
  `ac_sys_system`, so `LDSHARED` becomes bare `ld` and every extension module fails to link
  (`undefined reference to memset`, `No module named _struct`).
- **Fix**: `/usr/local/bin/uname` in the container reports release `2.6.32`, the same effect as
  `setarch --uname-2.6`, which squeeze's util-linux lacks. All 2010-era configure scripts
  now see a plausible kernel.

## 5. libgcc lacks the ARM EABI integer helpers
- **Symptom**: 26 kernel-side link failures: `undefined reference to __aeabi_idiv / __aeabi_uidiv /
  __aeabi_ldivmod / __aeabi_uldivmod / __aeabi_idivmod` (in EKERN, MMC, paging and sampler drivers).
- **Cause**: GCC's `config/arm/t-symbian` deliberately omits these from libgcc, because on Symbian
  the RVCT runtime normally supplies them. CSL 2009q3's members `_divdi3.o`/`_udivdi3.o` are
  **empty**. GCC_SURGE links kernel-side code with `-lgcc` and relies on the toolchain providing
  them; CSL 2012.03's libgcc does.
- **Fix** (`scripts/fix-toolchain-libgcc.sh`, run for both multilibs and idempotent from `libgcc.a.orig`):
  build the helpers from the **GCC 4.4.1 sources matching the compiler** (sha256-pinned):
  - `lib1funcs.asm` with `L_udivsi3 L_divsi3 L_umodsi3 L_modsi3`
  - `lib1funcs.asm` with `-U__symbian__` for `L_aeabi_ldivmod L_aeabi_uldivmod L_aeabi_lcmp
    L_aeabi_ulcmp L_lshrdi3 L_ashrdi3 L_ashldi3`, because these are compiled out for `__symbian__`
  - `bpabi.c` (`__gnu_[u]ldivmod_helper`)
  - `libgcc2.c` with `L_udivmoddi4 L_divdi3 L_udivdi3 L_muldi3`, compiled through a minimal target
    shim (`scripts/libgcc-shim/tm.h`, ILP32/LE plus the `bpabi.h` `__aeabi_lmul` rename).
    The shim's type model was checked by building the same routines for i386 and comparing them
    against native division: **5,000,000 random cases, 0 mismatches**
    (`scripts/libgcc-shim/test/divtest.c`).
- **Follow-on 1**: `multiple definition of __aeabi_idiv0/__aeabi_ldiv0`. The kernel defines both
  (`common/arm/gccehlp.cpp` calls `FAULT()`), and user side gets them from `drtaeabi.dll`. So
  `_dvmd_tls.o` is **not** added. But GCC 4.4.1's division code calls the legacy `__div0`, so a
  2-instruction `__div0 → __aeabi_idiv0` tail-call forwarder is added (`libgcc-shim/div0.S`).
- **Follow-on 2**: `elf2e32 : Error: E1035: Undefined Symbol __aeabi_llsr`. EKERN's frozen
  `ekernu.def` **exports** `__aeabi_{lmul,llsl,llsr,lasr,lcmp,ulcmp}` for kernel-side DLLs. Under
  RVCT, `armcchlp.cia` forces them in. Providing them in libgcc (above) fixes it.
- **Verified**: `kernelhwsrv` package builds with `sbs : errors: 0`. The kernel boots and runs
  64-bit maths paths (NTimer, kernel heap setup) without faults.

## 6. Missing `libboost_thread-mt`
- `imgtools/imgcheck` links `-lboost_thread-mt`. Fixed by adding `libboost-thread-dev` (1.42) to the image.

## 7. Small script portability issues inside squeeze
- `/.containerenv` is absent in this setup, so the scripts detect the container with `SYBORG_IN_CONTAINER=1`.
- bash 4.1: no negative array subscripts. coreutils 8.5 `timeout` has no `--foreground`.

## 8. Verified outcome

- Clean rebuild from scratch (`rm -rf build cache`, container image deleted) via `scripts/build-all.sh`:
  **3 min 35 s**, exit 0 ([log](evidence/clean-build.log)). This excludes the one-time
  downloads (about 2 GB, which were cached).
- `kernelhwsrv`: `sbs : errors: 0`. Syborg BSP + ROM: `sbs : errors: 0`, 124 warnings
  (mostly unfrozen exports and `-Wno-*` noise).
- ROM `syborg_tshell_ARMV5_udeb.img`: 4,890,624 bytes, 68 files. The RVCT-built PDK ROM is 2.1 MB;
  GCC udeb code is larger.
- Boot: ESHELL `0.01(3049) CFG=UDEB` on the framebuffer. The PDK fixture shows `(4004)`, which
  confirms our image is a different build. Commands work (`dir`), and a second process
  (`btrace.exe`) starts in its own console.
- Serial with `kerneltrace 0x80000003`: about 12k lines covering variant init, MMU (multiple memory
  model), nanokernel and timers, kernel heap, then loader → ESTART → domainSrv → EWSRV → ESHELL.
  No faults or panics.
- gdb (host, multi-arch 16.3) → QEMU gdbstub: stops at the reset vector, and a hardware breakpoint
  on `_E32Startup` (0x80003130) hits after the MMU is on.
