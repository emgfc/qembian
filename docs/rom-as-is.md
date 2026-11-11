# Toward running the firmware as is (2026-11-10)

Goal (owner): no changes to the phone's files. Our kernel side replaces the phone's (that stays: the phone's
kernel needs its SoC), and everything else runs byte for byte, the missing hardware answered by QEMU
devices, as the modem (syborg-modem) and EGL/OpenVG (syborg-gl + src/syvg) now are. [V] = read in the
profiles and code today, [I] = inferred.

## What the build changes today

| change | where | devices | what would replace it |
|---|---|---|---|
| kernel side swapped (our EKA2, baseport, stand-ins in the phone's kernel window; new root directory) | `tools/romcompose.py` | all | stays: it is "replace the kernel". It could be done by QEMU's loader at boot (the phone's image file mapped as is, our window and the directory laid over it in memory), but that only moves romcompose (372 lines) into C: no fidelity gain. Low priority [V] |
| H-khr: libEGL, libOpenVG, libOpenVGU, libGLES_CM, libGLESv1_CM, libGLESv2 replaced | `[[rom.rofs_replace]]` | N8 (all firmwares), 701, 808 | a VideoCore device: Broadcom's own libraries are KHRN clients that serialise every EGL/VG/GLES call to the VideoCore [V]. A stand-in for the VideoCore kernel driver passes those messages to QEMU, which decodes them into what src/syvg (EGL, OpenVG) and the GLES 2 replay already run. The host side exists now; the missing part is the KHRN message format. Broadcom's BSD-licensed Raspberry Pi userland (interface/khronos) is the same client design, a likely reference for it [I]. Biggest and most valuable item |
| gcebe: compositionbackend.dll's entry rebound to our backend (blob + import rebind) | `[composition]`, `gcebe-patches.txt` | N8, 701, 808 | the same VideoCore device: the phone's own composition backend talks to the VideoCore too [I] |
| H-startuptone-none: CenRep start-up tone points to a missing file | `[[rom.rofs_patches]]` | 5800, N8, 701, 808 | a RAPIDO DSP model: QEMU plays the PCM rings (guest shared chunk, aamdaifdriver) to syborg-audio and reports the stream's end, so the tone ends. The DSP's ISI replies are already in syborg-modem [V] |
| H-keysound-nommf, H-startuptone-nommf: 5 code patches (eiksrvui.dll, startup.exe) | `code-patches.txt` | N95 | the N95's DSP (TI DSP/BIOS bridge, ehwa stand-in answers init only [V]) as a QEMU audio DSP, so MMF tone players work. Medium-heavy [I] |
| H-fmradio-nosession / H-fmradio-none: code patches in fmradioaudiocontrol.dll | `code-patches.txt` | N8, 701, 808 | a Bluetooth HCI controller on a UART in QEMU (public HCI standard) plus the FM part of the BT/FM combo chip answering "no signal" (Broadcom vendor commands [I]). The radio HAL server then makes its session |
| H-bt-ondemand: `OnDemand=1` in esock_bt.cmi | `[[rom.file_edits]]` | N8, 701, 808 | the same HCI controller (the ESock Bluetooth worker panicked on a transport it never opened [V]) |
| ESTARTCOMP.TXT replaced (drive map) | `[rom] files` | N8, N79, 5730, N95 | our media drivers on the phone's own local drive numbers (C: on its NAND/eMMC number, Z: ROFS numbers as the phone's), so its own file works. Kernel-side configuration, no QEMU work [I] |
| H-delight-own-drivers: our lightpdd.pdd over Delight's | `[[rom.rofs_replace]]` | Delight only | a firmware mod adding a hardware PDD to ROFS (ROFS shadows the core image). Without a patch only a model of that light hardware would do; keep as a mod-specific hack |
| sygl (GLES 1.x) blob and trampolines | `[gl]`, code patches | N95 | the N95's GPU stack (PowerVR services) remoted: heavy; keep sygl [I] |

Not ROM changes: personality patches (`personalities/`) and kernel patches change our kernel; stand-ins
are our kernel-side drivers. Most stand-ins are thin (4-75 lines) and talk to QEMU devices already; the
ones with logic are d_display (the GPU channel), ehwa (N95 DSP bridge answers), exyin (touch) and medrofs.

## Order (proposal)

1. RAPIDO DSP model (small; removes H-startuptone-none on four devices, gives real start-up sounds).
2. HCI controller + FM "no signal" (removes three hacks on three devices).
3. ESTARTCOMP drive numbers (kernel-side only).
4. VideoCore/KHRN device (large; removes H-khr and gcebe, Broadcom's libraries run). Research first: the
   message format, the driver channel, how much of KHRN the firmware's libraries use.
5. Leave: the kernel window composition, Delight's lightpdd, N95 sygl.
