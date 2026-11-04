# N95 (Symbian 9.2 / S60 3rd FP1) on the open-source EKA2 kernel: first assessment

Scope: what can be checked **without** the N95 firmware. Evidence files are in
[evidence/abi/](evidence/abi/). The tools are `tools/dso_ordinals.py` and `tools/exec_table.py`.
Legend: **[verified]**, **[inferred]**, **[unverified]**.

## Verdict

**Approach B in its plain form does not work.** "Original N95 userspace unmodified on this kernel"
is not viable, because the **executive-call (SWI) ABI differs substantially** between 9.2 and
this kernel. User-side *library* ABIs (export ordinals) are, however, almost fully preserved. A
**hybrid (D)** is plausible: give this kernel a 9.2-compatible exec personality and compose ROMs
around the N95's own XIP user side. It needs N95-specific data (the 9.2 exec table and the ROM
layout), which needs the firmware. Approach **A** (OMAP2420 machine emulation) deserves a
serious estimate before committing, because the QEMU tree here already models the N95's SoC family.

## 1. Version gap

| | E32 build | Source |
|---|---|---|
| N95 (S60 3rd FP1) | 2.0 build **1150** | SDK `Epoc32/include/e32ver.h` [verified] |
| Oldest public kernelhwsrv commit (2009-10) | build **2044** | git [verified] |
| Our kernel (GCC_SURGE) | build **3098** | git [verified] |

There is **no public kernel source near 9.2**. Building an "older EKA2" from an earlier
public snapshot buys little: the oldest public one is already Symbian^2-era.

## 2. User-side library ABI (export ordinals): mostly preserved [verified]

The S60 3rd FP1 SDK import libraries were compared with our kernel's frozen EABI `.def` files:

| Library | 9.2 exports | same symbol @ same ordinal | changed | Note |
|---|---|---|---|---|
| EUSER | 2210 | **2203** | 7 → `ABSENT` | old `RHeap` ctors ×4, `RHeap::operator new`, `RHeap::GetAddress`, one `RChunk::Create` overload (Symbian^3 heap rewrite) |
| EFSRV | 335 | **333** | 2 | `RFs::ReadFileSection` → `..._RESERVED` alias; 1 ordinal was already absent in 9.2 |
| HAL | 5 | **5** | 0 | |

So a 9.2-built **relocatable** binary (an E32Image loaded by the loader, for example from ROFS or C:)
that imports from EUSER/EFSRV/HAL would bind correctly to *our* EUSER/EFSRV/HAL, unless it uses
one of the 7 absent EUSER exports. Other system DLLs (ECOM, CenRep, WSERV, CONE, ...) are not part
of kernelhwsrv. For those we would ship the N95's own copies.

## 3. Executive-call ABI: incompatible [verified, using third-party data]

User-side EUSER enters the kernel with numbered SWIs (fast calls `0x00800000|n`, slow calls `n`)
generated from `kernel/eka/kernel/execs.txt` by genexec. Numbering our `execs.txt` the same way
and comparing it with EKA2L1's per-release SVC tables gives this. EKA2L1 derives its tables from
real device binaries. It labels S60 3rd FP1 as `epoc93fp1` and uses its `v93` table for it.

| EKA2L1 table | same call at same number as our kernel |
|---|---|
| v10 (Symbian^3) | 78% (most of the rest are naming differences) |
| v94 (S60 5th) | 45% |
| **v93 (S60 3rd FP1, i.e. N95)** | **26%** |

The tables are not append-only across releases. Calls were inserted mid-table: for example,
slow `0x4d` is `dll_tls` in v93 but `SessionSendSync` in our kernel. **An N95 `euser.dll` would
invoke the wrong kernel functions.**

Semantics also drifted *within* the public history (2009 → 2010). [verified in git]
- `MutexWait` gained `arg2 = timeout` (`sexec.cpp`: `0` = forever, `-1` = poll, `>0` = µs).
  An old EUSER doesn't set r1, so a stale positive value would make `RMutex::Wait()`
  **time out silently without acquiring the mutex**, and old EUSER ignores the return value.
- `ThreadGetCpuTime` changed from handle-based to an `arg1` form.
- New calls appended (`MathSecureRandom`, `TimerAgainHighRes`, ...). These are harmless for old clients.

## 4. Binary/ROM format

- **E32Image (non-XIP)**: the loader (`sf_lepoc.cpp`, `f32image.h`) accepts header format V
  with deflate or byte-pair compression, which is the 9.x format. [verified in source; no 9.2 binary loaded yet]
- **XIP ROM binaries** [inferred, standard EKA2 design]: files in a core ROM image are
  pre-relocated to fixed addresses, and rombuild resolves their imports at build time. They cannot
  be repacked into a ROM at other addresses, and they cannot be pointed at a *different* EUSER,
  without re-binding. The core XIP image of S60 3.x devices typically holds EUSER, EKERN, F32
  and many system DLLs. This is the main structural obstacle to "our EUSER + Nokia user side".
  *Needs the firmware to quantify: which components are XIP and which are ROFS.*
- ROM header/root-directory structures (`e32rom.h`) are documented in our source. A custom
  composition tool could place our kernel-side XIP files *beside* N95's user-side XIP files at
  their original addresses. EKA2 also supports extension ROMs. [inferred]

## 5. Other areas

| Area | Assessment |
|---|---|
| Kernel-side drivers (LDD/PDD/extensions) | N95 ones are built against 9.2 EKERN's internal ABI and target OMAP2420/Nokia ASICs. **Not reusable.** Syborg equivalents exist for display, keyboard, pointer, serial, media and sound. [inferred] |
| Nokia-proprietary user-side servers that talk to hardware drivers (modem ISA/ISC, DOS/SysAp power and accessory, camera, ...) | Will fail to open their channels on Syborg. They need stubs or fake LDDs. Starter/SysAp will likely block the boot without them. [inferred; to be inventoried] |
| Memory model | Both use the multiple memory model (Syborg header `memmodel multiple`; N95 is ARM11-based) [N95 side inferred]. The address layout must match the N95 ROM's `romlinearbase`. |
| CPU / FP | The Syborg CPU model is a Cortex-A8 and runs ARMv5/v6 user code. If N95 binaries use **VFP**, the kernel must enable VFP context switching. The Syborg kernel is built for plain ARMv5. [unverified] |
| IPC / descriptors / chunks / handles | IPCv2 and descriptor layouts are stable since 9.0 per the user-side headers. `RChunk::Create` overload removed (above). [inferred] |
| HAL | User-side HAL ABI identical (5/5). HAL attribute enum is append-only by Symbian policy. [verified/inferred] |
| Platform security | Same capability/SID/VID model. ROM files are trusted; the loader enforces capabilities. Signatures are only checked for installs. [inferred] |
| Graphics | 9.2 WSERV is pre-NGA and uses a screen driver plus LCD HAL. Syborg's `LCD.DLL` provides a framebuffer through HAL, which is likely workable. Nokia's display drivers are not. [inferred] |

## 6. Approaches compared

| | A. Full N95 hardware emulation | B. Our kernel + unmodified N95 user side | C. HLE (Rust, EKA2L1-style) | D. Hybrid: our kernel with a 9.2 exec personality + N95 user side |
|---|---|---|---|---|
| Original kernel/ROM | yes, unmodified | no | no | no |
| Blocking issue | Nokia-specific HW (power ASICs, modem link, display/keypad wiring, flash, boot chain) | **exec ABI mismatch (26%)** + XIP binding | must reimplement kernel + many servers | 9.2 exec table must be reconstructed; ROM composition; Nokia HW servers need stubs |
| Head start here | QEMU 0.9.1 tree already has **OMAP2420** (`omap2.c`, `nseries.c` N800/N810, OneNAND, CBUS Retu/Tahvo) [verified files exist; N95 = OMAP2420 inferred] | — | earlier Rust work, EKA2L1 as reference | working EKA2 + Syborg + debug tooling (this repo) |
| Risk | High unknown HW surface, but every fix is "make HW behave". No ABI guesswork. | Not viable as is | Large, open-ended. EKA2L1 reimplements WSERV and other servers rather than running the originals. | Medium: well-defined mechanical work plus an unknown number of semantic mismatches |

## 7. Recommended next experiments

1. **No firmware needed (cheap)**: build a tiny console EXE against the **S60 3rd FP1 SDK**
   import libraries (9.2 ordinals; the user's `~/csl-gcc` 3.4.3 or our 4.4.1 + elf2e32). Run
   it from ROM or `C:` on our Syborg ROM. This proves that 9.2-linked relocatable binaries bind to our EUSER.
2. **Needs the firmware (decision point)**: *inventory only*. Split the core image vs ROFS; list
   the XIP files and their link addresses; extract the N95 `euser.dll` exec stubs to get the
   **exact 9.2 SWI table**; count imports of the 7 ABSENT EUSER ordinals; list the Nokia LDD/PDDs
   and the servers that open them. The output is a quantified choice between A and D.
3. Depending on (2): either a prototype exec-number translation in `kernel/eka/kernel/arm/`
   (selected per process, keyed by image metadata), or a time-boxed spike booting the N95 core
   image on the `n800` OMAP2420 machine to see how far the original kernel gets before it
   touches unmodelled hardware.
