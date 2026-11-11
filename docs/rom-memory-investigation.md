# ROM in its own memory region: investigation (2026-11-10)

Question (owner): real phones keep the XIP core image in memory of its own, apart from RAM. Can the
Syborg board give the hybrid ROM a dedicated read-only region, so that the guest's RAM is the phone's
RAM size and layout instead of RAM + core image?

Short answer: yes, and it is cheap. The generic EKA2 bootstrap and our Syborg PSL already contain the
"running from ROM/FLASH" path, with the ROM bank at 0x40000000 (`KHwNorFlashBaseAddr`). The board needs
a ROM region there, plus a two-word reset stub. No bootstrap change is needed for a first boot. One caveat
changes what "match the phone" means: the N95 8GB, N8 and the other NAND phones do **not** execute their
core image from separate memory. They copy the unpaged part into RAM and demand-page the rest into RAM.
A dedicated ROM region therefore gives *more* free RAM than the phone has, unless we reserve the
phone's ROM footprint in RAM (bootstrap hook `ReservePhysicalMemory`, now empty).

Tags: [verified] = read in source, a binary or a header; [inferred] = reasoning or memory, not checked.

## 1. How it works today

**QEMU board** (`src/qemu-syborg/hw/arm/syborg.c`)
- RAM: one region `machine->ram` at physical 0, size `-m` (default 168 MB) [verified]. sydb backs it with
  `memory-backend-file` of `board.ram_mb` MB in /dev/shm (`tools/sydb/daemon.py:287`) [verified].
- ROM: `scripts/run-modern.sh:83` passes `-kernel $ROM` (hybrid.img). Every launcher (sydb, syb_run,
  syb_gdb, run-gui) goes through run-modern.sh [verified by grep]. `arm_load_kernel` loads it raw at
  `loader_start + 0x10000` = **physical 0x10000, inside RAM**, with QEMU's boot stub at 0 [verified for
  loader_start=0 and the comment in syborg.c; the 0x10000 offset is QEMU's raw-image KERNEL_LOAD_ADDR
  [inferred from QEMU's arm boot]].
- The RAM size reaches the guest through the platform device's RAM_MB word (0xc0011000 + 12)
  [verified].
- The ROFS flash window is `memory_region_init_rom("syborg.flash")` at 0x60000000, `board.flash.size_mb`
  (1..1024 MB, default 128). It is filled by the board itself from `syborg-platform.flash-file=<rom>.rofs`
  (`tools/device.py qemu_args`), because the generic loader caps raw images at the RAM size. MEDROFS.PDD
  reads it, and no RAM is used for it [verified]. (`docs/hacks.md` H-rofs-flash still says
  `-device loader`, which is outdated.) **This window is already the pattern we need for the core image.**
- Free physical slots: RAM 0..ram_mb (548 MB at most today = 0x22400000), nothing at 0x40000000..0x5FFFFFFF,
  flash at 0x60000000..up to 0x9FFFFFFF, peripherals at 0xC0000000 [verified].

**Bootstrap** (`build/epocroot/sf/adapt/qemu/baseport/syborg/bootstrap/syborg.s`; syborgmv and syborgfx
are identical copies [verified by diff]; patches 0010, 0016)
- `DoInitialiseHardware` reads RAM_MB (falls back to `KHwRamSizeMb` = 168) and puts the super page at
  top of RAM - 0x2000 [verified].
- `GetRamBanks` returns one bank `{0, RAM_MB<<20, RAM_VERBATIM}` [verified].
- In the generic `bootmain.s`, `TestRamBank3` sets `KBootFlagRunFromRAM` when the PC lies inside a RAM
  bank. On that path ("Running from RAM") it excises `[iCodeBase, +iUncompressedSize rounded to 64 KB)` from
  the RAM list with `ExciseRamArea`. iPageableRomStart is 0 because romcompose turns paging off
  [verified: bootmain.s 289-327, romcompose.py 328-330]. Excised RAM is "never again identified as
  RAM" [verified, comment bootmain.s ~481], so the kernel's total RAM is ram_mb minus the ROM.
- `GetRomBanks`/`SetupRomBank` are used only on the *other* path, "Running from ROM/FLASH". The table is
  `ROM_BANK 0x40000000, 32 MB, ERomTypeXIPFlash`. `SetupRomBank` replaces it with base = PC rounded down to
  1 MB and size = `TRomHeader.iRomSize` [verified]. `CalcRomLinBases` computes linear = iRomBase - (header
  phys - bank base) and maps it with `BTP_Rom` [verified]. `RestartEntry` already jumps to
  `KHwNorFlashBaseAddr` [verified]. **The Syborg PSL was written for XIP from NOR at 0x40000000. We
  simply never boot that way.**

**What the guest sees today** [verified figures from profiles and image sizes; totals inferred from the
excise rule]

| device | ram_mb (board) | ram_mb_real | hybrid.img | kernel total RAM ~ |
|---|---|---|---|---|
| N95 8GB (multiple) | 168 | 128 | 60.7 MB | 110 MB |
| N8 011.012 (flexible) | 172 | 128 | 43.9 MB | 128 MB |
| N8 111.040 (Belle) | 172 | 128 | 37.1 MB | 135 MB |
| 701 / 808 | 548 | 512 | 37.5 MB | 510 MB |
| N73 (moving) | 68 | 64 | 25.4 MB | 43 MB |

So today *free* RAM was tuned to the phone (N95: 93.7 MB measured against "~90 MB", H-ram-168), but
HAL total RAM and the physical layout do not match the phone.

## 2. What a dedicated ROM region needs

**QEMU** (all [inferred] design, APIs [verified] in QEMU 11.1.3)
- `memory_region_init_rom(rom, NULL, "syborg.rom", size, ...)` at **0x40000000**, filled with memcpy from
  `syborg-platform.rom-file=PATH`, exactly like the flash window (syborg.c:287-312). Size = the file
  rounded up to 1 MB, at most 512 MB, so it never overlaps the flash at 0x60000000. A plain ROM region (RAM
  backed, readonly) is better than pflash (we do not need the CFI command set) and better than `rom_device`
  (no MMIO path needed).
- Reset entry: without `-kernel`, the ARM1136/1176 resets to 0 (SCTLR.V=0) [inferred]. Put an 8-byte stub
  at RAM 0 with `rom_add_blob_fixed("syborg.romboot", {0xE51FF004 /* ldr pc,[pc,#-4] */, 0x40000000}, 8, 0)`.
  QEMU rewrites ROM blobs on every system reset, so POWER=2 (reset) boots again [inferred]. Do not call
  `arm_load_kernel` in this mode, or call it with no kernel. The kernel later reuses RAM page 0, which is
  harmless.
- The ROM region is migrated: `memory_region_init_rom` calls `memory_region_register_ram` [verified,
  system/memory.c:3670].

**Bootstrap**: nothing for the first boot. The PC at 0x40000000 is outside the RAM bank, so the
flash path is taken, `SetupRomBank` finds the bank, and the ROM is mapped from there [verified code
path]. The hybrid's iRomSize is 4 KB aligned (N95 0x39EE000, N8 0x29DF000) [verified]. To match the phone's
RAM *use* (section 4), fill `ReservePhysicalMemory` (currently `bx lr`). It writes `(base,size)` pairs at
[R11]. They stay RAM (HAL total = the phone's), but are marked allocated at boot [verified, comment
syborg.s ReservePhysicalMemory]. The size comes from a new platform word (for example RESERVE_MB, word 4)
set from the profile.

**Kernel**
- Multiple model: the kernel never reads the ROM bank list. `KBootFlagRunFromRAM` and `iRomBootData`
  are used only by the bootstrap [verified by grep of memmodel/ and kernel/]. `MmuBase::AllocShadowPage`
  uses `LinearToPhysical` and `SPageInfo::SafeFromPhysAddr`, which handles non-RAM pages, so shadowing
  ROM in flash works [verified, mmubase.cpp:1113-1174].
- Flexible model: `DRomMemoryManager::Init3` claims the ROM pages with
  `MemoryClaimInitialPages(..., aAllowGaps=false, aAllowNonRamPages=true)` [verified, mrom.cpp ~470, mm.cpp:316].
  ROM outside RAM is a supported case. **But** `DRomMemoryManager::AllocShadowPage` calls
  `SPageInfo::FromPhysAddr(shadow->iOriginalPage)` without the Safe check (mrom.cpp ~807), and
  FromPhysAddr is plain arithmetic into the sparse SPageInfo array (mmu.h:968) [verified]. For a page at
  0x40xxxxxx that read lands in an unmapped SPageInfo page, so it is a kernel fault [inferred]. It matters
  only if something calls `Epoc::AllocShadowPage` (run-mode debuggers or code modifiers, ROM patching at
  run time). We call it nowhere (grep of src/ and patches/) [verified]. The fix is a one-line kernel patch
  (SafeFromPhysAddr plus a NULL origPi).
- HAL ROM size comes from `iTotalRomSize`, the sum of the ROM banks on the flash path (sutils.cpp:2552,
  mrom.cpp:427). It has the same value [verified code, inferred value].

**Hybrid composition** (`place = "after-firmware"`, romcompose): it produces one contiguous unpaged image,
linked at iRomBase 0x80000000 (0xF8000000 for moving). It begins with TRomHeader and the bootstrap, and
our kernel side sits after the firmware's user side. Only its physical home changes, so nothing in the
composition changes [verified layout docstring; inferred no change]. One linear->physical bank covers it,
whatever the order inside.

## 3. Constraints

- Linear addresses stay fixed. Only the physical base moves (0x10000 -> 0x40000000). The bootstrap maps
  by bank, so XIP code linked at 0x80000000.. is unaffected [verified mapping code]. No tool assumes ROM
  physical 0x10000 (grep of tools/ and scripts/) [verified]. pcsample, SYBCODE and gdb symbols are
  virtual.
- Demand paging: none of our hybrids is paged. romcompose clears iPageableRomStart, iPageableRomSize and
  iRomPageIndex [verified]. Firmware paging (N95 core, N8) is flattened at build time (rom_unpage.py,
  core-flat.img), so nothing pages from the ROM region [verified profile comments]. Paged ROMs are not
  an obstacle. A future real paging model would use NAND (ROFS window), not this region.
- Patches into ROM: code-patches.txt, gles-patches, gcebe-patches and `--blob` are all applied by
  `tools/romcompose.py --patches/--blob` when the image is built (tools/device.py:712-750) [verified].
  Nothing writes the ROM at run time. The bootstrap already maps the ROM `BTP_Rom` (read-only) today,
  so guest writes into ROM would fault now as well [verified permission; inferred: no guest code relies on
  it]. The QEMU side never writes guest ROM (address_space_write only in serial/nvmem DMA to RAM buffers,
  syborg_gl results) [verified grep].
- The board must not overlap regions: RAM below 0x40000000 (1 GB, enough for every profile), ROM 512 MB
  at most [verified addresses].

## 4. Benefits and risks

**Fidelity: what "the phone's RAM" is** [verified header bytes of core-flat.img; conclusions inferred]
- N95 8GB core: iPageableRomStart = 0x84C000 (8.3 MB unpaged, kept in RAM on the phone). The paged part
  is 49.5 MB. iDemandPagingConfig min/max = 1024/2048 pages (4/8 MB of page cache).
- N8 (011.012, 013.016, 025.007): unpaged 0xAEF000 (11 MB), min 2048 pages (8 MB), max 0 (no limit).
- On the phone the ROM therefore uses about 12 MB (N95) or 19 MB (N8) of its RAM. With ROM outside and
  `ram_mb = 128` but no reservation, N95 free RAM would rise from 93.7 to about 110 MB [inferred: 168-58-93.7
  = 16.3 MB used by the system, unchanged]. Reserving the unpaged size plus min pages (12.3 MB) gives about
  99 MB. The remaining ~9 MB against "~90 MB" is unknown phone-side use; tune it as a separate, labelled
  reservation if wanted.
- N73 (9.1, no paging): the phone held its whole 20 MB core in RAM, so its current model (ROM in RAM) is
  already the faithful one. Make the layout **per device**, not global.
- N8 RAM: the profile says 128 MB `[I: spec sheet]`. From memory the N8 has **256 MB** SDRAM [inferred,
  check before using ram_mb_real].
- NOR-XIP phones (the N-Gage on our EKA1 board, ROM at 0x50000000) are the case where a dedicated ROM
  region is exactly the phone (`src/qemu-ngage`, H-ngage-md5) [verified address].

**Performance** [inferred]: reads and instruction fetches from a RAM-backed readonly region use the same
TLB fast path as RAM. TBs from ROM pages need no dirty tracking. Today the guest never writes ROM pages
anyway, so expect no measurable change. Check boot-to-idle *time* before and after (owner's rule:
measure time, not profile).

**Host memory and snapshots**: total host memory is the same (128 + 60 vs 168 MB for the N95). The ROM block
is migrated, so `sydb snap save` (migrate to `state.bin`) does not shrink, but each snapshot keeps the
ROM it was taken with, which is safer [verified migration of ROM regions; inferred sizes]. An optional
later step is a `_nomigrate` ROM plus a hash check that skips ~40-60 MB per snapshot.

**What breaks**
1. **Old snapshots and seeds** (`build/seeds/*`, instances' `snapshots/`). Their RAM block is 168/172 MB with
   the ROM inside it. Restoring them on a 128 MB + ROM board fails (RAM block length mismatch). They must
   be re-seeded per device, and instance.toml should record the layout [inferred, QEMU migration rule]. The
   owner's phones (C:/E: drives) are unaffected: the drives do not depend on the RAM layout [inferred].
2. **Debug writes into ROM**: QEMU 11's `cpu_memory_rw_debug` writes with `address_space_rw`, not
   `write_rom` (physmem.c:4061) [verified]. A gdb or `sydb dbg write` to a ROM address fails (readonly RAM
   region, write not direct, unassigned ops) [inferred]. Today such writes succeed, because the ROM is RAM.
   Fix: a board property `rom-writable` (memory_region_set_readonly at run time, through qom-set) that
   sydb toggles around a write.
3. **Breakpoints are fine**. sydb inserts hardware breakpoints by default (`-break-insert -h`,
   debugger.py:311, catcher.py:110) [verified]. Under TCG, gdb SW breakpoints (Z0) also go to
   `cpu_breakpoint_insert` and never touch memory (tcg-accel-ops.c:136) [verified].
4. FMM shadow pages (section 2): a latent fault, only with run-time ROM shadowing [inferred].
5. Free RAM fidelity changes per device. Apps with OOM behaviour (browser, Carousel) see more RAM until the
   reservation is set [inferred].

## 5. Plan

| # | change | files | estimate |
|---|---|---|---|
| 1 | board: `syborg-platform.rom-file` (+ `rom-base`, default 0x40000000) -> `syborg.rom` ROM region, memcpy, reset stub at 0; skip `arm_load_kernel` in that mode; `rom-writable` bool for debug writes | `src/qemu-syborg/hw/arm/syborg.c` (+ QEMU rebuild in build/qemu-modern) | 1.5 h |
| 2 | profile key `board.rom.location = "ram" \| "flash"` (default "ram"); `qemu_args` emits `-kernel ROM` or `-global syborg-platform.rom-file=ROM`; run-modern.sh stops adding `-kernel` itself; schema check + unit test | `tools/device.py`, `scripts/run-modern.sh`, `tests/device_py_test.py`, `docs/devices.md` | 1 h |
| 3 | first experiment (below), N95 8GB then N8 | none | 1 h |
| 4 | RAM reservation: platform word RESERVE_MB from `board.rom.ram_reserve_mb` (default 0); `ReservePhysicalMemory` writes one `(top-of-RAM-below-superpage - N MB, N MB)` pair; `tools/device.py` derives N from the firmware header (unpaged + min pages) unless set | `patches/qemu-baseport/0025-bootstrap-reserve-rom-footprint.patch` (syborg.s; the variants share it), `syborg.c`, `tools/device.py` | 2 h + baseport rebuild of 3 variants |
| 5 | FMM shadow-page guard (SafeFromPhysAddr) | `patches/kernelhwsrv/...` (mrom.cpp) | 1 h, only if step 3/6 shows a need |
| 6 | profiles: `ram_mb = ram_mb_real` + `board.rom.location = "flash"` for paged-firmware devices (N95 family, N8, S60 5th, 701/808), keep "ram" for N73; re-seed `build/seeds`; `tests/device-smoke.sh` per device; replace H-ram-168 with H-rom-flash in hacks.md; note the snapshot incompatibility in docs/sydb.md | `models/*/model.toml`, docs | 2-3 h plus smoke runs |
| 7 | sydb: toggle `rom-writable` around `dbg write` to ROM addresses | `tools/sydb/daemon.py` or `debugger.py` | 0.5 h |

**Smallest first experiment** (main session, after steps 1-2; no bootstrap rebuild):
1. `tools/instance.py create romflash nokia-n95-8gb-rm320 --ephemeral --replace` with `[overrides]`
   `board.rom.location = "flash"`, `board.ram_mb = 128`. The daemon then sizes its RAM backend from
   `board.ram_mb` [verified daemon.py:288]. Alternatively, `sydb --session build/sydb-romflash start --fresh`
   on a profile copy.
2. Check: HMP `info mtree` shows `syborg.rom` at 0x40000000. `xp/4wx 0x40000000` shows the TRomHeader jump
   words. The boot reaches idle. `sydb sys_mem` shows total RAM about 128 MB (was ~110) and free RAM about
   110 MB (was 93.7). Time to idle is unchanged within noise. `tests/device-smoke.sh` (idle, Carousel,
   Switch off and restart, which tests the reset stub) passes.
3. Repeat on the N8 (`nokia-n8-rm596`, flexible model, syborgfx): the same checks. Also check
   that `DRomMemoryManager::Init3` does not assert (a kernel fault in the first second would point at
   the non-RAM claim).
4. Failure triage: if nothing prints on the serial port, the reset stub or entry is wrong (gdb `-S`, step
   from 0). If the bootstrap faults, check that PC>>20 gives 0x400 in `SetupRomBank` and that the header is
   at offset 0 of the file.
