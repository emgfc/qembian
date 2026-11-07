# N95 8GB (RM-320) firmware inventory

This is a read-only analysis of the stock firmware set `RM-320_35.0.001_PR.C0R` (sha256 in
[evidence/n95/firmware.sha256](../../evidence/n95/firmware.sha256)). No firmware bytes are in the
repository. To reproduce it:

```sh
FW_DIR=/path/to/RM-320 tools/fw/inventory.py --device nokia-n95-8gb-rm320   # outputs in build/devices/<id>/
```

Tools written for this:

| Tool | Purpose |
|---|---|
| `tools/fiasco.py` | BB5 container → partitions |
| `tools/rom_unpage.py` | demand-paged core ROM → flat XIP image (byte-pair decoder ported from `byte_pair.cpp`) |
| `tools/romimg.py` | XIP ROM directory and `TRomImageHeader` reader |
| `tools/swi_map.py` | recovers EUSER's exec (SWI) numbering, ARM and Thumb aware |
| `tools/xip_imports.py` | ROM-wide import-thunk census |

Legend: **[verified]** means measured on the firmware; **[inferred]** means reasoned, not measured.

## 1. Container → partitions [verified]

The `.C0R` is a BB5 "B2" FIASCO container. After the header comes a chain of blocks
`<0x54|0x5D> 01 <tag> <len> <desc> <ck8> <data>`:
- tag `0x27`/`0x28` blocks are partition records: SHA-1, name, size at desc+37, location at desc+41.
- tag `0x17` blocks are data chunks: size at desc+6, flash location at desc+10.

The APE (Symbian) side is these partitions ([listing](../../evidence/n95/c0r-partitions.txt)):

| Partition | Size | Content |
|---|---|---|
| X-LOADER, NOLO, PRIMAPP, KEYS, PAPUBKEYS, PASUBTOC | small | Boot chain and keys. **NOLO** is the same Nokia loader family used on N800/N810. |
| SOS+XZIP | 36 KB | Core-image loader |
| SOS+UPDAPP / UPDAPP | 1.5 MB / 210 KB | Update/flash apps |
| **SOS+CORE** | 41 MB stored | **Core XIP ROM**, demand paged |
| **SOS+ROFS** | 21 MB | ROFS1: 5,847 entries, but only about 94 executables |
| DSP0, MCUSW | | DSP and modem (CMT) firmware |

The container header names the platform `OMAP2420 UNISTORE-II-1.5.1 ALG` with `XSR 1.5`.

## 2. Core XIP ROM [verified]

[Header and driver list](../../evidence/n95/core-rom-header.txt).

- **TRomHeader:** `romBase 0x80000000`, `romSize 0x3a00000`, `kernData 0xC8000000`, multiple memory
  model layout. It is the same base address as our Syborg ROM.
- **Paging:** the ROM is **demand paged** (`iPageableRomStart 0x84c000`, 14,795 pages).
  - The unpaged part (0–0x84c000) is stored raw. Its page-index entries carry compression type 1 anyway.
  - The pageable part is byte-pair compressed per page.
  - Uncompressed, the image is 58 MB.
- **Files:** 2,719 files, of which 2,712 are XIP executable images.
- **Validation:** 2,710 of 2,712 are byte-identical to the separate dump in
  a file dump of the same core ROM taken from a modified phone. The two exceptions, `efile.exe` (7 bytes) and `installserver.exe`
  (16 bytes), are the classic RomPatcher targets. **That dump came from a patched device.**
  Use this extraction as the stock reference.
- **Where the code is:** practically all of S60 is XIP in the core image: EUSER, EFSRV, WSERV, CONE,
  Avkon, EikCore, AppArc, ECOM and the rest. ROFS adds about 94 binaries. **XIP handling is not an
  edge case; it is the whole problem.**
- **Kernel side:**
  - Primary file is `ekern.exe`; the variant is `ecust.dll` on top of `kaomap.dll`.
  - 49 extensions load in boot order. Among them: `kaomap dma GAPE power i2c_master i2c2_master
    menelaus sti … ekeyb elocd medint EPBUSM* MEDSD mednand usbcc exstart watchdog d_display gdd dvfs_*
    Light* switch keypad tv_out evfp VariationMgrKernel exmon* kosal bridgecfg pvrsrv pvromaplcd spi2
    camldd camparifpdd gespi nips_kernel_eka2`.
  - 38 LDD/PDDs, including the modem ISC/ISI stack (`NokiaISCDriver`, `IscIsiMultiplexer`,
    `xbus`), WLAN, camera and the PowerVR driver.

## 3. N95 EUSER: exec (SWI) table recovered [verified]

`EUser.dll` sits at `0x8039d5f0`: code `0x8039d668`, 2,229 exports.
- **Code mode:** 1,903 exports are Thumb code (RVCT default). The exec stubs are ARM, in the form `svc #n; bx lr`.
- **Size of the exec table:** **265 SWIs: 28 fast (`0x800000–0x80001b`) + 237 slow (`0x00–0xed`).**
  Ours has 24 fast + 262 slow.
- **How the numbers were named:** we follow each exported function, through Thumb `BLX` calls, to
  the stubs it reaches. 774 N95 exports reach a stub, and 736 of ours do. Matching export names
  pairs an N95 number with one of ours:
  - **218 of the 265 are identified** this way. **Only 53 keep the same number.**
  - 32 more are named by EKA2L1's `v93` table, so about 250 of 265 have a name. Of the
    calls both tables cover, 119 agree outright. 11 more differ only in naming style
    (`ProcessId` vs `process_get_id`). Only 2 genuinely disagree: `S84` and `F05`.
    These are two independent derivations agreeing.
  - [Full table](../../evidence/n95/swi-n95-vs-ours.txt).
- **9.2-only semantics** visible in the table: kernel-held active scheduler and trap handler (fast
  `F06`/`F08`/`F09`), the DLL-lock and library-attach protocol (`S9b`–`Sa1`),
  `MutexWait` without a timeout, and others. A 9.2 personality must implement these, not just renumber.
- **Export ordinals:**
  - **The 7 EUSER exports that are ABSENT in our build (the old `RHeap` constructors and so on)
    are used by 0 of 2,712 N95 images.**
  - The N95 has 19 exports beyond the SDK (2211–2229): demand-paging and loader APIs that were
    backported. Wherever the kernel calls they make can be checked, they are the same functions at
    the same ordinals as in our `euseru.def` ([notes](../../evidence/n95/euser-ordinal-notes.txt)).

## 4. How XIP binaries bind to EUSER [verified]

[Census](../../evidence/n95/xip-imports.txt).

- **Import thunks:** each XIP image ends its code section with 8-byte import thunks
  `E51FF004 <absolute target>` (`ldr pc,[pc,#-4]`).
  - **323,609 thunks ROM-wide, and every target resolves to a ROM file.**
  - **EUSER: 114,223 thunks from 2,590 images, using 1,574 of 2,229 exports.**
  - EFSRV: 8,120 thunks from 964 images. HAL: 95 from 84.
  - The RVCT runtime: DRTAEABI from 2,605 images, SCPPNWDL from 2,417.
- **No branch collapsing:** only 5 candidate direct `BL`/`BLX`s into EUSER, which are noise level.
  Thunks are the real import table.
- **Plus 13,830 non-thunk words equal to EUSER export addresses, all inside text/rodata.** These are
  almost certainly import fixups in vtables and other constant tables [inferred]. Rebinding by
  patching must find them by value, because rombuild discarded the import section.
- **Export spacing:** 2,229 EUSER exports sit at 2,019 distinct addresses. 126 of them are less
  than 8 bytes from the next export (29 at 2 bytes, 68 at 4, 29 at 6). These are tiny Thumb leaf
  functions, so a "jump stub at every old export address" cannot be written uniformly.
- **`iDllRefTable` is empty for most images:** rombuild lists only dependencies that need
  initialisation, so dependency lists cannot be taken from it.

## 5. Local Symbian Foundation sources

- a local clone of `oss.FCL.sf.os.kernelhwsrv` has **exactly the same 269 commits** as
  `external/oss.FCL.sf.os.kernelhwsrv` (2009-10 to 2010-12, build 4016 on master, 3098 on
  GCC_SURGE). It holds no 9.2 kernel source, so it doesn't change either option. Same for the
  other `oss.FCL.sf.*` packages (graphics, classicui, appsupport and so on): Symbian^3-era middleware.
- **What they are good for:** checking class layouts and inline code in our EUSER and kernel
  headers against the 9.2 SDK headers, which is the main risk of option 2.
- **What they are not good for:** rebuilding N95 components. That isn't the goal.
