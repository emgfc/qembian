# EKA1 phones (N-Gage first) on qembian: feasibility

Study of 2026-11-10. Desk work only: no QEMU, no build. Sources: the `eka1/` workspace (extractor,
reconstructed ROM, analysis notes), byte scans of the extracted modules, local EKA2L1 and QEMU 11.1.3
trees, web search. Tags: **[V]** verified from bytes, files or a cited source; **[I]** inferred.
The Ghidra bridge (127.0.0.1:8089) answered but had an unrelated program open (`sbengine.elf`, N95), so
nothing here relies on Ghidra or its labels.

**The material is the N-Gage QD (RH-29, V 04.10, 09-09-2004), not the original N-Gage (NEM-4)** [V:
`sw.txt` reads `V 04.10 / 09-09-2004 / RH-29`]. No NEM-4 firmware is in the repo. Both are Symbian
6.1 / S60 v1 on Nokia's UPP_WD2 ASIC [I for the QD; the NEM-4 service manual names UPP_WD2].

## Recommendation (short)

1. **Not a personality, and not "our kernel + stand-ins".** EKA1 kernel source was never published (the
   2009 EPL release was EKA2 only) [V, press coverage of the release], there is no public EKA1 DDK
   [I: none found], and Nokia's EKA1 drivers are GCC 2.9 C++ objects against an unnamed ordinal-only
   kernel API (506 `ekern.exe` exports, no names in the ROM) [V]. Options b and c below turn into
   writing an EKA1 kernel.
2. **Recommended route: "UPP-lite" machine.** Run Nokia's EKA1 kernel, variant, drivers and the whole
   user side unchanged on a new QEMU board that models only the UPP_WD2 MCU blocks the Symbian side
   touches (about ten MMIO regions, below). **Replace NOS**, the cellular stack that runs on the same ARM
   core, with a small fake-NOS blob that answers PhoNet/ISI from the host fake modem. This is the
   qembian idea (real user side, fake modem, stand-ins where the hardware is hard) moved one layer down:
   the stand-ins are register models and a NOS replacement, not drivers.
3. **Go/no-go gate first (about 1 week, static):** map the boot path from the ROM header jump table to
   `ekern.exe` and find out what NOS gives the Symbian side (keys? power? UEM over CBUS?). If Symbian
   cannot boot without a live NOS (for example if NOS owns reset, the interrupt controller or the FIQ),
   the cost goes up a lot and **EKA2L1 is the practical answer for N-Gage games**.
4. Expect months, not weeks: a working idle screen is roughly 2-4 months of focused work, with high
   uncertainty, because every register is undocumented.

Main risks: (1) NOS entanglement: NOS runs on the same MCU [V, service manual: UPP_WD2 "handles almost
all of the signal processing and operation controlling tasks of the phone as well as all PDA tasks"], so
the Symbian/NOS boundary is a software interface inside one image, not a bus; (2) undocumented UPP_WD2
registers, so each block is reverse-engineered from ekern/ecust/LDD code; (3) almost no tooling on the
guest side: sydb's agent, kernel traces, `SYBCODE:` lines and IPC strace all come from our EKA2 kernel
and do not exist on Nokia's EKA1 kernel.

## 1. What the RH-29 ROM contains

### Container and layout
- Two Phoenix/DCT4 files: `RH2941.0C1` (MCU flash, 3082 records) and `RH2941.026` (PPM/variant
  overlay: localized resources, ringtones, wallpapers) [V, `eka1/analysis/notes.md`].
- No scrambling or compression; the EKA1 ROM is uncompressed XIP, little-endian ARM [V].
- Flash split in two banks: flash `0..0xF00000` = ROM offset 0; flash `0x02000000..0x02265000` = ROM
  offset `0xF00000..` [V, proven by `sw.txt` and a JPEG landing at predicted offsets].
- Reconstructed image: `eka1/output/rom/ROM.bin`, 18,284,544 bytes, base `0x50000000` [V]. Z: tree
  1060 files, all byte-verified; PPM overlay 1576 files, own virtual base `0x51230000` [V].
- Tools: `eka1/tools/eka1_extract.py` (working extractor and report), `eka1/dct4_extract.py` (older,
  kept), `eka1/tools/identify_kernel.py`; analysis in `eka1/analysis/` (manifests, `files.tsv`,
  `kernel-api-map.tsv`, `kernel-identifications.tsv`, `kernel-entry-guide.md`, `ghidra-notes.md`).

### ROM header (EKA1 TRomHeader) [V, `eka1/analysis/notes.md` section 3]
| off | value |
|---|---|
| `0x00..0x7B` | ARM branch table (bootstrap vectors: reset `b 0x5000032C`, undef, swi, ...) |
| `0x8C` rom_base | `0x50000000` |
| `0x90` rom_size | `0x01170000` |
| `0x94` root dir list | `0x50311000` (1 root, hardware variant `0x05040001`) |
| `0x98/0x9C` kernel data | `0x80000000..0x80400000` |
| `0xA0/0xA4` primary/secondary module lists | 16 / 388 modules |
| `0xB8..0xC0` screen | 176 x 208, 12 bpp |

### Two operating systems in one image
- ROM offsets below about `0x300000` hold the **NOS/CMT cellular software** (434 `ENOS/CMT_SW/...`
  source paths in `0x2C2A40..0x30AD71`: `CORESW/HWDRV/Hw_mdi.c` (MCU-DSP interface), `Hw_mbus.c`,
  `Hw_fbus.c`, `Hw_audio.c`, `Hw_sti.c`, `SIM_SW_DCT4`, `CS.ISA/...`, `ISA_UI/...`) [V].
- Symbian starts at `0x311000` (module tables), `ekern.exe` at `0x5031AF9C` [V].
- Kernel-side PhoNet in the primary list: `phonet.dll`, `isimsg.dll`, `pnmsg.dll`, plus
  `nosstarter.ext` (148 bytes, imports only EUser) and `nosdriver.ldd` [V]. So Symbian talks ISI over an
  in-kernel PhoNet to NOS on the same CPU; nothing in the Symbian tree imports NOS [V], so the link is
  set up at run time (shared memory or function pointers) [I].
- The ekern module entry writes an FIQ override pointer (`0x800004E4`) [V, kernel-entry-guide]; NOS
  using the FIQ is a plausible reading, not proven [I].

### Kernel and hardware-specific parts
- Primary list [V]: `ekern.exe` (161,900 B), `ecust.dll` (variant, 12,696 B), `ecomm.ldd`,
  `ramdrive.pdd`, `rtc.dll`, `nosstarter.ext`, `pnmsg.dll`, `isimsg.dll`, `phonet.dll`, `cbus.dll`
  (CBUS to the UEM power/energy chip), `helper.ldd`, `medint.pdd` (internal media), `ELocal.fsy`,
  `ELocl.dll`, `EUser.dll`, `Hal.dll`. Also `ekdata.dll` (764 B).
- Other kernel-side modules [V]: `lcd.ldd`, `ekeyb.dll`, `EDISP.DLL`, `epbus.dll` + `medmmc.pdd` (MMC),
  `euart1.pdd`, `audio.ldd`, `cameraldd.ldd`/`camerapdd.pdd`, `btldd.ldd`, `hotswapdrv.ldd`,
  `GD1DRV.ldd`, `ERam.fsy`.
- **The ASSP is linked into `ekern.exe`**: ekern itself loads IO addresses (`0x5812xxxx`, `0x5820_0000`,
  `0x5901_0000`, `0x591D_0000`, `0x59A0_0000`) [V, literal scan], so the kernel core is not separable
  from the chip layer; `ecust.dll` adds the board layer [I, matches EKA1's EKERN-per-ASSP + ECUST split].
- IO mapping: virtual `0x58xxxxxx` = physical `0x0Cxxxxxx` (ecust and lcd.ldd carry both forms of the
  same registers, e.g. `0x58121200`/`0x0C121200`) [V for those pairs, I as a general rule].
- IO pages referenced directly by Symbian kernel-side code [V, literal scan; aliases of instruction
  words in EUser/ELocal discarded]:

| virtual page | referenced by | guess |
|---|---|---|
| `0x58004000` | cbus.dll, ecust.dll | CBUS master (UEM) [I] |
| `0x58120000` | ekern, epbus.dll, medint.pdd | interrupt/system control [I] |
| `0x58121000` | ecust, lcd.ldd | LCD interface [I] |
| `0x5814F000`, `0x58150000` | ecust | GPIO / pin control [I] |
| `0x58200000`, `0x59010000`, `0x591D0000` | ekern | timers / MMU-side regions [I] |
| `0x59200000`, `0x59A00000`, `0x59A01000` | ecust, ekern, medint.pdd | shared RAM / flash ctrl [I] |

  `ekeyb.dll`, `medmmc.pdd`, `rtc.dll`, `euart1.pdd` reference no IO address directly [V]: they go
  through ecust/ekern exports, or through NOS (keypad on UEM/NOS) [I]. The Symbian-side register
  surface looks small; the large surface (DSP via MDI, RF, SIM, audio ADSP, UEM charging/ADC) is NOS's.

### CPU and memory model
- ARMv4T code: in `ekern.exe` and `EUser.dll` no CLZ and no `BLX reg` (EUser has 2075 `BX`) [V], i.e.
  built for ARMI/THUMB with GCC 2.9 [I]. Core: ARM920T/ARM925T-class per public specs (sources disagree;
  one says ARM9E for the QD) [I]. Either way an ARMv4T-compatible MMU core.
- CP15 in ekern: 20 writes to c3 (domain access control), c7/c8 cache and TLB maintenance [V]. Many
  domain switches is the signature of EKA1's **moving memory model** on a VIVT ARMv4/v5 cache [I].
- ROM at `0x50000000`, kernel data `0x80000000` [V]; process data run address `0x00400000` [I].

### Kernel interface (useful whatever route is taken)
- SWI dispatcher `0x50321490`, table `0x503210DC`, 237 slots `0x00..0xEC`; bit `0x800000` selects an
  alternate path; slots `0xCD..0xEC` return -5 [V, kernel-entry-guide]. The "executor" call
  (`0xC00076` in EKA2L1's v6 table) takes a function code and an argument block for heavyweight object
  creation (chunks, threads, servers, sessions, LDD loading) [V, EKA2L1 source; I that this ROM matches].
- 506 ordinal-only kernel exports [V]. About 30 exec slots have behaviour-checked identities in
  `eka1/analysis/kernel-identifications.tsv` [V there]; the rest are reference candidates.

## 2. Options

### a) Nokia's EKA1 kernel unchanged, emulate the real hardware
- Full version (UPP_WD2 MCU + DSP + UEM + ADSP + RF, NOS running): blocked. No register documentation
  exists publicly [I: only block-level service manuals]; NOS needs the Lead-family DSP over MDI, the
  SIM, the UEM watchdog/ADC/charger. No DCT4 emulator exists to borrow from [I: searches found only a
  DCT3 emulator]. Years of work.
- **"UPP-lite" version (recommended)**: model only the Symbian-facing MCU blocks (table above, about
  ten pages: interrupt controller, OS timer, LCD interface, CBUS master with a minimal UEM behind it
  (RTC, watchdog kick, power key), GPIO, MMC host, shared RAM) and **do not run NOS**. Cost: each block
  is reversed from ekern/ecust/LDD accesses, then modelled in `src/qemu-syborg/hw/arm/` style. QEMU has
  ARMv4T-capable cores (`ti925t`, `sa1110`; `arm926` is a v5TE superset) [V, QEMU 11.1.3
  `target/arm/tcg/cpu32.c`]. Blockers: whether the bootstrap/ekern waits on NOS (gate step), and how the
  keypad, power and battery status reach Symbian.

### b) Keep the EKA1 kernel core, replace the variant/ASSP/drivers with ours on Syborg devices
- The ASSP is inside `ekern.exe` [V], so only `ecust.dll` and the LDD/PDDs are separable. Writing
  replacements needs: the ekern<->ecust interface (unknown; ecust is 12.7 KB so reversible) [I], the
  EKA1 driver class layouts (DLogicalDevice/DPhysicalDevice/DLogicalChannel, GCC 2.9 vtables and name
  mangling) and the semantics of the ordinals they call. No public EKA1 DDK: S60 v1.x SDKs ship
  user-side headers (`e32std.h`, `e32svr.h`, `d32*.h`) and the WINS emulator, not kernel headers [I];
  Symbian DevKits with the DDK were licensee-only [I]; no 6.1/7.0 source leak was found [V: search,
  absence not proof].
- Still needs register models for whatever ekern's built-in ASSP touches, or binary patches of ekern.
- Verdict: more expensive than (a)-lite, because drivers must be written blind against an unnamed ABI
  with a GCC 2.9 toolchain (gnupoc's Linux build) [I]. Use it only per driver, where a register model is
  harder than a replacement (candidates: `nosdriver.ldd`, maybe audio/camera, which can simply be left
  out of the ROM).

### c) EKA1 user side on our EKA2 kernel with an "EKA1 personality"
- Not a personality (agrees with `docs/uiq.md`). Needed: ~237 exec slots and ~100 executor functions
  with EKA1 semantics (handle model, EKA1 IPC where servers read client memory by address instead of
  RMessage2 IPC args, chunk API, TLS keyed by handle, request completion) [V for the counts, I for
  semantics]; EKA1 ROM structures (TRomHeader/TRomImageHeader differ from EKA2) in our kernel's ROM
  handling [V for difference, from the headers]; Nokia's EKA1 `EFile.exe` and loader flow; and every
  EKA1 kernel-side driver (lcd, keyboard, media, comms, PhoNet) rewritten as EKA2 stand-ins exposing
  EKA1 channel APIs. Our moving-to-multiple memory model difference matters less (data at `0x00400000`
  in both) [I].
- That is a second kernel inside ours: comparable to EKA2L1's EKA1 work plus real drivers. Large, and
  it loses the main gain (real kernel behaviour).

### d) HLE like EKA2L1
- EKA2L1 (GPL-3) already runs N-Gage/S60v1: HLE kernel with a v6 SVC table (~110 entries) and an
  `the_executor_eka1` dispatcher, EKA1 thread bootstrap built from EUser, HLE window server, fbs, fs,
  applist, etc.; it boots from a dumped `SYM.ROM`, not the phone's own server set [V, local tree
  `~/work/EKA2L1/src/emu/kernel/src/svc.cpp`, `libmanager.cpp`]. QD-vs-NEM-4 ROM matters per game [V,
  Batocera wiki].
- For qembian: its EKA1 call list is the best public map of EKA1 exec semantics, usable as a naming
  reference only (project rule: no GPL code copied). Its approach is the opposite of qembian's
  (servers reimplemented, not run).
- If the goal is "play N-Gage games", EKA2L1 is the answer today; qembian-EKA1 makes sense only for the
  real-firmware-on-a-real-kernel goal.

### Choice
(a)-lite, with (b) per driver as an escape hatch, after the gate step. (c) and (d) are rejected for
qembian.

## 3. External projects

| project | covers | licence | use for us |
|---|---|---|---|
| EKA2L1 (github.com/EKA2L1/EKA2L1) | HLE Symbian incl. EKA1 v6 (S60v1, N-Gage, N-Gage QD); EKA1 SVC + executor tables; ROM structures | GPL-3 [V] | exec/executor semantics and names, ROM format cross-check; already in `eka1/references/` |
| QEMU `omap1.c` + `omap_sx1.c` (in our 11.1.3 tree) | Siemens SX1: TI OMAP310 (ARM925T), LCD, MMC, UARTs; an EKA1 Symbian 6.1 S60 phone | GPL-2+ [V] | proof that ARMv4T + ARM925T MMU in QEMU is fine; board skeleton pattern. Not the Nokia ASIC. Whether it boots the SX1 firmware fully: unknown [I] |
| MAME `psion5.cpp` (Ryan Holtz, Ash Wolf) | Psion 5mx, EPOC R5 (EKA1 ancestor) on modelled Windermere hardware | BSD-3 [V, per header quoted in search] | precedent that an unmodified EKA1 kernel boots on modelled registers; EKA1 bootstrap expectations |
| WindEmu (github.com/Treeki/WindEmu) | same 5mx, standalone | not checked | as above; blog "16 Shades of Grey" documents the bring-up |
| TriX (g3gg0) | DCT4 Phoenix container, DCT4 crypt | not checked | already used for the container; nothing on the ASIC |
| Nokia service manuals NEM-4 ch.7 / RH-29/RH-47 baseband | UPP_WD2 (MCU+DSP+logic), UEM, ADSP, CBUS, SDRAM; block diagrams | proprietary docs | which chips exist and how they connect; no registers |
| DCT3 "3310" emulator | DCT3 MAD2 full emulation | GPL-2 [per wiki] | DCT3 only; marginal (MBUS/FBUS ideas) |
| FirmWire | baseband firmware rehosting (Shannon, MediaTek) | open source | method only (peripheral modelling for rehosting) |
| S60 v1.2 / 2.x SDK (WINS emulator) | EKA1 user-side headers, WINS ekern | Nokia SDK licence | user-side API layouts; building an EKA1 sydb agent (with gnupoc GCC 2.9) [I] |

No DCT4/UPP emulator, no N-Gage hardware emulator, no EKA1 source drop and no EKA1 DDK were found [V:
searches; absence not proof].

## 4. Reuse from qembian

- **As-is:** QEMU 11.1.3 build and the device-model scaffolding (`hw/arm/syborg_*` patterns: framebuffer
  back end, input, serial, timer/intc code structure), QMP/HMP, gdbstub, snapshots/fork, screenshot and
  key injection in sydb's daemon layer; `tools/isi.py` decoding and the fake modem's reply engine idea;
  `devices/` profiles + `tools/device.py` (new family, e.g. `dct4-s60v1`; nothing from `bb5-s60` moves);
  Ghidra workflow, `tools/xipdis.py`-style ROM disassembly (needs an EKA1 header reader).
- **Reimplemented differently:** ROM composition (an EKA1 TRomHeader/TRomImageHeader writer that swaps
  or drops modules; no kernel side of ours to merge; start from `eka1/tools/eka1_extract.py`); the fake
  modem front end (old DCT4 ISA/ISI versions, and it must sit behind PhoNet in the guest as a fake-NOS
  blob plus an MMIO "ISI pipe" to the host, not behind an ISC driver); `isi-replies` per device; sydb's
  guest agent (an EKA1 build, no platsec); boot milestones (from LCD writes, serial or gdb, since the
  EKA1 kernel has no `SYBCODE:`/KIPC traces).
- **Not applicable:** personalities, our EKA2 kernel, EKA2 stand-ins (`src/standins/`), exec-table tools
  for EKA2.
- **Entirely new:** UPP-lite QEMU machine (CPU `ti925t` or `arm926`, flash at physical 0, SDRAM, the
  ~10 MCU blocks, minimal UEM over CBUS); fake NOS (replaces `nosstarter.ext` target / CMT region);
  EKA1 kernel symbol map for gdb (from `kernel-identifications.tsv`, verified entries only); MMC host
  model for E: (N-Gage games live on MMC, some check the card ID) [V for the DRM note, Batocera wiki].

## 5. Next steps

| # | step | effort | exit criterion |
|---|---|---|---|
| 1 | Static boot map: from `0x5000032C` (bootstrap) to ekern entry `0x5031B054`; who sets up MMU, page tables and IO mapping; does NOS start before Symbian; what `nosstarter.ext` jumps to | 3-5 days | written boot sequence with every MMIO access |
| 2 | PhoNet/NOS boundary: reverse `phonet.dll` (6 KB) + `nosdriver.ldd` lower interface; list what Symbian needs from NOS besides ISI (keys, power, battery, RTC) | 3-5 days | interface sheet; go/no-go on fake NOS |
| 3 | Register inventory: per IO page, which module reads/writes which offsets, with the code that polls (cheapest-first list) | 1 week | model spec per block |
| 4 | EKA1 ROM writer: rebuild `ROM.bin` with modules removed/replaced, checksum fixed | 3-5 days | round-trip byte-identical, then a module swap |
| 5 | UPP-lite QEMU machine (CPU, memories, intc, timer, LCD, CBUS/UEM minimum), boot under gdb | 3-6 weeks | ekern reaches EStart; LCD shows something |
| 6 | Fake NOS blob + host ISI pipe, reuse fake-modem engine with DCT4 message variants | 3-6 weeks | phone app/idle screen up without SIM errors |
| 7 | MMC host model, keypad path, EKA1 sydb agent | 2-4 weeks | N-Gage game installs and runs from E: |

Get a NEM-4 (original N-Gage) firmware before step 5 if the original is the target: the gate work on
the QD likely carries over (same ASIC family), the register details might not [I].
