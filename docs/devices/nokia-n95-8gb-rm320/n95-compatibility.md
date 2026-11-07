# N95 (Symbian 9.2 / S60 3rd FP1) on the open-source EKA2 kernel: first assessment

Scope: what can be checked **without** the N95 firmware. Evidence files are in
evidence/abi/. The tools are `tools/dso_ordinals.py` and `tools/exec_table.py`.
Legend: **[verified]**, **[inferred]**, **[unverified]**.

## Verdict

*Updated 2026-11-04 after the firmware inventory ([n95-firmware-inventory.md](n95-firmware-inventory.md)).
Sections 1–5 are the original pre-firmware assessment. Sections 6–7 replace the earlier option
table.*

**Approach B in its plain form does not work.** Running the original N95 user side unmodified on
this kernel fails because of the exec ABI: of the N95's 265 SWIs, only 53 keep their number in our
kernel. Two ways of bridging that gap are now measured and both are feasible:

1. A 9.2 exec personality in our kernel, with the N95 user side byte-identical, including its EUSER.
2. Our EUSER swapped in and the N95 XIP binaries rebound to it.

**Recommendation: option 1**, with option 2 kept as a fallback. **Decided 2026-11-04: option 1, then
option A** ([roadmap.md](../../roadmap.md)). The reasoning is in §7. Approach A
(OMAP2420 emulation) is confirmed plausible but has the largest open-ended hardware surface.

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

> Superseded by measurement: imports are 8-byte thunks (323,609 ROM-wide), there is no branch
> collapsing, and the core ROM is demand paged. See [n95-firmware-inventory.md §4](n95-firmware-inventory.md).

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

## 6. Options after the firmware inventory

Measured facts are in [n95-firmware-inventory.md](n95-firmware-inventory.md). The common ground for
options 1 and 2:
- The N95 core ROM uses **romBase 0x80000000 and the multiple memory model, the same as our Syborg ROM**.
  It flattens to 58 MB (the paging can simply be switched off), and the Syborg board's 512 MB of RAM holds it.
- In both options our kernel-side files (EKERN, variant, Syborg drivers) replace the N95's 49
  extensions and 38 LDD/PDDs. They have to be placed where they don't overlap N95 user-side files,
  for example after `0x839cb000`, with a new `TRomHeader` and root directory at `0x80000000`.
  User code does not reference the N95's kernel-side files.

| | **1. 9.2 exec personality** | **2. Rebind to our EUSER** | **A. OMAP2420 emulation** |
|---|---|---|---|
| N95 user side | **byte-identical, EUSER included**. No rebinding: 323,609 thunks stay valid. | EUSER (and probably EFSRV/efile) replaced. About 114k EUSER thunks + about 13.8k value-matched words patched, or trampolines, which can't cover 126 exports spaced under 8 bytes apart. | byte-identical |
| Kernel | ours + a 9.2 SWI table: 265 entries. About 250 are named (218 via export matching, 32 more via EKA2L1, two independent sources that agree). | ours, unchanged | Nokia's, unchanged |
| Where incompatibilities show up | **at one choke point (SWI dispatch)**, traceable per call on serial | spread over 2,590 images: 9.2-compiled inline code and class layouts against a Symbian^3 EUSER. Bugs are silent memory corruption. | hardware register accesses |
| Hard parts | 9.2 semantics: kernel-held active scheduler and trap handler (fast `F06`/`F08`/`F09`), the DLL-lock and attach protocol (`S9b`–`Sa1`), `MutexWait` without a timeout, 9.2 loader structures (N95 `efile.exe` stays), 9.2 user-side views of LDD channels (`TBusLocalDrive`, comms) | F32 plugin ABI: N95 `.fsy`/`.fxt` link to N95 `efile.exe` internals, so file systems would come from our build. Header and layout drift. The 13.8k non-thunk refs are found by value (false-positive risk). | modem link (ISC/ISI, `nips_kernel_eka2`), GAPE, STI, display and keypad wiring, DSP bridge, PowerVR, boot chain (NOLO → XZIP) |
| Head start | our kernel boots and is debuggable. EKA2L1's `v93` code is a reference implementation of 9.2 exec semantics. | all ABSENT EUSER ordinals are unused (0 of 2,712 images). The N95's extra ordinals 2211–2229 match ours. | QEMU 0.9.1 already has OMAP2420, Menelaus (`twl92230`), OneNAND, CBUS, I2C, SPI and the N800 machine. The N95 uses the same NOLO loader family. |

Common to all three: the Nokia hardware-facing user-side servers (DOS/SysAp power, modem ISA
stack, camera, accessories) need stub drivers or fake peers. Starter/SysAp will block the boot
otherwise. The display needs to be 240×320 through the Syborg framebuffer, which is configurable in the DTS.

## 7. Recommendation and next experiments

**Option 1** is preferred:
- It keeps the user side exactly as shipped, which is the project's goal, with no rebinding and no
  mixed 9.2/^3 user code.
- It concentrates every incompatibility in one mechanically checkable layer (the exec dispatch).
- The table it needs has now been recovered with two independent derivations agreeing.

Option 2's risk is spread across thousands of binaries and fails silently. Its one advantage, no
kernel changes, is lost because F32 and the drivers' user-side interfaces still differ.

1. **E1, the option 1 spike:**
   - A ROM composer: our kernel-side image + the flat N95 core (unpaged), N95 `EStart.exe` and
     `efile.exe` kept.
   - A per-process or global "9.2 personality" SWI table in `kernel/eka/kernel/arm/`. The 250
     named calls map to our handlers or to shims; unknown calls trace and panic.
   - **Goal:** N95 `efile.exe` runs and mounts Z:, then `EStart` progresses. Every SWI is logged
     to serial, so each stall names its cause.
2. **E2:** resolve the remaining 15 unnamed SWIs and the 2 EKA2L1 disagreements (`S84`, `F05`) by
   disassembling the N95 EUSER callers. Extend the comparison to argument structures for the
   loader and chunk calls.
3. **E3 (cheap, parallel):** a time-boxed boot of the flat N95 core image on the QEMU `n800`
   machine, to see how far Nokia's own EKERN gets on modelled OMAP2420 hardware. This gives
   option A a real data point.
4. **Deferred:** build the option 2 rebinder only if E1 hits a wall in the kernel personality.
