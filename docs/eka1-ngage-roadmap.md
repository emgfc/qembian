# N-Gage QD (RH-29, Symbian 6.1, EKA1): boot roadmap

Status 2026-11-10: research only, nothing boots yet. Inputs and earlier analysis are in `eka1/`
(untracked: firmware, `ROM.bin`, Ghidra notes). [V] = checked in the ROM bytes, [I] = inferred.

## 1. What exists outside (prior art)

- **No public EKA1 guest for QEMU exists.** The Symbian Foundation QEMU platform
  (Syborg/SVP, what we use) was EKA2-only.
  - QEMU's `sx1` machine (Siemens SX1: TI OMAP310, ARM925T, Symbian 6.1 / S60 1st Ed., the
    N-Gage's OS generation) is the only Symbian-6.1-era phone in QEMU. Its 2008 patch was
    tested with U-Boot and Linux, not the Siemens Symbian firmware
    ([patch](https://lists.gnu.org/archive/html/qemu-devel/2008-11/msg00933.html),
    [docs](https://www.qemu.org/docs/master/system/arm/sx1.html)).
  - It is in our QEMU 11.1.3 (`hw/arm/omap_sx1.c`, `omap1.c`, CPU `ti925t`): a template for
    a board file, not hardware we can reuse (UPP is not OMAP [I]).
- **The documented EKA1 low-level boot is WindEmu** (Psion 5mx, EPOC R5 = EKA1): original ROM
  to GUI in about 3 days ([write-up](https://wuffs.org/blog/building-a-psion-emulator)). The
  method carries over directly:
  - ROM at 0, RAM, plus only the registers the bootstrap/kernel touch.
  - Unimplemented registers must not read 0xFFFFFFFF (busy loops).
  - Kernel export names come from the SDK's `EKERN.LIB`, and struct layouts from SDK stabs.
  - Kernel ordinals differ between versions, so an EKA2 or other-version table must not be
    used.
- **[rengage](https://github.com/JaCzekanski/rengage)** (2017, Unicorn, 2 commits) is an N-Gage
  stub. It contributes only a memory map:
  - an 8 KB boot ROM at 0, which it needs dumped from a device and we don't have;
  - ROM at 0x50000000, superpage at 0x40000000, MMIO at 0x58000000-0x5EFFFFFF, high vectors.
- **EKA2L1** runs N-Gage games by high-level emulation (its own C++ kernel). It is no help
  for a real boot. Its `epoc6.def` holds name hashes, not ordinals.

## 2. Facts from this ROM that shape the plan

- [V] The CPU is ARMv4T (920T or 925T). The ROM header is at flash 0, and the jump table
  there is the Symbian bootstrap (`b 0x5000032c`).
- [V] Kernel data is at 0x80000000-0x80400000. `ekern.exe` is at 0x5031AF9C (entry
  0x5031B054). The SVC table is at 0x503210DC (237 entries) and the export table at
  0x50342068 (506 ordinals).
- [V] The bootstrap uses physical MMIO at 0x0C000000 (0x0C004000, 0x0C120000, 0x0C150000) and
  maps it at virtual 0x58000000. It also uses 0x0D000000, 0x40001000 and 0x10xxxxxx (SDRAM? [I]).
- [V] Literal MMIO windows in the kernel-side modules (virtual addresses, 64 KB windows):

  | module | windows |
  |---|---|
  | `ekern.exe` | 58120000, 58200000, 59010000, 591c0000, 591d0000, 59a00000, 5aff0000 |
  | `ecust.dll` (variant) | 58000000, 58120000, 58140000, 58150000, 59200000, 59a00000 |
  | `lcd.ldd` | 58120000 |
  | `medint.pdd` (internal flash media?) | 58120000, 59a00000, 5aff0000 |

  `rtc.dll`, `ekeyb.dll`, `euart1.pdd`, `audio.ldd` and `medmmc.pdd` have no MMIO literals:
  they reach hardware some other way (the NOS, or ecust helpers [I]).
  - About 11 windows in total: a small hardware surface for the Symbian side.
- [V] **The GSM stack runs on the same ARM.** About 3 MB of Nokia's own NOS/CMT code
  (`ENOS/CMT_SW/...`, strings at ROM 0x2C2A49-0x30AD7A) sits below the Symbian tables.
  - It is started by `nosstarter.ext` (148 bytes).
  - Symbian talks to it over in-memory ISI: `nosdriver.ldd`, `helper.ldd`, `phonet.dll`,
    `pnmsg.dll`, `isimsg.dll`, `phonetsy.tsy`.
  - No Symbian module holds a direct pointer into the NOS region, so the link goes through
    the kernel or RAM tables [I].
- [V] The display is 176x208, 12 bpp (ROM header).

## 3. Decision: original EKA1 kernel on a new QEMU board

Recommended: run the **original** bootstrap, `ekern.exe` and `ecust.dll` on a new QEMU machine,
`ngage`, which models only the hardware they touch. Replace the NOS with a stand-in that speaks
ISI to our QEMU modem.

Rejected for now: an "EKA1 personality" on our EKA2 kernel.
- There is no EKA1 source.
- Every EKA1 kernel-side module (variant, LDDs, PDDs, NOS driver) would need a rewrite, since
  the kernel API is different.
- EKA2L1 already covers high-level emulation.

## 4. Phases (each with an exit test)

Status 2026-11-11 (details: [eka1-ngage.md](eka1-ngage.md)):

| Phase | Status |
|---|---|
| P0 | done for what booting needed: the user side named, 98 kernel exports named |
| P1 | done |
| P2 | done |
| P3 | done as a stand-in: the stub plus the ISI reply table; nothing on the boot path is unanswered |
| P4 | done: C: on the internal flash (persistent), screen (the screen chunk, H-ngage-fb), keypad. The UI reaches the idle screen |
| P5 | not started: the board runs from its own tools, `eka1/tools/ngage_*.py` |
| P6 | open: MMC/game cards, audio, the LCD DMA, the power key |


**P0: Ground truth in Ghidra.** About 2-3 days. Fixes the naming that failed before.
1. **SDK libraries.** The owner's archive is `/mnt/trash/symbian`. Checked 2026-11-10:
   - [V] `symbian_sdk_series60_1.2.tar.gz` (Symbian 6.1) has 514 ARMI user-side import
     libraries: `euser`, `efsrv`, `hal`, `etel`, `esock`, `ecuart` and the rest.
     - These name every user-side DLL in the ROM directly.
     - Its headers include `e32rom.h`, `e32svr.h`, `f32fsys.h` and `d32comm.h`.
     - It has no `EKERN.LIB` and no kernel headers.
   - [V] `_s600.9Sdk.zip` (Series 60 0.9, **Symbian 6.1**) has `Winsb.cab` with the WINS
     (x86, Borland) emulator's `EKERN.LIB` and `EKERN.DLL`.
     - The DLL has 244 named exports, the real 6.1 kernel API: 76 `Kern::`, 31 `DThread::`,
       24 `Plat::`, 13 `DProcess::`, plus `DMediaDriver`, `DLogicalChannel`, `DTimer`,
       `TTickLink`, `DPower*`, `TDfc` and others.
     - The ordinals are the emulator's, not ARM's. The ARM ekern has 506 exports (ASSP
       helpers and GCC extras).
   - [V] `uiq_2_1_sdk_wins.zip` (Symbian 7.0, still EKA1) has the full EKA1 kernel headers
     in `epoc32/include/KERNEL/`: `K32STD.H`, `M32STD.H`, `P32STD.H`, `K32HWA.H` and
     `K32POWER.H`.
     - It also has reference ASSP headers (Integrator AP, SA1100, Cotulla, template ASSP)
       and a WINS `EKERN.LIB`.
     - These give prototypes and object layouts; check offsets against the 6.1 ROM.
   - No archive found so far has an ARM `EKERN.LIB`. The S60 2.x, S80 and 7710 SDKs are
     InstallShield `data2.cab` archives that need `unshield` to open. Even there an ARM
     library would be 7.0s, with different ordinals.
   - [V] User side done: `eka1/tools/name_from_lib.py <S60 1.2 armi/urel>` names 24,606
     exports in 372 ROM modules. Only `EwSrv.exe` is skipped (one export, while the library
     has 31). `--apply` labels the program in Ghidra.
     - Checked on EUSER: the stubs behind `User::WaitForAnyRequest`/`WaitForRequest` reach
       SVC 0x4D, `User::Adjust`/`AllocLC` reach 0x6C (heap), `CActiveScheduler::Add`/
       `Current` reach 0x74, and `User::IsRomAddress` reaches 0xBC. Each matches the exec
       identity found independently.
   - [V] The ARM `ekern` export order is sorted by **GCC-mangled name**, so the method
     name comes first. Evidence from the behaviour-identified lookups:
     - `Kern::ChannelFromHandle` 44 < `ChunkFromHandle` 50 < `Library…` 189 <
       `Mutex…` 245 < `Process…` 293 < `Semaphore…` 339 < `Server…` 342 <
       `Session…` 343 < `Thread…` 441 < `Timer…` 448.
     - `ServerFromHandle` and `SessionFromHandle` are adjacent, and 6.1 has no
       `Kern::SessionFind` that would sort between them.
     - [V] `eka1/tools/bor2gcc.py` converts all 244 Borland names to GCC 2.9 mangling.
       `eka1/tools/ekern_align.py <WINS EKERN.DLL>` writes `analysis/ekern-align.tsv`.
       - Anchors live in `eka1/tools/ekern_evidence.tsv`, one row per export with its code
         evidence. Families used: handle lookups and `*Find` by container address,
         container getters, LDD/PDD pairs by code block, ctors/dtors by vtable, the
         `KErrDied` check of `DThread::Read`/`Write`, `TTrap`, the DTimer/TDfc/TSecondLink
         queues, and flag getters. Rows marked `-` are WINS-only names (`_E32*`).
       - [V] 2026-11-10: 86 anchors plus 12 forced by the sort = **98 kernel exports
         named** and applied in Ghidra (`--apply`). No anchor breaks the sort order.
         143 names still have only an ordinal range (`analysis/ekern-align.tsv`).
     - Parked: the names are not needed to boot (the original kernel runs native).
       Narrow further only when a boot problem lands in an unnamed export.
2. **Naming tool.** Write `eka1/tools/name_from_lib.py`. It reads the ordinal and name pairs
   from the SDK `.lib` files (GCC 2.9 / dlltool import archives) and applies
   `ordinal -> export table -> address -> name` through the REST API on port 8089.
   - Verify each library on a few leaf functions before applying the whole set.
   - If there is no `EKERN.LIB`: name the kernel by its callers. The ordinals that
     `ecust`/LDDs/PDDs import, plus the SDK `k32std.h`/`d32*.h` prototypes, give their
     roles.
3. **Hardware map.** Write `eka1/hw-map.md`. Per window: who reads and writes which offset,
   the values expected, polling loops, and which interrupt lines the IRQ handler
   (0x5032178c) dispatches. Do the same for the CP15 operations.
4. **NOS interface.** Map what `nosstarter.ext` starts, and how `nosdriver.ldd`/`helper.ldd`
   and the MMIO-less drivers (keys, RTC, UART, audio) reach the NOS.

Exit: names for EUSER and (ideally) EKERN in Ghidra, plus a hardware map with [V]/[I] marks.

**P1: QEMU board `ngage`.**
- Add `src/qemu-syborg/hw/arm/ngage.c`, built into the same `qemu-system-arm` next to
  `syborg`.
- CPU `ti925t`; `ROM.bin` as flash at physical 0; SDRAM (16 MB on the real phone); the
  0x0C000000 window as a "logging unknown device" that returns benign defaults.
- Run with gdbstub, and add an `-d` style register log.

Exit: the bootstrap turns the MMU on and jumps to `ekern.exe` at 0x5031B054.

**P2: Kernel alive.**
- Model the interrupt controller, tick timer and clocks/PLL status that the variant waits on.
- Find the variant's debug output (UART or `DebugPrint` path) and send it to serial.
- Give gdb kernel symbols: the Ghidra names exported to an ELF/`add-symbol-file` script.

Exit: the null thread idles, timers tick, and `EStart.exe`/`EFile` start (seen in debug
output or by a breakpoint).

**P3: NOS stand-in.**
- Add a code patch (declared hack, `H-nos-none`) so `nosstarter.ext` doesn't start the real
  NOS.
- Write a small ISI peer: a QEMU device, plus a tiny ARM stub where the Symbian side
  expects NOS entry points.
- Route phonet ISI to the existing `syborg_modem.c` logic. The 2004 ISI versions differ from
  BB5, so they get their own reply file.
- Answer the NOS calls from the keys/RTC/UART/audio drivers.

Exit: system servers start without waiting on the modem, and `isi.py`-style logs show no
unanswered requests on the boot path.

**P4: Storage, screen, keys.**
- Model the internal flash media behind `medint.pdd` (C:, likely NOR flash, QEMU pflash) and
  the RAM drive.
- Model the LCD controller (`lcd.ldd`): 176x208 at 12 bpp to the QEMU console.
- Add key input on the path P0 found.

Exit: the S60 standby screen shows, and soft keys and menu keys work.

**P5: Into the device model.**
- Add `models/nokia-ngage-rh29` with a new `dct4-s60` family (never `bb5-s60`).
  `tools/device.py build` composes `ROM.bin` and code patches only, with no kernel build.
- Support sydb for start, stop, screenshot, keys, gdb and catches. The agent doesn't run on
  EKA1.
- Add the smoke test and `docs/devices/nokia-ngage-rh29/bring-up.md`.

Exit: `SYBORG_DEVICE=nokia-ngage-rh29 sydb phone --fresh` reaches the standby screen.

**P6: Later.** MMC (`medmmc.pdd`, for game cards), audio, camera/BT off, speed.

## 5. Risks

- UPP registers are undocumented, so expect WindEmu-style guessing from the driver code.
  The small MMIO footprint in section 2 keeps this bounded.
- The NOS interface may be a shared-memory mailbox with interrupts, not calls. P0 step 4
  decides how big P3 is.
- 920T vs 925T CP15 cache/TLB operations: `ti925t` may need small CP15 additions.
- If the bootstrap needs the internal 8 KB boot ROM (rengage loads one), we enter at the ROM
  jump table with registers preset instead. Check in P1.
