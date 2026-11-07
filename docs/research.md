# Research notes: sources, toolchain, dependencies

Status legend: **[verified]** = checked by running or reading the artifact in this project;
**[inferred]** = plausible from indirect evidence; **[unverified]** = not checked.

## 1. Source repositories

| Repo | What it is | Branches | Choice |
|---|---|---|---|
| `SymbianSource/oss.FCL.sf.os.kernelhwsrv` | EKA2 kernel, nanokernel, memory models, EUSER, F32 file server, HAL, drivers, kernel tests, template BSP | `master` (to 2010-11-02, `201043_12`), `BRANCH_GCC_SURGE` (to 2010-11-25), `BRANCH_GCCE_COMPATIBILITY`, `BRANCH_COMPILER_COMPATIBILITY{,_2}`, `BRANCH_RCL_3`, `BRANCH_ANYWHERE`, ... | **`BRANCH_GCC_SURGE` @ `e0036b02`** [verified builds] |
| `SymbianSource/oss.FCL.sf.adapt.Qemu` | Syborg BSP (`baseport/syborg`), QEMU 0.9.1-symbian (`symbian-qemu-0.9.1-12`), docs, elf4rom | `master`, `BRANCH_GCC_SURGE` (master tip = "merge with GCC_SURGE tip"; the trees differ in 27 files), `BRANCH_NVMEMORY` and `BRANCH_PHONESIM_INTEG` (both merged), `BRANCH_GRAPHICS_PHASE_3` and `BRANCH_RCL_1` (not merged) | **`BRANCH_GCC_SURGE` @ `59747f3`** [verified] |
| `SymbianSource/oss.FCL.interim.SFTools.LinuxBuild` | Linux port of the Symbian build tools: Raptor (sbs 2.15.2), elf2e32, rcomp, rombuild/rofsbuild/romnibus, sbsv1, plus `cross-plat-dev-utils` scripts | `master` only @ `5da968b` (2010-12-01) | **`master`** [verified] |
| `conexport/symbian_tools_qemu` | Fork of qemu-symbian-svp with fixes for modern compilers (`0.9`). Also `dec2011` = upstream QEMU 1.0.50 with the **native C Syborg models**, and `5.0.0-next` = plain QEMU 5.0 with **no Syborg** | | reference only |
| `martintrojer/qemu-syborg` | 2011 mirror of the same QEMU repo layout | `master` | not used |
| `atreia108/eka2_build_guide` | Ubuntu 10.04 + VirtualBox recipe | | followed in spirit |

**EKA2 version.** On `GCC_SURGE`, `e32ver.h` gives `KE32BuildVersionNumber=3098` (Symbian^3/^4
era; the tree merged "Catchup to latest Symbian^4"). The **oldest** public commit (2009-10-19)
is already build **2044**. The S60 3rd FP1 SDK's `e32ver.h` gives **1150** for Symbian 9.2.
**No public EKA2 source is anywhere near 9.2.** [verified]

**Is the Syborg BSP complete?** For a text shell, yes [verified: it builds and boots]. It contains
the bootstrap, ASSP/variant, interrupt, timer, serial, LCD/framebuffer, keyboard, pointer,
NVMemory media, hostfs, snapshot, sound, ethernet (virtio), webcamera, and estart.

## 2. QEMU options

| Option | Status |
|---|---|
| `qemu-symbian-svp` 0.9.1 from `oss.FCL.sf.adapt.Qemu` | **In use.** Builds unpatched with GCC 4.4.5 in squeeze [verified]. Core Syborg devices (intc, timer, rtc, serial, fb, keyboard, pointer, nvmemory) are **Python 2.6 plugins** driven by the `.dtb`. hostfs, nand, platform, snapshot and virtio are C. `screendump`/VNC are stubbed out in this fork. |
| conexport `0.9` | Same tree with fixes for modern host compilers and Python 2.7. Useful if we ever want to build on a modern host, but Python 2 is still required. [inferred] |
| Upstream QEMU ≤ 1.0 (`dec2011` branch) | Native C `hw/syborg*.c` by Paul Brook (CodeSourcery): interrupt, timer, rtc, serial, fb, keyboard, pointer, virtio-net. **No nvmemory, hostfs or snapshot.** Upstream removed Syborg after 1.x. [verified: files present] |
| Port Syborg to modern QEMU | Feasible: the dec2011 C models are small sysbus devices. NVMemory (needed for `C:`) would have to be rewritten in C. Est. 1–3 days. **Not needed now.** |

## 3. Toolchain

| Item | Source | Status |
|---|---|---|
| CodeSourcery `arm-2010q1-190` (the guide's version) | `sourcery.sw.siemens.com` | **Gone.** The host no longer resolves and there is no Wayback capture. [verified] |
| **CodeSourcery `arm-2009q3-63`** (GCC 4.4.1, binutils 2.19.51) | Wayback capture of `sourcery.mentor.com/public/gnu_toolchain/arm-none-symbianelf/` | **In use**, sha256-pinned. Its symbianelf libgcc **lacks the ARM EABI integer helpers**, which needed a fixup ([build-log §5](build-log.md#5-libgcc-lacks-the-arm-eabi-integer-helpers)). |
| CodeSourcery `arm-2012.03-42` (GCC 4.6.3) | archive.org `nokia_sdks_n_dev_tools/Gcce4.6.3.zip` (Linux + MinGW tarballs) | Downloaded and pinned as a fallback. Its libgcc *does* have the helpers. Raptor here has no `gcce4_6_3` variant. |
| CodeSourcery 2005q1C / 2008q3 | Wayback, and the `Nokia_Symbian3_SDK_v1.0/epoc32/tools/distrib` Windows installers | not used |
| User's `~/csl-gcc` | CSL **GCC 3.4.3** (S60 3rd FP1 GCCE), i386 | too old for GCC_SURGE |
| User's `~/gcc-builds/gcc-14.2.0` | `arm-none-symbianelf` GCC 14.2 + binutils 2.29.1 (GCC4Symbian-style) | Interesting for later modernisation. Expect many C++ diagnostics on 2010 kernel code. Not tried. |
| SymbianRevive fork of LinuxBuild | modern-compiler fixes; `imgtools` excluded from its build | not used (we need rombuild; the original LinuxBuild builds fine in squeeze) |

Binary-only components that end up in the ROM:
the **RVCT runtime DLLs** `drtaeabi.dll`, `dfpaeabi.dll`, `drtrvct3_1.dll`, `dfprvct3_1.dll`,
`dfprvct-thunk.dll` and `scppnwdl.dll`. These ship inside `kernelhwsrv` itself as
`kernel/eka/compsupp/rt_*_export.zip` (GCC_SURGE uses the RVCT 4.0 runtime for user side, bug 3073).
Everything else in the ROM is compiled from source here. [verified: ROM file list vs mtimes/.map files]

## 4. PDK 4.0a

`archive.org/download/nokia_sdks_n_dev_tools/PDK_4_0a.zip`: 1.9 GB, md5 matches archive.org
metadata, release `FCL.single.1054` (Dec 2010). Each inner archive is verified against
`release_metadata.xml` md5s. Inner archives are 7z files named `.zip`. Contents used:

- `binaries_epoc.7z.zip`: `epoc32/include` (headers for every package), data, sbs_config
- `tools_epoc.7z.zip`: makefile templates and Windows tools (Raptor needs some exported config)
- `binaries_rom_images.7z.zip`: **prebuilt reference ROMs**: `syborg_tshell_ARMV5_{udeb,urel}.img`,
  `syborg_001.minigui-stripped.IMG` (a graphical ROM, not tried yet), and also beagle and ne1_tb ROMs.
  These are RVCT-built (`armcchlp.cpp` paths in the image).

Not used: `binaries_armv5_{udeb,urel,lib}` (prebuilt RVCT target binaries). Using them would let
the ROM silently pick up prebuilt files. `src_oss_*` holds the source snapshot (2 GB).

## 5. Dependency graph (smallest working configuration)

```
host: podman ──► squeeze i386 container (gcc 4.4.5, python 2.6, perl 5.10, SDL 1.2, boost 1.42, p7zip)
                    │
 LinuxBuild ────────┼─► Raptor (sbs + bundled Python 2.7, make 3.81, bash 4.0, talon)
                    ├─► host tools: elf2e32, rcomp, rombuild, readimage, ...
 PDK binaries_epoc ─┼─► epoc32/include, sbs_config   (+ prep_env.pl fixes: gcc.h preinclude, 'surge' variant)
 CSL 2009q3 + GCC 4.4.1 helper sources ─► arm-none-symbianelf gcc 4.4.1 with completed libgcc
                    │
 kernelhwsrv (GCC_SURGE) ─► ksrt_gcce.lib ─► ekern/euser/f32/drivers/e32utils (arm.v5.udeb.gcce4_4_1.surge)
 adapt.Qemu baseport ─────► _syborg_* variant, drivers, bootstrap ─► rombuild ─► syborg_tshell_ARMV5_udeb.img
 adapt.Qemu QEMU 0.9.1 ───► qemu-system-arm + Python plugins + libnvmemmory.so + syborg.dtb ─► boot
```

## 6. Other useful local assets (user's ~/work)

- `S60_3rd_FP1/` SDK (Symbian 9.2): headers, `e32ver.h`, ARMV5 `.dso` import libraries. Used for the ABI diff.
- `EKA2L1/` (GPL-3.0 HLE emulator, @e67f84dc6): per-OS-version SVC tables. Used read-only for the exec-ABI comparison.
- `elf2e32`, `elf2e32_next`, `claude-epoc32-zig` (E32Image parser), `symbian-clang`, `GCC4Symbian`, `gnupoc-package`: useful for building 9.2-ABI test binaries later.
- `symsim/`: the earlier Rust HLE attempt.
- `symbian-bin/RM-320`: appears to be the N95 8GB firmware. **Deliberately not inspected yet**, per instructions.

## 7. Open questions / uncertainties

- Exact behaviour of the guide's CSL 2010q1 libgcc is unknown (no copy found). Our helper set
  reproduces what GCC_SURGE needs, as shown by a clean link of every package component.
- `urel` builds, the SMP config, and `e32test`/`f32test` runs on Syborg have not been tried.
- The graphical PDK ROM (`syborg_001.minigui-stripped.IMG`) has not been booted.
- Whether upstream QEMU still has the OMAP2420 `n800/n810` machines (relevant to approach A):
  the 0.9.1 tree here has them. Current upstream status was not checked.
