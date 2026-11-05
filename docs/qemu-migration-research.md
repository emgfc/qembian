# Moving the Syborg board to a modern QEMU: research report

Written 2026-11-05. Read-only research: nothing was built or booted. Every claim is tagged
**[verified]** (checked in source files in this tree, upstream QEMU sources, or upstream docs; the
file/commit is named) or **[inferred]** (from memory of QEMU internals, or a conclusion drawn from
verified facts; needs a check before anyone relies on it).

This report confirms, corrects and extends `docs/emulator-options.md`.

## Executive summary

- **Do the port; the plan in `emulator-options.md` is sound.** The device set is small (eight
  devices the ROMs touch, ~900 lines of Python plus the NVMemory C++ library), every register map
  is documented by the guest drivers and the plugins, and mainline QEMU 0.11–1.0 carried
  MIT-licensed C versions of seven of them with *identical* register layouts (good reference, not
  directly compilable). Budget **3–5 focused days** for a working headless boot with snapshots
  (the doc's 2–4 days covers the devices but not the tooling and validation). [inferred]
- **Target QEMU 11.1.x** (latest stable 11.1.3, 2026-10-28), built on the host
  (`--target-list=arm-softmmu`); keep the board as a patch series/branch pinned in `sources.lock`.
  Modern QEMU cannot be built in the Debian 6 container and there is no out-of-tree machine-type
  ABI, so an in-tree `hw/arm/syborg*.c` patch set is the only practical shape. [verified for the
  version; inferred for the rest]
- **CPU: `cortex-a8`, as now.** The kernel is built for Cortex-A8 (`__CPU_CORTEX_A8N__`,
  `__VFP_V3`, multiple memory model with ASIDs, ARMv7 TLB maintenance). Modern QEMU still ships
  `cortex-a8` (VFPv3+NEON, EL3). Create it with `has_el3=false` so it behaves like the
  TrustZone-less A8 of 0.9.1. [verified except the `has_el3` advice]
- **The speed-up should be large, for reasons specific to this workload**, not just "newer TCG":
  (1) 0.9.1 emulates ARM with **1 KB soft-MMU pages** (`TARGET_PAGE_BITS 10`), so every 4 KB
  guest page costs four page walks after a flush; modern QEMU uses 4 KB pages for ARMv7.
  (2) 0.9.1 turns **every TLB-invalidate-by-MVA into a full TLB flush**; the Symbian kernel issues
  one on every page unmap and on every IPC descriptor alias; modern QEMU flushes one page.
  (3) ASID changes flush the whole TLB in both (the kernel changes ASID on every context switch
  and twice per cross-process IPC copy), but the modern flush/refill path is far cheaper (dynamic
  TLB sizing, victim TLB, QHT block lookup, `goto_ptr`, 1 GB code buffer vs 32 MB). No published
  benchmark covers 0.9.1 vs modern; expect several-fold, measure in phase 0. [verified facts;
  magnitude inferred]
- **Two cheap experiments on the old QEMU meanwhile:** rebuild the fork with
  `TARGET_PAGE_BITS 12` (safe for the ARMv6/v7 page-table format the kernel uses) and switch the
  `c8,c7,1` TLB-invalidate-by-MVA case from `tlb_flush` to `tlb_flush_page`; and try
  `-tb-size 256` (the fork accepts it; check `info jit` first). Each is under an hour plus a
  container rebuild. [inferred; validate on the text-shell ROM]
- **Tooling wins are real:** QMP `screendump` (PNG since 7.1), `send-key`, `input-send-event`
  (absolute pointer, 0..0x7fff, which is exactly what the guest pointer driver expects), so
  Xvfb/xdotool go away; `savevm`/`loadvm` of a booted state is feasible as soon as all devices
  have `VMStateDescription`s and the NVMemory drive is a qcow2 (or an "orphan" qcow2 holds the
  state); TCG plugins (`hwprofile`, `execlog`) replace serial tracing; `perf` works on the host
  process. Record/replay works in principle but costs `-icount`; treat as a later extra.
  [verified]
- **Correction to `emulator-options.md`:** modern QEMU **no longer has the OMAP2/N800/N810
  machines** (removed in 9.2 together with all PXA2xx boards). Option A would need those files
  revived from QEMU 9.1 too. [verified]
- **Pitfalls to design in from day one:** level-triggered interrupt controller whose `CURRENT`
  register must never disagree with the IRQ line (the guest dispatcher does no spurious check);
  the RTC plugin's odd mixed-unit arithmetic (replicate exactly, the guest only uses the low 32
  bits as a µs counter); NVMemory completion is synchronous and `STATUS` reads de-assert the IRQ;
  framebuffer is a 32 bpp little-endian word with `COLOR_ORDER 0`; keyboard wants PC set-1
  scancodes with the 0xE0 prefix folded into bit 7 and release in bit 31; unassigned-address
  behaviour (0.9.1 returns 0 silently; set `ignore_memory_transaction_failures` to match at first).
  [verified]
- **Licensing is clean for everything we need to reuse:** CodeSourcery's Syborg C files (fork and
  mainline) are MIT; `devtree.c`/`fb_render_engine.c` are LGPL-2.1+; the Python plugins carry no
  header (fork is GPL/LGPL per its `LICENSE`); the NVMemory plugin and C++ library and the guest
  baseport are **EPL-1.0** (Nokia), which is *not* GPL-compatible, so re-implement the NVMemory
  device from the register description rather than copying EPL code into a GPL QEMU tree.
  [verified licences; GPL/EPL incompatibility is the standard reading, inferred]

## 1. Inventory of the current Syborg device set

Sources: `build/qemu/qemu-symbian-svp/plugins/*.py` (same files as
`build/qemu/install/share/qemu/plugins/`), `hw/syborg*.c`, `devtree.c`, `python-plugin.c`,
`hw/arm_boot.c`, `hw/arm_pic.c`; guest side `build/epocroot/sf/adapt/qemu/baseport/syborg/`;
board file `external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dts`. All [verified] unless noted.

### 1.1 How the fork builds the machine

- `-M <file.dtb>`: `vl.c` returns `devtree_machine`; `devtree.c` (CodeSourcery 2008, **LGPL
  2.1+**, 734 lines) parses the FDT with libfdt: creates CPUs from nodes with
  `device_type = "cpu"` (`hw/arm_pic.c`: node name `ARM,Cortex-A8@0` → `cpu_arm_init("cortex-a8")`,
  optional `cp15,ctr`/`cp15,clid`/`cp15,ccsidN` properties, unused in our DTS), RAM from
  `device_type = "memory"` (`reg = <0 20000000>` → 512 MB at 0), then for every registered device
  class finds nodes by `compatible`, copies integer/string properties named by the class, maps
  each `reg` base with the class's `mem_size` (0x1000 for all plugins, 0x1000000 for the platform
  device), and wires `interrupts`/`interrupt-parent` (or `qemu,interrupts = <&cpu0 0>` for the
  interrupt controller → CPU IRQ line; 1 would be FIQ). DTS integers inside `< >` are hex (dtc
  syntax; `d#` marks decimal), so `num-interrupts = <20>` is 32 and `frequency = <d#100000>` is
  100 kHz.
- Python glue: `python-plugin.c` (2086 lines, no licence header) embeds Python 2.6 and exposes
  module `qemu` with `devclass`, `ioregion(size, readl, writel)`, `ptimer` (wraps `hw/ptimer.c`),
  `render` (wraps `hw/fb_render_engine.c`, LGPL), `chardev`, `memregion` (host pointer to guest
  RAM), `dma_readb/writeb/readl/writel`, `get_clock` (`vm_clock`, ns), `start_time` (host
  wall-clock seconds), `register_keyboard`, `register_mouse`, `register_device`. Plugins are loaded
  from `share/qemu/plugins/qemu_arm_plugins.py`, which imports fb, interrupt, rtc, serial, timer,
  keyboard, pointer, nvmemorydevice (usbtest is commented out).
- Boot: `hw/arm_boot.c` `cpu_bootstrap` → `arm_load_kernel` with `loader_start` = first RAM base
  (0). A raw `-kernel` image is loaded at `loader_start + 0x10000`, treated as "Linux": a
  bootloader stub is written at 0, ATAGs at 0x100, and the CPU resets to PC 0 and jumps to
  0x10000 with r0=0, r1=board-id (`/chosen/board-id`, absent → 0), r2=0x100. The Symbian
  bootstrap ignores r1/r2.
- Build flags: `-O2 -g` (`config-host.mak`), so the slowness is not an unoptimised build. The
  fork is TCG-based (has `tcg/`), despite the 0.9.1 label (it is a 2008 SVN snapshot).

### 1.2 Devices

| Node (`compatible`) | Impl., size, licence | Base / IRQ | Needed by our ROMs? |
|---|---|---|---|
| `syborg,interrupt` | Python 91 lines | 0xC0000000 → CPU IRQ | **Yes** |
| `syborg,rtc` | Python 53 | 0xC0001000, no IRQ | **Yes** (high-res timer) |
| `syborg,timer` | Python 102 | 0xC0002000, IRQ 1 | **Yes** (nanokernel tick) |
| `syborg,keyboard` | Python 81 | 0xC0003000, IRQ 2 | **Yes** |
| `syborg,pointer` | Python 104 | 0xC0004000, IRQ 3 | **Yes** |
| `syborg,framebuffer` | Python 136 + `fb_render_engine.c` 627 (LGPL) + templates | 0xC0005000, IRQ 4 | **Yes** |
| `syborg,serial` ×4 | Python 185 (incl. unused `syborg,serial,modem` subclass) | 0xC0006000–0xC0009000, IRQ 5–8 | **Yes** (serial0 = kernel debug port; others mapped but idle) |
| `syborg,hostfs` | C 1144, MIT (CodeSourcery 2009) | 0xC000A000 | No for boot; `svphostfsdriver.ldd`/`svphostfsy.fsy` are in the ROM and read the platform device's FDT; must fail gracefully [inferred] |
| `syborg,snapshot` | C 171, MIT | 0xC000B000 | No (`svpsnapdriver.ldd` extension pokes its registers at init; reads of an absent device return 0 in 0.9.1) [inferred] |
| `syborg,virtio-net` / `virtio-audio` | C `syborg_virtio.c` 332, MIT; custom MMIO transport, predates virtio-mmio | 0xC000C000 IRQ 9 / 0xC000E000 IRQ 10 | No (ethernet/soundsc drivers load on demand) |
| `syborg,nand` | C 238, MIT | 0xC000D000 | No |
| `syborg,usbtest` | Python 113, not loaded | 0xC000F000 | No |
| `syborg,nvmemorydevice` | Python 233 (**EPL-1.0**, Nokia 2010) + `model-libraries/nvmemmory/nvmemmory.cpp` (**EPL-1.0**) | 0xC0010000, IRQ 12 | **Yes** (`mednvmemory.pdd`: C: drive / ROFS media) |
| `syborg,platform` | C 136, MIT | 0xC0011000, 16 MB window | Keep (trivial; only the hostfs driver reads it) |
| (none) `syborg_dummy.c` | C 57, "Symbian Ltd 2008", no licence text | – | No (logging stub, not in DTS) |

Register semantics the guest relies on (all word registers, index = byte offset / 4; every
device answers reads of unknown offsets with 0 and ignores unknown writes, except NVMemory which
calls `sys.exit`):

**Interrupt controller** (`syborg_interrupt.py`; guest `specific/interrupts.cia`, `syborg.cia`,
`interrupts.cpp`): `0 ID` = 0xC51D0000; `1 STATUS` = number of lines that are both asserted and
enabled; `2 CURRENT` = lowest such line, else 0xFFFFFFFF; `3 DISABLE_ALL` (write); `4 DISABLE n`;
`5 ENABLE n`; `6 TOTAL` = `num-interrupts` (32 from the DTS). Inputs are level-sensitive; output
= any asserted & enabled. The guest IRQ dispatcher reads `CURRENT` and indexes a handler table
with it **without checking for 0xFFFFFFFF**, and never acknowledges the controller: sources
de-assert themselves (timer `INT_STATUS` write, FIFO drains). So the model must guarantee the IRQ
line is only high when `CURRENT` is valid. Guest init writes `DISABLE_ALL`.

**Timer** (`syborg_timer.py`; guest `specific/syborg.cpp`, `assp.cpp`, `interrupts.cia`):
`0 ID` = 0xC51D0003; `1 RUNNING` (write toggles `ptimer_run(oneshot)`/`ptimer_stop`);
`2 ONESHOT` (stop, set, re-run if running); `3 LIMIT` (`ptimer_set_limit(v, reload=1)`);
`4 VALUE` (`ptimer_set_count` / `ptimer_get_count`); `5 INT_ENABLE` (bit 0);
`6 INT_STATUS` (bit 0, write-1-to-clear); `7 FREQ`. Frequency from DTS: **100 kHz**. The guest
loads `LIMIT = 1000` (`K1000HzTickMatchLoad`, despite the name) and declares
`KMsTickPeriod = 10000` µs, so the nanokernel tick is 10 ms and the timing is self-consistent (I
checked this because it looked like a 10× bug; it is not). The tick ISR writes 1 to `INT_STATUS`
(offset 24) before calling `NTimerQ::Tick`.

**RTC** (`syborg_rtc.py`; guest `specific/highrestimer.h`): `0 ID` = 0xC51D0004; `1 LATCH`:
write v ≥ 4 → `offset = data - now`; write v < 4 → `data = (now + offset) / 1000^v`;
`2 DATA_LOW`, `3 DATA_HIGH` read/write halves of the 64-bit `data`. `now` is `vm_clock` in **ns**
and `offset` is initialised as `start_time_seconds * 10^6 - now`, i.e. the plugin mixes µs and ns
(the mainline C version used ns throughout). The guest's `GET_HIGH_RES_TICK_COUNT` writes
`LATCH = 1` and reads `DATA_LOW` as a free-running 1 MHz counter (`KHighResTimerFrequency`), so
only the low 32 bits of `(now_ns + offset)/1000` matter and the absolute value is irrelevant.
Wall-clock time is **not** read from the RTC (`Syborg::SystemTimeInSecondsFrom2000` is a stub).

**Keyboard** (`syborg_keyboard.py`; guest `keyboard/syborg_keyboard.cpp`): `0 ID` = 0xC51D0002;
`1 DATA` pops the FIFO or returns 0xFFFFFFFF; `2 FIFO_COUNT`; `3 INT_ENABLE` (bit 0);
`4 FIFO_SIZE` (16). Input: PC/AT **set-1 scancodes** as delivered by 0.9.1's
`qemu_add_kbd_event_handler`; a 0xE0 prefix is not queued but sets bit 7 of the next code; a
break code (bit 7 set) sets **bit 31** of the queued word. IRQ = FIFO non-empty & enabled. The
guest ISR drains `FIFO_COUNT` entries.

**Pointer** (`syborg_pointer.py`; guest `pointer/syborg_pointer.cpp`): `0 ID` = 0xC51D0006 when
`absolute` (DTS default 1) else 0xC51D0005; `1 LATCH` (write pops the next FIFO entry into the
current registers); `2 FIFO_COUNT`; `3 X`, `4 Y`, `5 Z`, `6 BUTTONS`; `7 INT_ENABLE`;
`8 FIFO_SIZE` (16). Coordinates are 0.9.1's absolute range **0..0x7FFF**; the guest scales
`x / 0x6000` and `y / 0x8000` to the screen. Buttons are 0.9.1's bitmask (1 left, 2 right,
4 middle) [inferred for the bit assignment]. IRQ = FIFO non-empty & enabled.

**Framebuffer** (`syborg_fb.py`; guest `fb/syborg_fb.h/.cpp`): `0 ID` = **0xC51D0006 in the
plugin** (same as the touchscreen; `syborg.h` and mainline say 0xC51D0007; the guest never checks
it); `1 BASE` (guest physical address; write also arms `INT_BASE_UPDATE`); `2 HEIGHT`, `3 WIDTH`
(initialised from DTS `width`/`height`, 300×280 stock, 240×320 in our patched DTB);
`4 ORIENTATION`; `5 BLANK`; `6 INT_MASK` (bits: 1 VSYNC, 2 BASE_UPDATE); `7 INT_CAUSE`
(write-1-to-clear); `8 BPP`; `9 COLOR_ORDER` (0 BGR, 1 RGB); `10 BYTE_ORDER`; `11 PIXEL_ORDER`;
`12 ROW_PITCH`; `13 ENABLED`; palette at word index 0x100 (byte 0x400), 256 entries. The guest
driver writes `ENABLED 0, BASE, BLANK 0, BPP 32, COLOR_ORDER 0, BYTE_ORDER 0, PIXEL_ORDER 0,
INT_MASK 0, ENABLED 1`, reads `WIDTH`/`HEIGHT` once, never writes `ORIENTATION` and never enables
interrupts (grep of the driver). So the only format to support initially is 32 bpp little-endian
words, "BGR" order (0.9.1 renders this to the SDL surface; verify red/blue by screenshot
[inferred which byte is which]). The render engine raises `VSYNC` on every display refresh when not
blanked; irrelevant while `INT_MASK = 0`.

**Serial** (`syborg_serial.py`; guest `serial/syborg_serial.cpp`, `specific/assp.cpp`
`DebugOutput`): `0 ID` = 0xC51D0001; `1 DATA`: read pops or 0xFFFFFFFF, write sends one byte to
the chardev; `2 FIFO_COUNT`; `3 INT_ENABLE` bits: 1 = RX FIFO non-empty, 2 = TX DMA complete
(always true), 4 = RX DMA complete; `4..7` DMA TX/RX address/count. The guest uses **only
`DATA`, `FIFO_COUNT`, `INT_ENABLE = 1/0`** (grep); the DMA registers are unused. `chardev` from
DTS `chardev = "serialN"` → `-serial` slot N. RX FIFO 16 (`can_receive`).

**NVMemory** (`syborg_nvmemorydevice.py` + `libnvmemmory.so`; guest
`mednvmemory/syborg_mednvmemory.cpp/.h`): byte offsets `0x00 ID` = 0xDEADBEEF;
`0x04 TRANSACTION_OFFSET` (sector); `0x08 TRANSACTION_SIZE` (sectors); `0x0C DIRECTION` (1 read,
2 write); `0x10 EXECUTE` (write runs the transfer **synchronously** between the image file and the
guest's shared buffer, then the library callback sets `STATUS` = sectors done and raises the
IRQ); `0x14 SHARED_MEMORY_BASE` (guest physical, from `Epoc::AllocPhysicalRam`);
`0x18 NV_MEMORY_SIZE` (sector count, read); `0x1C SHARED_MEMORY_SIZE`; `0x20 STATUS` (read
**de-asserts the IRQ**); `0x24 ENABLE` (write 1 → plugin resolves a host pointer to the shared
buffer via `qemu.memregion`). Image: `nvmemory/<drive_image_name>` relative to the cwd
(`build/run/`), created zero-filled at `drive_size` (DTS 0x4000000 = 64 MB) if missing; sector
512. Illegal register accesses `sys.exit` the emulator.

**Platform** (`syborg_platform.c`): `0 ID` = 0xC51D1000; `1 TREE_START` = 0x1000; bytes
0x1000.. mirror the DTB (`machine_devtree`), 16 MB window, read-only (writes/unknown reads
`cpu_abort`). Only `svphostfs/driver/svphostfsdriver.cpp` reads it.

### 1.3 Which devices the ROMs actually need

`rom/base_syborg.iby` [verified] loads as kernel extensions: lcd (fb), epointer, keyboard,
`mednvmemory.pdd`, `svphostfsdriver.ldd` (+ `svphostfsy.fsy`, `svphostfs.exe`),
`svpsnapdriver.ldd`; webcamera/soundsc/ethernet are `device=` entries loaded on demand. The
kernel (`kasyborg.dll`) needs intc, timer, RTC, serial0. The N95 hybrid ROM uses the same
baseport (`lcd_n95` build of the fb driver, 240×320) plus the project's stand-ins, which are
software. So the minimum machine is: **intc, timer, rtc, serial×4, fb, keyboard, pointer,
nvmemory, platform**. hostfs/snapshot/virtio/nand can be absent as long as reads return 0 and the
drivers cope (0.9.1 behaves exactly that way for unmapped addresses) [inferred: that they cope;
check `svphostfsdriver` and `svpsnapdriver` init paths in phase 1].

## 2. CPU model

- DTS CPU node `ARM,Cortex-A8@0` → fork `cpu_arm_init("cortex-a8")` (`ARM_CPUID_CORTEXA8`
  0x410FC080). [verified]
- Kernel build (`variant.mmh`, bootstrap `config.inc`): `__CPU_CORTEX_A8N__`, `__VFP_V3`,
  `__CPU_HAS_VFP` (the project enabled VFP for GCCE, `docs/hacks.md`), `MM_MULTIPLE`,
  `CFG_CPU_CORTEX_A8N`, `CFG_CopyFlashToRam`. The multiple memory model on ARMv7 uses ASIDs:
  `xsched.cia` writes TTBR0 and CONTEXTIDR on every context switch ("no TLB flush required due to
  ASID"), `xipc.cia` switches ASID twice per cross-process descriptor copy, `xmmu.cia` uses
  `UTLBIMVA`/`UTLBIALL` (`InvalidateTLBForPage`, `FlushTLBs`) with DSB/ISB. The "ARMV5" in the
  ROM name is the ABI/build name, not the CPU target. The N95 user side is ARM11 (ARMv6) code, a
  subset of what an A8 runs. [verified]
- Modern QEMU (`target/arm/tcg/cpu32.c`, master): `cortex-a8` exists: MIDR 0x410FC080, V7, NEON
  (VFPv3), Thumb2EE, dummy c15, **EL3**, PMU; CTR 0x82048004, CLIDR/CCSIDR for 16 K L1 caches.
  Also available: arm1136(-r2), arm1176, arm11mpcore, cortex-a7/a9/a15. [verified]
- Recommendation: `cortex-a8` with `object_property_set_bool(cpu, "has_el3", false)`. With EL3
  present, `arm_load_kernel` switches a non-`secure_boot` guest to the Non-secure world before
  entry (0.9.1 had no security extensions at all); dropping EL3 keeps CP15 banking and reset
  state closest to what the kernel saw on 0.9.1. [inferred]

## 3. Syborg in mainline QEMU

[verified via GitHub commit search API and GitLab raw files unless noted]

- **Added:** `4af396115a1722dfaf8f7d4ecee59202dd7d5ea8` "Syborg (Symbian Virtual Platform) board",
  Paul Brook, 2009-05-14; `340d96e7ce6e1c2e6d75504e9b4d8153896d5b42` "Syborg virtio bindings",
  2009-05-18. First release containing them: 0.11.0 (Sept 2009) [inferred from dates].
- **Maintained through:** qdev property conversions (Gerd Hoffmann, 2009-08-03, e.g.
  `c230c4e3`, `3c2aed8b`, `51dd5927`, `5cdabc14`, `c4470b25`, `083301fc`), vmstate ports (Juan
  Quintela, Dec 2010: `0c067bbb` keyboard, `4ba673ce` rtc, `80a52680` pointer, `8dc59070`
  serial), memory-API conversions (Avi Kivity `9f7adc31`, Benoît Canet `0520a049`, `9cc60076`,
  `6620cb3c`, `9ca9e358`, `43b8c30d`, `a8a00822`, `7d5ebaf0`, `af687ddc`, Oct–Nov 2011).
- **Removed:** `d396a657baec8c6b7aa0c888746e0e2f78303650` "syborg: drop support for Symbian
  Virtual Platform", Stefan Hajnoczi, 2011-12-12, applied by Anthony Liguori (qemu-devel thread
  2011-12 msg01690/01711; reason: "Symbian has been disbanded and the code is no longer being
  used"). Last release with Syborg: **1.0** (Dec 2011); gone from 1.1 (May 2012). Not listed on
  the "removed features" page (predates it).
- **Files at v1.0:** `hw/syborg.c`, `syborg.h`, `syborg_fb.c`, `syborg_interrupt.c`,
  `syborg_keyboard.c`, `syborg_pointer.c`, `syborg_rtc.c`, `syborg_serial.c`, `syborg_timer.c`,
  `syborg_virtio.c`. **Licence: MIT** (CodeSourcery 2008/2009), same text as the fork's C files.
- **Board at v1.0** (`hw/syborg.c`): default `cortex-a8`; RAM at 0; intc at 0xC0000000 with 64
  inputs → CPU IRQ; rtc 0xC0001000; timer 0xC0002000 IRQ 1 with **`frequency = 1000000`** (our
  DTS says 100000; the guest's 10 ms tick assumes 100 kHz, so keep 100 kHz); keyboard IRQ 2;
  pointer IRQ 3; fb IRQ 4; four serials 0xC0006000–9000 IRQ 5–8; virtio-net at 0xC000C000 IRQ 9
  if a NIC is configured. No DTB, no platform/hostfs/snapshot/nand/nvmemory, `arm_load_kernel`
  with `board_id = 0` (same raw-image path as the fork).
- **Register compatibility:** intc, timer, rtc, serial, keyboard, pointer and fb register maps
  and semantics are the same as the Python plugins (compared one by one above). Differences:
  fb ID 0xC51D0007 (guest does not care); fb renders through `framebuffer_update_display` with
  `pl110_template.h` draw functions, no rotation/blank (fine for us); RTC start offset in ns
  (consistent, unlike the plugin; guest does not care); serial DMA fully implemented (unused).
- **Reviving them on QEMU 11.x:** the device logic is 20–40 lines each and identical to the
  plugins, but every line of glue is from the 1.0 era: `SysBusDevice` init functions,
  `sysbus_from_qdev`, `DeviceInfo` registration (pre-QOM `TypeInfo`), `qemu_add_kbd_event_handler`
  / `qemu_add_mouse_event_handler` (replaced by `qemu_input_handler_register`),
  `qemu_chr_add_handlers` (now `qemu_chr_fe_set_handlers` on a `CharBackend`),
  `graphic_console_init`/`DisplayState` (now `QemuConsole` + `GraphicHwOps`), `cpu_abort` on bad
  offsets, `ptimer_init` without transactions. Verdict: **use them as the reference design and as
  MIT-clean source for the device logic, but write new files**; the effort equals writing from the
  Python, and the result is cleaner. [inferred]
- No modern port of Syborg exists publicly (searched; only the 0.9.1-based forks
  `conexport/symbian_tools_qemu`, `martintrojer/qemu-syborg`, `Phorepoc/qemu-syborg`). [verified
  search, negative result]

## 4. Recommended target and approach

- **Version:** QEMU **11.1.x** (11.1.3 released 2026-10-28 is current; 10.2.4 is the previous
  LTS-ish line). Anything ≥ 8.2 has every feature named in section 6; pick the newest stable and
  pin the tarball/sha in `sources.lock`. If the host distro's `qemu-system-arm` is ≥ 8.2 it can
  serve for API experiments, but the board must be compiled in. [verified versions]
- **Where to build:** on the host (meson/ninja, Python 3, glib ≥ 2.66, a C11 compiler); the
  Debian 6 container cannot build it and 32-bit hosts were dropped in 11.0. Configure
  `--target-list=arm-softmmu --disable-docs` plus whichever display backend is wanted
  (`--enable-gtk` or `--enable-sdl`; headless runs use `-display none`). Plugins are on by
  default. The container stays for building Symbian ROMs only. [verified]
- **Shape:** QEMU has no stable ABI for out-of-tree machine types, so keep a branch of the pinned
  release with a patch series under `patches/qemu-modern/` adding:
  `hw/arm/syborg.c` (machine), `hw/intc/syborg_intc.c`, `hw/timer/syborg_timer.c`,
  `hw/rtc/syborg_rtc.c`, `hw/char/syborg_serial.c`, `hw/display/syborg_fb.c`,
  `hw/input/syborg_keyboard.c`, `hw/input/syborg_pointer.c`, `hw/block/syborg_nvmem.c`,
  `hw/misc/syborg_platform.c`, a `Kconfig` symbol `SYBORG` selected by `ARM` default configs,
  `meson.build` entries, and `include/hw/arm/syborg.h` with the IDs. One `hw/arm/syborg/`
  directory is equally fine. [inferred]
- **Device models:** `TYPE_SYS_BUS_DEVICE` subclasses with a 0x1000 `MemoryRegion` using
  `MemoryRegionOps` (`.valid.min/max_access_size = 4`, `.impl.min_access_size = 4`, little
  endian), qdev properties mirroring the DTS ones (`num-interrupts`, `frequency`, `fifo-size`,
  `width`, `height`, `absolute`, `chardev`, `drive`), a `VMStateDescription` each (needed for
  snapshots, section 6), and `qemu_log_mask(LOG_GUEST_ERROR, …)` where the Python called
  `sys.exit`. Timer: `ptimer_init(cb, s, PTIMER_POLICY_LEGACY)`, all changes inside
  `ptimer_transaction_begin/commit`, `ptimer_set_freq(100000)`. Keyboard:
  `qemu_input_handler_register` with `INPUT_EVENT_MASK_KEY` and
  `qemu_input_key_value_to_scancode(key, down, codes)` to get set-1 codes including 0xE0 (this is
  what `hw/input/ps2.c` does). Pointer: `INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS`, collect
  axis/button events and push one FIFO entry on `sync`; QMP and GUI deliver absolute axes in
  0..0x7FFF already. Serial: `CharBackend` with `qemu_chr_fe_set_handlers` and
  `qemu_chr_fe_write_all`; the board connects `serial_hd(n)`. Framebuffer: `graphic_console_init`
  + `GraphicHwOps.gfx_update`, `qemu_console_resize(width, height)`, draw from guest RAM with
  `framebuffer_update_display` (`hw/display/framebuffer.c`, still in tree, used by pl110) or
  simply copy 240×320×4 bytes per refresh via `cpu_physical_memory_read` into the
  `DisplaySurface`. NVMemory: a `BlockBackend` (`-drive if=none,id=nvmem,file=…,format=raw`
  or qcow2) with `blk_pread/blk_pwrite` on sector×512 offsets and `dma_memory_read/write` to the
  guest's shared buffer; keep the synchronous-completion + IRQ semantics. Platform: expose the DTB
  bytes read-only at +0x1000. [inferred API details; all APIs exist in 11.x]
- **Machine:** `MachineClass` "syborg": `default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a8")`,
  `default_ram_size = 512 MiB`, `max_cpus = 1`, `ignore_memory_transaction_failures = true` at
  first (0.9.1 returned 0 for unmapped reads; flipping it later turns stray accesses into data
  aborts, a useful diagnostic), machine properties `fb-width`, `fb-height` (replacing
  `tools/dtb_set.py`), `dtb` (generic `-dtb` → `machine->dtb`, served by the platform device,
  **not** passed into `arm_boot_info` so it is not loaded into RAM). Wiring: intc output →
  `qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ)`; device IRQ lines →
  `qdev_get_gpio_in(intc, n)` with the DTS numbers. [inferred]
- **ROM loading:** keep `-kernel ROM.img` and call `arm_load_kernel(cpu, machine, &binfo)` with
  `loader_start = 0`, `ram_size`, `board_id = 0`. Modern `hw/arm/boot.c` does exactly what the
  fork did for a raw image: loads at `loader_start + 0x10000`, writes the bootloader stub at 0
  (r0=0, r1=board-id, r2=ATAGs at 0x100), resets PC to 0. [verified] If a non-Linux start is ever
  wanted, the alternative is `load_image_targphys()` plus a reset hook setting PC; not needed.
- **DTB-driven vs hard-coded map:** hard-code the map (it is fixed by the guest `syborg.h`
  constants anyway) and expose only the knobs we change (fb size, RAM size, drive image) as
  machine/device properties. Parsing the DTB for the memory map would re-implement `devtree.c`
  for no benefit; serving it to the guest through the platform device costs ten lines. [inferred]

## 5. Expected speed-up: what changed since 0.9.1 that matters here

Baseline facts about the fork [verified in `build/qemu/qemu-symbian-svp/`]:

- `target-arm/cpu.h`: **`TARGET_PAGE_BITS 10`** in system mode ("The ARM MMU allows 1k pages").
  The soft-MMU TLB therefore works in 1 KB units: each 4 KB guest page needs four `tlb_fill`
  → `get_phys_addr` page walks after a flush, and `tb_find_slow`/`tb_jmp_cache` keys are per
  1 KB page too. `cpu-defs.h`: `CPU_TLB_BITS 8` (256 entries per MMU mode), `NB_MMU_MODES 2`,
  `TB_JMP_CACHE_BITS 12`. `exec.c`: static 32 MB code buffer; a full `tb_flush` when it fills
  (`-tb-size` exists in this fork's `vl.c`; `info jit` in the monitor shows the flush count).
- `target-arm/helper.c` CP15 writes: `c8 op2=1` (invalidate single entry by MVA) →
  **`tlb_flush(env, 1)`** (the per-page code is `#if 0`'d with the comment "wrong for large pages
  and sections"); `c8 op2=2` (by ASID) → full flush; `c8 op2=3` → full flush;
  `c13 op2=1` (CONTEXTIDR) → full flush when the ASID changes; `c3` (DACR) → full flush;
  `c13 op2=0` (FCSE) → full flush. A `tlb_flush` also wipes `tb_jmp_cache`.
- The guest kernel (verified in `memmodel/epoc/multiple/arm/*.cia|.cpp`) issues `UTLBIMVA`
  from `InvalidateTLBForPage` at 37 call sites, including `Alias`/`RemoveAlias` used for every
  cross-process descriptor access in IPC (`xmmu.cpp:2667, 2692`), page unmap/commit and the
  temp-mapping helpers; it changes ASID in `xsched.cia` (every context switch) and `xipc.cia`
  (twice per IPC copy). So on 0.9.1 **every IPC and every page-table change is a full TLB +
  jump-cache flush**, followed by refills at 1 KB granularity. That matches the gdb profile in
  `emulator-options.md` (`tb_find_slow`, `tlb_fill`/`get_phys_addr`, `__ldb_cmmu`).

What modern QEMU does instead:

- **4 KB soft-MMU pages for ARMv7 guests** (`TARGET_PAGE_BITS_VARY`; 1 KB only for CPUs that
  support subpages). Four times fewer fills and jump-cache entries per page. [inferred: the
  mechanism exists since ~2.9; the exact selection rule should be confirmed in
  `target/arm/cpu.c` `arm_cpu_realizefn`]
- **TLBIMVA/TLBIMVAA flush one page** (`tlb_flush_page`), TLBIALL/TLBIASID flush the CPU
  (`target/arm/tcg/tlb-insns.c`, master) [verified]. CONTEXTIDR writes still call `tlb_flush`
  when the ASID changes (`contextidr_write`, `target/arm/helper.c`, master) [verified], and TTBR
  writes flush only on a 64-bit ASID change (LPAE; not our case) [verified]. So the per-context-
  switch and per-IPC ASID flushes remain; what disappears is the flush per page invalidate.
- **Cheaper flush/refill:** dynamic TLB sizing (per-MMU-index table grows/shrinks with use;
  "tcg: introduce dynamic TLB sizing", Emilio Cota, QEMU 4.0, Jan 2019; backend enabling commits
  `cd7d3cb7`, `ac33373e`, merge `3a183e33`) [verified commits]; victim TLB
  (`88e89a57f985296a6eeb416b2a875072e09d7faa`, Xin Tong, 2014, QEMU 2.1) [verified]; TB lookup via
  the lock-free QHT instead of hash chains (`909eaac9bbc2ed4f3a82ce38e905b87d478a3e00`, Cota,
  2016, QEMU 2.7) [verified]; `goto_ptr`/`lookup_and_goto_ptr` so indirect branches and cross-page
  jumps stay inside generated code (`cedbcb01529cb6cf9a2289cdbebbc63f6149fc18`, and ARM use
  `7ad55b4ffd982c80f26f7f3658138d94cdc678e8`, Cota, 2017, QEMU 2.10) [verified]; default code
  buffer = min(host RAM/8, **1 GiB**) instead of 32 MB (`tcg/region.c`, master) [verified], so
  whole-cache `tb_flush`es essentially stop; out-of-line soft-MMU slow paths and an inlined fast
  path, TCG optimiser (constant folding, liveness, better register allocation), decodetree-based
  ARM front end, code fetch through cached host pointers instead of per-byte `__ldb_cmmu`
  [inferred, well-known history].
- **Not relevant:** MTTCG (single vCPU), KVM/HVF (no 32-bit ARM guests on x86), `-icount` (off by
  default; only needed for record/replay; it slows execution).
- **Tunables:** `-accel tcg,tb-size=<MB>` (default is already large), `-accel
  tcg,one-insn-per-tb=off` (default), `-display none`, no `-d` logging, `-serial file:`;
  `-icount` off. [verified names: `-tb-size` removed in 6.0 in favour of `-accel tcg,tb-size=`;
  `-singlestep` removed in 9.0 in favour of `one-insn-per-tb`]
- **Benchmarks:** no published comparison of 0.9.1-era vs modern QEMU for ARM system emulation.
  The GSoC 2020 "TCG Continuous Benchmarking" work (Ahmed Karaman) measured adjacent 4.x/5.x
  releases and found single-digit-percent differences and some regressions in user mode; it does
  not cover the structural changes above. The expectation here (several-fold on the boot-to-UI
  time, dominated by the 1 KB-page and TLBIMVA effects) is **inferred and must be measured** in
  phase 0 with the text-shell ROM. The ASID flush on every context switch/IPC remains a ceiling
  in both.

Measurement that becomes possible: the modern binary runs natively on the host, so `perf record
-g` on it works (the container's user namespace blocked it before) and the gdb sampler
(`tools/pcsample.py`) still works. [verified that the blocker was the container; perf itself
inferred]

## 6. Tooling wins with a modern QEMU

All [verified] against QEMU docs / `qapi/*.json` on master unless noted.

- **Headless screenshots and input via QMP** (`-qmp unix:…,server,nowait` + `-display none`):
  `screendump` (since 0.14; `format: png` since 7.1) replaces Xvfb + ImageMagick; `send-key`
  (since 1.3, QKeyCode or raw numbers, `hold-time`) replaces xdotool typing; `input-send-event`
  (since 2.6) sends `abs` x/y in 0..0x7FFF and `btn` events, the exact range the guest pointer
  driver scales from. `boot-test.sh` becomes a small Python QMP client (or `qemu.qmp` package).
- **Serial:** unchanged (`-serial file:` / `-chardev file,…`); add `-chardev socket` for the fake
  modem later if wanted.
- **gdbstub:** `-s -S`/`-gdb`; under TCG, hardware breakpoints and watchpoints are advertised
  without limit; `maintenance packet Qqemu.PhyMemMode:1` reads physical memory (handy with the
  multiple memory model where user addresses depend on the current ASID); single-step IRQ masking
  configurable (`qqemu.sstep`); ARM system registers (TTBR0, CONTEXTIDR…) are exposed to gdb via
  XML [inferred: added around 5.0]. Our `SYBORG_GDB=1|wait` flow carries over unchanged.
- **Snapshots to skip the first N minutes:** `savevm`/`loadvm` (HMP) or `snapshot-save` /
  `snapshot-load` (QMP, since 6.0) and `-loadvm <tag>` at start. Requirements: every device has a
  `VMStateDescription` (write them from day one; the mainline 1.0 files already had them for
  keyboard/rtc/pointer/serial), and a qcow2 block node to hold the state: make the NVMemory drive
  qcow2 (`qemu-img convert -O qcow2 …`), or attach an "orphan" qcow2 (the replay docs recommend
  exactly this for diskless VMs). Alternative without qcow2: `migrate` to `file:` (since 8.2) and
  `-incoming file:…` [verified the transport; `-incoming` with a file URI inferred]. Caveats: a
  snapshot binds to the exact QEMU binary and device versions; host chardev state is not
  restored (serial log just continues); the guest wall clock jumps unless `-rtc` tricks are
  used. For our purpose (resume at "UI shows", then poke) this is ideal. [inferred caveats]
- **TCG plugins** (`-plugin …`, on by default): `hwprofile` (system-mode device register access
  profile per device/offset, which is a free "which registers does the guest touch" audit for
  the new devices), `execlog` (per-instruction trace with `afilter`/`ifilter`, optional register
  dumps), `hotblocks`/`hotpages`, `stoptrigger` (stop at an address or instruction count),
  `insn` (instruction counts, usable as a determinism check between runs), `lockstep`.
- **Record/replay** (`-icount shift=auto,rr=record,rrfile=…`, `blkreplay` on every block device,
  `-net none`, `rrsnapshot`): gives deterministic re-runs and `reverse-stepi`/`reverse-continue`
  in gdb. All our devices are deterministic given the recorded inputs (chardev input, keyboard,
  pointer, host clock are recorded). Cost: `-icount` makes execution slower and changes guest
  timing; keep it as an opt-in debugging mode, not the default loop.
- **Display options:** `-display none` (QMP screendump still works), `-display gtk`/`sdl` for
  interactive sessions (`hybrid-run.sh`), `-vnc :0` for remote viewing.

## 7. Risks and gotchas

[verified where a file is named; otherwise inferred]

CPU behaviour:
- **EL3/TrustZone:** modern `cortex-a8` has EL3; `arm_load_kernel` moves a non-`secure_boot`
  guest to Non-secure before entry. Use `has_el3=false` (closest to 0.9.1) or set
  `binfo.secure_boot = true`. Watch for the kernel touching Secure-only CP15 registers.
- **Exclusive monitor:** 0.9.1 implements LDREX/STREX with `mark_exclusive`/`test_exclusive`
  helpers (`target-arm/translate.c`): STREX succeeds whenever the address matches. Modern QEMU
  compares the loaded value with memory (cmpxchg-style), so a STREX after an intervening write
  fails, as on hardware. The `docs/hacks.md` item "exclusive monitor not cleared on context
  switch" stays benign (a stale monitor can only make a STREX succeed when the value is
  unchanged), but code that relied on the old always-succeed behaviour would now spin; unlikely.
- **Alignment:** modern QEMU enforces ARMv7 alignment faults for LDM/STM/LDRD/STRD/exclusives
  (SCTLR.A=0 still allows unaligned LDR/STR); 0.9.1 silently performed them. ARMv5-era user code
  in the N95 image doing misaligned multi-word accesses would now take data aborts; watch the
  panic/exception trace in phase 2.
- **VFP/NEON:** both expose VFPv3; the kernel must set CPACR cp10/11 and FPEXC.EN (it does, VFP
  works on 0.9.1). Lazy VFP context switching relies on UNDEF traps when FPEXC.EN=0; modern QEMU
  raises them correctly. MVFR/FPSID values are the hardware ones (A8); the kernel's `__VFP_V3`
  build expects that.
- **CP15 details:** cache maintenance (c7) is a no-op in both; `mcr p15,0,r0,c7,c0,4` (used by
  the guest idle loop, `interrupts.cia`) is treated as WFI by modern QEMU for v6/v7; c15 writes
  are ignored (`ARM_FEATURE_DUMMY_C15_REGS`); CTR/CLIDR/CCSIDR differ from 0.9.1's defaults
  (`cp15,ctr` etc. were settable from the DTS, unused), which only affects cache-size-driven
  loops. PMU present (harmless). DACR writes still flush the TLB in both.
- **ASID semantics** are the same (flush on CONTEXTIDR change) so no behaviour change, only
  cost.
- **Page-table format:** the kernel uses the ARMv6 format with SCTLR.XP=1 (no 1 KB tiny pages);
  both emulate it; modern QEMU adds TEX remap/AFE support if the kernel enables it (it does not
  on 0.9.1 today).

Device semantics to replicate exactly:
- Interrupt controller: `CURRENT` and the IRQ line must change atomically; the guest dispatcher
  indexes `Handlers[CURRENT]` with no spurious check.
- Timer: `LIMIT` write reloads immediately (`ptimer_set_limit(…, 1)`); `RUNNING=1` with
  `ONESHOT=0` is periodic; `INT_STATUS` is W1C; 100 kHz, period 10 µs.
- RTC: implement the plugin's exact arithmetic (`offset = start_s*1e6 - now_ns`, `LATCH v<4`
  divides by 1000 v times, `v≥4` sets); the guest needs a monotonic µs counter in the low 32
  bits.
- Framebuffer: 32 bpp little-endian words, `COLOR_ORDER 0`; verify channel order with a
  screenshot (`run33-first-ui…` style evidence exists for comparison); `WIDTH`/`HEIGHT` must
  report the configured 240×320 before the guest enables the display; no rotation needed.
- Keyboard: set-1 scancodes with 0xE0 folded into bit 7 and break in bit 31; FIFO 16, drop on
  overflow.
- Pointer: absolute 0..0x7FFF, `LATCH` pops into the current registers, FIFO 16.
- Serial: `DATA` read on empty FIFO returns 0xFFFFFFFF; `INT_ENABLE` bit 1 is permanently true
  if enabled (the guest only sets bit 0); the `syborg,serial,modem` subclass is unused.
- NVMemory: `EXECUTE` must complete the transfer before returning (the guest ISR reads `STATUS`
  and expects the sector count); `STATUS` read lowers the IRQ; `NV_MEMORY_SIZE` in sectors; image
  size 64 MB default, 512-byte sectors; `ID` 0xDEADBEEF.
- Unmapped addresses: 0.9.1 returns 0 / ignores writes without logging; the hostfs and snapshot
  drivers in the ROM probe their devices at boot. Start with
  `ignore_memory_transaction_failures = true`.
- Timing: 0.9.1's `vm_clock` runs on host time; so does modern QEMU without `-icount`; the tick
  is 10 ms either way.

Project workflow:
- `scripts/*` assume `/work` inside the container; the new run path is host-native, so
  `run.sh`/`boot-test.sh`/`hybrid-boot.sh` need a "which QEMU" switch during the transition.
- The DTB patcher (`tools/dtb_set.py`) becomes unnecessary for fb size; keep producing the DTB
  for the platform device (hostfs reads `host-path`/`drive-number` from it).

## 8. Quick wins available in the old QEMU meanwhile

Ordered by expected effect; each is a small change to the fork plus a container rebuild
(`scripts/build-qemu.sh`), validated by the text-shell ROM's time to ESHELL. All [inferred];
the facts they rest on are verified in section 5.

1. **4 KB soft-MMU pages:** in `target-arm/cpu.h` set `TARGET_PAGE_BITS 12` for system mode.
   Safe for this guest: the ARMv6/v7 page-table format the kernel uses (XP=1) has no 1 KB pages,
   and Cortex-A8 does not support them. Effect: four times fewer `tlb_fill`/page walks and
   jump-cache misses after every flush. Risk: other ARM boards in the fork (irrelevant to us);
   `get_phys_addr`'s 1 KB-page case becomes unreachable. ~30 minutes.
2. **Per-page TLB invalidate:** in `target-arm/helper.c` CP15 `c8 op2=1` (and `op2=3`), replace
   `tlb_flush(env, 1)` with `tlb_flush_page(env, val & TARGET_PAGE_MASK)` (with 4 KB pages this is
   one call, matching modern QEMU's `tlbimva_write`). Risk: a section (1 MB) mapping invalidated
   by MVA would leave stale 4 KB entries; the kernel uses `FlushTLBs` (full) for section changes
   and `InvalidateTLBForPage` for 4 KB pages (verified call sites), so this mirrors hardware
   intent. Keep `op2=2` (by ASID) and CONTEXTIDR as full flushes. ~30 minutes. Together with (1)
   this removes the largest 0.9.1-specific overhead; it may recover a good part of the modern
   QEMU gain for the boot workload, so measure it before and after as the first data point.
3. **Translation cache:** run with `-tb-size 256` (option exists in this fork) after checking
   `info jit` (needs `-monitor`) for a non-trivial "TB flush count"; if flushes are rare it will
   not matter.
4. **Trace-heavy runs:** a C `syborg,serial` using the fork's `qdev_new`/`qdev_add_registers`
   API (see `hw/syborg_platform.c` for the pattern) removes Python from the byte path; 2–3 hours;
   only helps with kernel tracing on.
5. Already in place: panic-only trace, `-nographic`, serial to file. Nothing to gain from
   `-icount` or from the SDL dummy drivers.

These are stop-gaps; they do not remove Python 2.6 or the container and do not give snapshots.

## 9. Phased migration plan

Effort assumes one person familiar with the project; QEMU API learning is included. Total
**3–5 days** to replace the old QEMU in the daily loop; the doc's 2–4 days is right for the
devices alone.

**Phase 0 — baseline and spike (0.5–1 day).**
Record on 0.9.1: wall time to ESHELL (text-shell ROM) and to the first UI (hybrid ROM), plus a
serial-log milestone table (timestamp each line, e.g. pipe `-serial` through `ts`). Optionally
apply quick wins 1–2 and record again (cheap second data point). Build QEMU 11.1.x on the host.
Implement the machine with intc, timer, rtc, serial×4, a register-only framebuffer (reports
width/height, no drawing), keyboard/pointer register stubs, and the platform device; `-kernel`
the text-shell ROM. *Exit:* serial0 shows the same kernel and EStart milestones as 0.9.1 in the
same order; time to the EStart milestone measured against the baseline. If the ratio is
disappointing, profile with `perf` before continuing.

**Phase 1 — full device set and headless loop (1–1.5 days).**
Framebuffer rendering, keyboard and pointer input, NVMemory on a `BlockBackend`, VMState for
every device, `-machine syborg,fb-width=240,fb-height=320`. Port `boot-test.sh` to QMP
(`screendump` PNG, `send-key`), keep the serial-log and screenshot outputs identical in layout so
existing evidence and tooling compare directly. *Exit:* text-shell ROM reaches ESHELL, a typed
`dir` lists the C: drive from the NVMemory image, screenshots match 0.9.1's in content; hybrid
ROM reaches the current N95 stage ("Starter list runs" / first UI) with a normalised serial-log
diff (timestamps, pointer values stripped) showing the same sequence; boot time recorded.

**Phase 2 — snapshots, gdb, tracing (0.5–1 day).**
`savevm` of the booted hybrid state into a qcow2 (NVMemory image or orphan), `-loadvm` resume
script, verify the UI keeps working after resume and that NVMemory writes after resume are
sane. gdb flow (`SYBORG_GDB`) over the new stub, `Qqemu.PhyMemMode` recipe. A `hwprofile` run to
confirm the guest touches only modelled registers; `execlog` recipe for targeted traces. *Exit:*
resume-to-UI in seconds; `scripts/run.sh`, `boot-test.sh`, `hybrid-boot.sh` default to the new
QEMU with a `SYBORG_QEMU=legacy` escape hatch; `docs/build-log.md` records before/after times.

**Phase 3 — optional extras (1–2 days, as needed).**
hostfs (1144 lines of MIT C to re-base on modern APIs; or drop it and use NVMemory images),
the Syborg virtio transport for net/audio (332 lines MIT; the guest drivers target this layout,
not virtio-mmio), snapshot device, record/replay mode, retiring the legacy QEMU build from
`build-all`.

**Validation against the old QEMU throughout:** same ROM image, same DTB (fb size via machine
property but identical values); compare serial logs after normalisation; compare milestone
timings; compare screenshots; after any divergence, bisect by device (swap one modern device for
a register-trace of the old one using `hwprofile` on the new side and `DEBUG_*` prints on the old
side). Keep the old container runnable until phase 2 exits.

## Sources

Local (verified by reading):
- `docs/emulator-options.md`, `docs/hacks.md`, `scripts/run.sh`, `scripts/boot-test.sh`,
  `scripts/build-qemu.sh`, `sources.lock`, `container/Containerfile`
- `external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dts` / `.dtb`
- `build/qemu/qemu-symbian-svp/`: `devtree.c`, `python-plugin.c`, `vl.c`, `exec.c`,
  `exec-all.h`, `cpu-defs.h`, `cpu-exec.c`, `config-host.mak`, `hw/arm_boot.c`, `hw/arm_pic.c`,
  `hw/syborg.h`, `hw/syborg_platform.c`, `hw/syborg_dummy.c`, `hw/syborg_*.c` headers,
  `hw/fb_render_engine.h/.c`, `hw/fb_render_template.h`, `hw/ptimer.c`,
  `target-arm/cpu.h`, `target-arm/helper.c`, `target-arm/translate.c`, `plugins/*.py`
- `external/oss.FCL.sf.adapt.Qemu/symbian-qemu-0.9.1-12/qemu-symbian-svp/LICENSE`,
  `build/qemu/model-libraries/nvmemmory/nvmemmory.h`
- `build/epocroot/sf/adapt/qemu/baseport/syborg/`: `variant.mmh`, `kasyborg.mmp`,
  `bootstrap/config.inc`, `specific/syborg.h`, `specific/syborg_priv.h`,
  `specific/highrestimer.h`, `specific/assp.cpp`, `specific/syborg.cpp`,
  `specific/interrupts.cia`, `specific/syborg.cia`, `serial/syborg_serial.cpp`,
  `fb/syborg_fb.h/.cpp`, `keyboard/syborg_keyboard.h/.cpp`, `pointer/syborg_pointer.h/.cpp`,
  `mednvmemory/syborg_mednvmemory.h/.cpp`, `svphostfs/driver/svphostfsdriver.cpp`,
  `svpsnapdriver/src/svpsnapdriver.cpp`, `rom/base_syborg.iby`
- `build/epocroot/sf/os/kernelhwsrv/kernel/eka/memmodel/epoc/multiple/arm/`: `xsched.cia`,
  `xipc.cia`, `xmmu.cia`, `xmmu.cpp`, `xmonitor.cia`

Upstream QEMU (fetched 2026-11-05):
- Removal thread: https://lists.nongnu.org/archive/html/qemu-devel/2011-12/msg01690.html
  (and msg01711); removal commit `d396a657baec8c6b7aa0c888746e0e2f78303650`
- Addition commit `4af396115a1722dfaf8f7d4ecee59202dd7d5ea8`, virtio bindings
  `340d96e7ce6e1c2e6d75504e9b4d8153896d5b42` (GitHub commit search
  `https://api.github.com/search/commits?q=repo:qemu/qemu+syborg`)
- v1.0 sources: https://gitlab.com/qemu-project/qemu/-/raw/v1.0/hw/syborg.c,
  `syborg_interrupt.c`, `syborg_fb.c`, `syborg_serial.c`, `syborg_timer.c`, `syborg_rtc.c`,
  `syborg_pointer.c`, `syborg_keyboard.c`
- Master sources: https://gitlab.com/qemu-project/qemu/-/raw/master/target/arm/helper.c
  (`contextidr_write`, `vmsa_ttbr_write`),
  https://gitlab.com/qemu-project/qemu/-/raw/master/target/arm/tcg/tlb-insns.c,
  https://gitlab.com/qemu-project/qemu/-/raw/master/target/arm/tcg/cpu32.c,
  https://gitlab.com/qemu-project/qemu/-/raw/master/hw/arm/boot.c,
  https://gitlab.com/qemu-project/qemu/-/raw/master/tcg/region.c,
  https://gitlab.com/qemu-project/qemu/-/raw/master/qapi/ui.json,
  https://gitlab.com/qemu-project/qemu/-/raw/master/qapi/migration.json
- TCG history commits: victim TLB `88e89a57f985296a6eeb416b2a875072e09d7faa`; QHT
  `909eaac9bbc2ed4f3a82ce38e905b87d478a3e00`; goto_ptr `cedbcb01529cb6cf9a2289cdbebbc63f6149fc18`
  and ARM use `7ad55b4ffd982c80f26f7f3658138d94cdc678e8`; dynamic TLB sizing series (backend
  enablers `cd7d3cb7a26297c2ca28da68bbc7aacec0c04d25`, `ac33373e0ed49a815f6906d0882898f1a9d89f24`,
  merge `3a183e330dbd7dbcac3841737ac874979552cca2`)
- Docs: https://www.qemu.org/docs/master/about/removed-features.html (n800/n810/OMAP2/PXA
  removed in 9.2; `-tb-size` → `-accel tcg,tb-size`; `-singlestep` → `one-insn-per-tb`);
  https://www.qemu.org/docs/master/system/replay.html;
  https://www.qemu.org/docs/master/about/emulation.html (TCG plugins);
  https://www.qemu.org/docs/master/system/gdb.html; https://www.qemu.org/ (release list:
  11.1.3 on 2026-10-28)
- Benchmarks (negative result): https://ahmedkrmn.github.io/TCG-Continuous-Benchmarking/
  (GSoC 2020), https://lists.nongnu.org/archive/html/qemu-devel/2017-03/msg01682.html
- Other forks (no modern port): https://github.com/conexport/symbian_tools_qemu,
  https://github.com/martintrojer/qemu-syborg, https://github.com/Phorepoc/qemu-syborg
