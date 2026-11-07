# qembian architecture

## Layers

```
┌───────────────────────── host (any modern x86_64 Linux) ─────────────────────────┐
│ podman ── squeeze i386 container ───────────────────────────────────────────────┐ │
│   build side:  Raptor/sbs ─ CSL GCC 4.4.1 ─ elf2e32 ─ rombuild  ──► ROM image    │ │
│   run side:    qemu-system-arm 0.9.1-symbian                                     │ │
│                 ├─ machine built from syborg.dtb (devtree.c + libfdt)            │ │
│                 ├─ Python 2.6 device plugins (intc, timer, rtc, serial, fb, kbd, │ │
│                 │   pointer, nvmemory→libnvmemmory.so) + C devices (hostfs,      │ │
│                 │   snapshot, nand, virtio-net/audio, platform)                  │ │
│                 └─ SDL (Xvfb for tests / host X11) ; serial0 → file/stdio ;      │ │
│                    gdbstub :1234                                                 │ │
└──────────────────────────────────────────────────────────────────────────────────┘
```

Building and running both happen in the same legacy container. QEMU needs Python 2.6 and SDL
1.2 there, and the build tools need 32-bit glibc 2.11, so one image serves both. QEMU is still
a separate artifact (`build/qemu/install`) and does not depend on the Symbian build tree.

## Syborg virtual board (from `baseport/syborg/syborg.dts`)

CPU: ARM1136 r0p2 (QEMU `arm1136-r2`), as the N95; kernel and BSP built for `__CPU_ARM1136__` (code ARMv5, docs/hacks.md H-cpu-arm1136). Until 2026-11-06 it was a Cortex-A8. RAM is 168 MB at physical 0 (N95 free-RAM level, docs/hacks.md H-ram-168); the N95 ROFS is in a read-only flash window at 0x60000000 (H-rofs-flash).

| Base | Device | IRQ | Implemented in |
|---|---|---|---|
| `c0000000` | interrupt controller (32 lines) | → CPU | Python |
| `c0001000` | RTC | | Python |
| `c0002000` | timer 0 (100 kHz), system tick | 1 | Python |
| `c0003000` | keyboard (PC scancodes) | 2 | Python |
| `c0004000` | pointer/touchscreen | 3 | Python |
| `c0005000` | framebuffer | 4 | Python |
| `c0006000`–`c0009000` | serial 0–3 (serial0 = kernel debug port) | 5–8 | Python |
| `c000a000` | hostfs (host directory as a drive) | | C |
| `c000b000` | snapshot | | C |
| `c000c000` | virtio-net | 9 | C |
| `c000d000` | NAND | | C |
| `c000e000` | virtio-audio | 10 | C |
| `c0010000` | NVMemory block device (`C:` drive, `build/.../nvmemory/syborg_system.img`) | 12 | Python + `libnvmemmory.so` |
| `c0011000` | timer 1 | 14 | Python |
| `c0012000` | platform/control | | C |

## ROM image and boot

QEMU's `arm_load_kernel` copies the raw ROM image to **physical `0x00010000`** and starts at
a small stub at `0x0`. gdb confirms the CPU starts at `pc=0` in SVC mode. The image starts with
the **Syborg bootstrap** (`_syborg_bootloader_bootrom.bin`, from the `base/bootstrap` extension
with `syborg.s`). The bootstrap sets up the page tables for the **multiple memory model**, maps
the ROM at linear `0x80000000` (`romlinearbase`) and kernel data at `0xC8000000`, enables the MMU and
jumps to EKERN's `_E32Startup` (`0x80003130` in our build).

ROM header settings (`rom/header.iby`, preprocessed into
the generated oby): `memmodel multiple 0x100000`,
`romsize 0x2000000`, `kerneltrace 0x80000000` (panics only; `build-rom-trace.sh` changes it),
`debugport 0`, `nowrapper`, PlatSec enforcement ON.

Kernel-side load order (from the oby): `EKERN.EXE` (primary) → `BTRACEX.LDD` → `KASYBORG.DLL`
(variant) → `EXMONCOMMON`/`EXMONDEBUG` (crash monitor) → `KDEBUG` → `LCD.DLL` → `EUART1.PDD`,
`ELOCD.LDD` → `EPOINTER` → `ECOMM.LDD` → `EXSTART` → `EKEYB` → hostfs, snapshot, webcamera,
`MEDNVMEMORY.PDD`, `MEDINT.PDD`, sound.

Boot sequence observed on serial with `kerneltrace 0x80000003`
(excerpt):

1. `Variant installed` → `K::InitialiseMicrokernel` → `MM::Init1` (ARM MMU, caches)
2. `Syborg::Init1`, `SyborgInterrupt::Init1`, `NTimerQ::Init1 - period 10000 us` (100 Hz tick)
3. Kernel fixed heap, initial thread and process, `MmuBase::Init2`, object containers, SvHeap/SvStack/MsgChunk
4. Supervisor thread, `K::Init3`, NVRAM, then the kernel extensions above in order (`SVPSNAPDRIVER ...` etc.)
5. User side: F32 loader → `ESTART.EXE` (reads `Z:\sys\data\ESTART.TXT`: `D:` IRAM FAT, `C:` NVMemory FAT, format-on-corrupt)
   → `domainSrv.exe` → `EWSRV.EXE` (text window server on the framebuffer) → `ESHELL.EXE`

## Build pipeline

```
sbs -c arm.v5.udeb.gcce4_4_1.surge
     │   arm.v5      ARMV5 platform, EABI, build to epoc32/release/armv5/<var>
     │   gcce4_4_1   GCCE with $SBS_GCCE441BIN, RUNTIME_LIBS drtaeabi.dso dfpaeabi.dso
     │   surge       (epoc32/sbs_config/s60_sbs_config.xml from prep_env.pl) -fpermissive,
     │               -mapcs-frame, USER_STATIC_RUNTIME_LIB usrt4_0.lib, KERNEL_STATIC_RUNTIME_LIB ksrt_gcce.lib
     ▼
 .cpp/.cia ──gcc──► .o ──ld (+libgcc, ksrt, .dso import libs)──► ELF ──elf2e32 (+frozen .def)──► E32Image
                                                                               │
 baseport bld.inf PRJ_EXTENSIONS base/rom ─► romnibus.pl ─► cpp(.oby/.iby) ─► rombuild ─► ROM .img + .symbol
```

Things to know:
- **Variant-specific binaries** are built per board with a `_<variant>_` prefix (for example
  `_syborg_ekern.exe`, `_syborg_euser.dll` → `EUSER.DLL` in the ROM). The `kernelhwsrv` package
  also builds the `_template_*` variant as a compile check.
- Frozen **DEF files** (`kernel/eka/eabi/*.def`) fix export ordinals. elf2e32 fails if an
  exported symbol is missing (see build-log §5).
- `.cia` files are C++ with inline assembler. GCC_SURGE converted the RVCT `__asm` bodies to GCC
  syntax or C++ (bug 3391 and others).

## Where things can be hooked for debugging

- serial0: `Kern::Printf` / `RDebug::Print`. Mask bits are in `kernel/eka/include/nkern/nk_trace.h`.
- gdb: `scripts/run.sh --gdb`, then use addresses from `epoc32/rom/syborg/syborg_tshell_ARMV5_udeb.symbol`.
  Not done yet: `tools/elf4rom` in the QEMU repo converts a ROM image to an ELF with debug info.
- Crash monitor: `EXMONDEBUG` dumps kernel state to the debug port on a fault.
