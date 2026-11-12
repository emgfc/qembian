# Pixel 3a (google-sargo, SDM670): CPU and device facts for a hosted EKA2

Sources read on 2026-11-12. [verified] = stated by the cited source or read in our tree; [inferred] =
best guess from architecture knowledge, to be confirmed in P0 on the real phone.

## 1. CPU: AArch32 at EL0 and EL1

| Fact | State | Source |
|---|---|---|
| SoC: Snapdragon 670 (SDM670), Kryo 360 Gold/Silver, Adreno 615, 4 GB RAM, 1080x2220 | [verified] | Ubuntu Touch device page |
| Kryo 360 Gold/Silver are Cortex-A75/Cortex-A55 derivatives | [verified: "believed to be based on"] | CNX Software (Snapdragon 670/675 articles) |
| Cortex-A55 and Cortex-A75 are the last Arm cores with AArch32 at EL0..EL3; Cortex-A76 and later: AArch32 at EL0 only | [verified] | Arm Cortex-A comparison table (A76+ carry the "AArch32 at EL0 only" note), FreeBSD-arm list analysis (Jan 2024), Android Authority A76 deep dive |
| So `ID_AA64PFR0_EL1.EL1 == 0b0010` (AArch64 and AArch32) on both clusters of the SDM670 | [inferred: from the core type; Qualcomm may fuse it off] | P0 reads it |
| 32-bit user processes (EL0 AArch32) run on the Pixel 3a: Google's Pixel 7 was "the first Android phone to support only 64-bit apps" | [verified] | Android Developers blog, Oct 2022 |
| A 32-bit *kernel* at EL1 natively: the core can, but the exception level is handed over by the firmware/hypervisor (HCR_EL2.RW). ABL/Gunyah boot an AArch64 Linux; whether they will enter an AArch32 EL1 image is unknown | [inferred] | - |

How to read the EL fields from Linux: the arm64 MRS emulation hides the EL0..EL3 fields of
`ID_AA64PFR0_EL1` from user space (only DIT, SVE, AdvSIMD, FP are visible; invisible fields read as
"feature missing") [verified: kernel doc cpu-feature-registers]. So P0 needs either a kernel module
(`read_sysreg(id_aa64pfr0_el1)`), the kernel's boot log (`CPU features`, "CPU: All CPU(s) started at
EL1"), or simply the fact that 32-bit binaries run (EL0). The EL1 bit only matters for options (b)/(c).

## 2. Hypervisor, EL2, KVM

| Fact | State | Source |
|---|---|---|
| Qualcomm phones boot Linux at EL1 under Qualcomm's hypervisor (QHEE on this generation, Gunyah on newer ones); KVM "requires the kernel to boot in exception level 2 (EL2)" | [verified] | Qualcomm Linux docs ("enable virtualization"), Gunyah blog posts |
| Running KVM at EL1 on top of Gunyah was proposed (LKML RFC 2025) and rejected: "if you want to run KVM on your Qualcomm HW, boot at EL2" | [verified] | LKML, Marc Zyngier |
| Exiting the Qualcomm hypervisor to reach EL2 exists only as `qhypstub` for MSM8916/MSM8939 and as 2026 U-Boot patches for newer boards; nothing for SDM670 | [verified: the projects; inferred: SDM670 absent] | github msm8916-mainline/qhypstub, U-Boot list April-May 2026 |
| Stock/rooted Pixel 3a Android kernels (msm 4.9) have no `/dev/kvm` | [inferred: follows from EL1 boot] | - |

Conclusion: no KVM on the Pixel 3a. Option (b) is only possible on QEMU `virt` (where it is just (d)
with another board) or on a different device whose firmware boots Linux at EL2.

## 3. Bootloader

| Fact | State | Source |
|---|---|---|
| Bootloader unlockable (`fastboot flashing unlock`), except the Verizon model | [verified] | Ubuntu Touch page ("that model does not allow the bootloader to be unlocked"), Google factory-image instructions |
| A/B slots; community ports note "switch to the other slot if the modem does not work" | [verified] | Mobian/pmOS port reports (lemmy posts) |
| `fastboot boot boot.img` of a test image without flashing: standard on Pixel bootloaders, used by the pmOS/Mobian ports | [inferred: not stated by a page I could read; pmOS wiki is bot-blocked] | - |
| Stock Android above 9.0 must be downgraded to PQ3B.190801.002 before Halium-based Ubuntu Touch | [verified] | Ubuntu Touch page |
| The bootloader appends `console=null` to the cmdline (hides boot logs on screen) | [verified: search excerpt of the pmOS wiki] | pmOS wiki |

## 4. Linux support state

| Stack | State | Source |
|---|---|---|
| Ubuntu Touch (Halium, downstream kernel 4.9.124): display, touch, GPU (UI, hardware video), audio, calls, data, SMS, Wi-Fi, BT, camera, USB MTP/ADB all "Working"; VoLTE not | [verified] | devices.ubuntu-touch.io/device/sargo |
| Mainline (sdm670-mainline tree, Richard Acayan, close to upstream, 6.14.x in 2026): "Bluetooth, call audio, display, USB, I2C, eMMC, modem, video acceleration, WiFi, haptics, NFC, sensors, touchscreen, thermals" work ("slightly broken like before"); compute DSP added; ~90 downstream patches carried; GPU not mentioned in the notes | [verified] | gitlab sdm670-mainline tag notes v6.14.5 |
| Pixel 3a / 3a XL device trees upstream (sdm670-google-sargo.dts); debug UART patches March 2026 | [verified] | kernel trees, patchew |
| GPU on mainline: Adreno 615 is an a6xx; freedreno/msm support the family | [inferred] | - |
| postmarketOS: images in edge and stable v25.12; proximity, light, haptics marked working | [verified: search excerpt] | pmOS wiki (page itself bot-blocked) |
| Maemo Leste has a device page | [verified: exists; content not reachable, 502] | leste.maemo.org |

For (a) we need: display (DRM/KMS), touch (evdev), audio (ALSA), optionally GPU (Mesa freedreno, for the
host-GPU path of the N8/808); the modem is irrelevant (our fake ISI modem is kernel-side code). All of
these are reported working on mainline or Halium.

## 5. Host kernel configuration for AArch32 user code

| Option | Why | State |
|---|---|---|
| `CONFIG_COMPAT` (menuconfig, arm64) | 32-bit processes at all | [verified: arch/arm64/Kconfig in /usr/src/linux] |
| `CONFIG_KUSER_HELPERS` | vectors page at `0xFFFF0000`; not needed by Symbian code, harmless; with `ARM64_64K_PAGES` it moves `TASK_SIZE_32` to the full 4 GB | [verified: processor.h] |
| `CONFIG_COMPAT_VDSO` | optional; needs a 32-bit cross compiler at kernel build time | [verified: Kconfig] |
| `CONFIG_ARMV8_DEPRECATED` + `SWP_EMULATION`, `CP15_BARRIER_EMULATION`, `SETEND_EMULATION` | EUSER (EPL ARM build, and so the phones' ARMv5-built EUSERs) uses `swp` in request completion and lock code [verified: `euser/epoc/arm/uc_exec.cia:271`, `uc_utl.cia` 5 sites]; ARMv6K EUSERs (N8+) probably use `ldrex/strex` + CP15 `dmb` [inferred]. Sysctls `abi.swp` (default 0 = undef, 1 = emulate), `abi.cp15_barrier` (default 1 emulate, 2 = hardware if supported) | [verified: kernel doc legacy_instructions] |
| `CONFIG_COMPAT_ALIGNMENT_FIXUPS` | unaligned LDM/STM etc. from old ARM code | [verified: Kconfig exists; need inferred] |
| Page size 4 KB | Symbian chunks are 4 KB granular; 16 KB/64 KB host pages would waste and break `MAP_FIXED` at 4 KB offsets | [inferred] |
| Compat syscalls take the number from r7 (EABI); the `svc` immediate is ignored; seccomp sees `arch = AUDIT_ARCH_ARM` for compat tasks and `instruction_pointer` | [verified: `arch/arm64/include/asm/syscall.h` AUDIT_ARCH_ARM; `man 2 seccomp` instruction_pointer; r7 inferred] |
| 32-bit address limit `TASK_SIZE_32 = 0x100000000 - PAGE_SIZE` (4 KB pages) | ROM `0x80000000`, DLL data `0x90000000`, kernel data `0xC8000000` all mappable; `vm.mmap_min_addr` (64 KB) only excludes the bottom, below `KUserLocalDataBase 0x00400000` | [verified: torvalds/linux processor.h; mmboot.h constants] |

Android's own kernel on the Pixel 3a supports 32-bit processes (it ships 32-bit app support), so P0's
EL0 check can run on stock Android through `adb shell` with a static armhf binary; the sysctls and
seccomp behaviour should be checked on the Linux kernel we will actually use (pmOS/mainline).

## Sources

- Arm Cortex-A comparison table: https://documentation-service.arm.com/static/65f1a01687f147198672977d
- FreeBSD-arm, "last AArch32 EL0..EL3 capable Cortex-A": https://lists.freebsd.org/archives/freebsd-arm/2024-January/003598.html
- Android Authority, Cortex-A76 deep dive: https://www.androidauthority.com/cortex-a76-deep-dive-870896/
- Android Developers blog, 64-bit only devices: https://android-developers.googleblog.com/2022/10/64-bit-only-devices.html
- Linux doc, asymmetric 32-bit SoCs: https://docs.kernel.org/arch/arm64/asymmetric-32bit.html
- Linux doc, legacy instructions: https://docs.kernel.org/arch/arm64/legacy_instructions.html
- Linux doc, CPU feature registers: https://docs.kernel.org/arch/arm64/cpu-feature-registers.html
- `arch/arm64/include/asm/processor.h`: https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/include/asm/processor.h
- Qualcomm, enable virtualization (KVM needs EL2): https://dragonwingdocs.qualcomm.com/System/Kernel/enable-virtualization.md
- LKML, KVM at EL1 on Gunyah RFC and reply: https://lkml.indiana.edu/hypermail/linux/kernel/2504.3/01461.html
- qhypstub: https://github.com/msm8916-mainline/qhypstub ; U-Boot EL2 switch patches: https://lists.denx.de/pipermail/u-boot/2026-May/618654.html
- Ubuntu Touch sargo: https://devices.ubuntu-touch.io/device/sargo/
- sdm670-mainline v6.14.5 notes: https://gitlab.com/sdm670-mainline/linux/-/tags/sdm670-v6.14.5
- pmOS wiki (bot-blocked for tools): https://wiki.postmarketos.org/wiki/Google_Pixel_3a_(google-sargo)
- CNX Software on Kryo 360: https://www.cnx-software.com/2018/10/23/qualcomm-snapdragon-675-cortex-a76-processor/
