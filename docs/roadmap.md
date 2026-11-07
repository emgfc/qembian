# Roadmap

Decision (2026-11-04, by the project owner): **option 1 first, then option A.** Options are
defined in [n95-compatibility.md §6](devices/nokia-n95-8gb-rm320/n95-compatibility.md).

1. **Option 1: our EKA2 kernel with a Symbian 9.2 exec personality, running the N95 8GB user side
   byte-identical** on the Syborg virtual platform. This continues until the N95 user side is
   "working": the home screen, Avkon/CONE/Uikon/WSERV up, and apps launchable.
2. **Option A: emulate the N95's OMAP2420 platform** (QEMU `n800`-family machine) and run the
   unmodified Nokia kernel and ROM.

Why this order:
- Option 1 runs on a kernel we can build, trace and debug, so user-side problems can be told apart
  from hardware problems.
- What it produces carries over to option A:
  - ROM composition, unpaging and inspection tools
  - the exec trace
  - the inventory of Nokia hardware-facing servers and the fake peers they need (modem ISA,
    DOS/SysAp power, accessories)
  - a known-good reference boot to compare against

## Option 1 work plan

| Step | Content | Exit criterion |
|---|---|---|
| E1.1 | ROM composer: our kernel-side files + the N95 user-side files at their original XIP addresses + a new root directory/TRomHeader; paging off | composed image passes `romimg.py`; every N95 user file byte-identical at its original address |
| E1.2 | 9.2 exec table in our kernel: same dispatch mechanism, 9.2 numbering; unknown/unimplemented calls trace + fault; per-call serial trace switch | kernel builds; our own text shell still boots with a matching EUSER (regression check) |
| E1.3 | Boot the composed ROM: N95 `efile.exe` as file server | `efile` reaches its first IPC/loader calls; every stall named by the SWI trace |
| E1.4 | 9.2 semantics shims as hit (active scheduler / trap handler fast calls, DLL lock and attach, `MutexWait`, loader structures) | N95 `EStart.exe` runs |
| E1.5 | Hardware-facing stubs (DOS/SysAp, modem, accessories), display 240×320, keypad mapping | N95 `ewsrv.exe` draws, then Starter/SysAp, then the home screen |

The status of each step is tracked in [build-log.md](build-log.md) (option 1 section) and the
README.

**Status 2026-11-04 (end of day):**
- E1.1–E1.3 done.
- E1.4: shims added as hit (HandleDuplicate, MutexWait, SafeInc/Dec, loader struct size).
- E1.5 in progress. Hardware stand-ins exist for ISC (silent modem), Custom, the display driver,
  the ROFS media and a 240×320 32 bpp LCD. Nokia's WSERV, FBSERV, ECOM, Starter and DosServer run.
- Fake modem peer (ISI over ISC) done for the boot-critical messages; Starter runs its start list.
- The next blocker is `DATAPORT.CSY` leaving in the comms server; needs a ROFS E32-image loader for analysis.

## Option A (later)

Start from the QEMU 0.9.1 `n800` machine (OMAP2420, Menelaus, OneNAND, CBUS, I2C, SPI already
modelled). Boot the flat N95 core image directly, skipping NOLO and XZIP. Then add Nokia-specific
devices in the order Nokia's EKERN touches them: GAPE, STI, the display and keypad path, the ISC
modem link (fake CMT peer), DVFS, the PowerVR and DSP bridge stubs.

**Status 2026-11-05 (overnight, unattended):** runs 20–31 in [build-log.md](build-log.md); every
shortcut is in [hacks.md](hacks.md).
- Starter now runs its whole start list without crashing: `sae`, `SysAp`, `Phone`, `Idle`,
  `screensaver`, `startup`, `alarmserver`, `aknnfysrv`, `PhoneServer`, … (Avkon servers stable).
- Unblocked on the way: DRM key storage (dummy device cert), IMEI/IMSI/light/energy-management ISI
  replies, MmfAudioServer's permanent thread dying (no DSP; kernel spares it, key-sound server
  patched to run without MMF).
- **Display:** the path works (WSERV → ScDv 16MU device at our frame buffer, hardware rotation via
  `DisplayDriver`). The screen stays dark because (a) the splash logo renders empty and (b) no
  other window is drawn yet: SplashScreen keeps being dismissed and restarted by Starter, i.e. the
  startup sequence (Startup.exe / SysAp) has not reached the idle state.
- Next: find who sets `KPSUidStartup` (0x101F8766) key 0x301 and why the startup app does not
  finish (kernel `SYBPROP:` trace, patch 0004); then why AknIconServer renders SVG icons empty.
- Tooling added: `tools/pcsample.py` (guest PC sampling), `SYBORG_GDB=1|wait` for
  `scripts/hybrid-boot.sh`, `romcompose.py --patches` (verified byte patches), stub control hooks.

**Audio (paused 2026-11-05, owner's call):** QEMU `syborg-audio` device (PCM, tones, MP3/AAC via
FFmpeg; `scripts/run-gui.sh` plays through PipeWire), DSP stand-in streams the raw PCM node (startup
jingle works on `build/n95/hybrid-exp.img`, composed with the key-sound/startup-tone byte patches off by `scripts/compose-exp.sh`).
Open: key tones (DevSound request refused; audio-route reply 77/01→09 added, unverified), AAC/MP3
nodes untested (trigger: Profiles → ringing tone preview), working ROM still has the audio patches.
Details: docs/devices/nokia-n95-8gb-rm320/nokia-dsp-playback.md, docs/hacks.md (H-dspbridge).

**Queue (2026-11-05, from the project owner):**
- Done (run 42): Startup "hands" animation, first-boot region/date/time queries and the idle screen (fake modem answers a definite "no SIM"). Done (run 44): Phone.exe comes up (DSP bridge + DVFS stand-ins let MmfAudioServer start), publishes 213, the phone-ready skip patch is gone and the full Active Standby idle screen stays up.
- Screensaver partial display: implemented (DisplayDriver ctl 3/4/5 → Syborg fb PARTIAL_* registers, rows outside the band black, 8-colour idle mode); verified by poking the registers, not yet seen from the real screensaver (the idle screen does not stay up long enough: Phone/audio).
- White band at rows 240–319 and a stuck "SysAp: Feature not supported" note: half-initialised UI;
  expected to go away as SysAp/Startup get their missing features. Not a display bug.
- Done: host mouse input to the Syborg pointer is off by default on modern QEMU
  (`-global syborg-pointer.input=on` to enable); S60 3rd edition has no touch screen.

**Fixed (2026-11-06): home screen lost focus after the menu ("frozen idle").** Cause: no SIM, so
Startup switches to the Offline profile, which sends `MTC_STATE_REQ` (RF_INACTIVE) via DosServer;
the fake modem did not answer, the switch failed ("failed to activate offline mode"), the phone
stayed in General. Phone.exe (FUN_8170b898) brings "Standby mode" back to the front on a foreground
event only when SIM status (P&S 0x101F75B6/0x100052E9) != 5 (removed) or the Offline profile is
active; otherwise Phone itself (window group "Telephone", nothing drawn) keeps the focus. Found
with `src/n95apps/wgprobe` (focus/window-group order) and `syb_gdb brk 0x8170b898`. Fix: MTC_STATE_RESP
0x64 OK in `iscpeer.inl`. Keyguard unlock (F5 then F7 or `*`) was never broken.
**Open: keyguard emergency-number check.** While the keypad is locked every key sends
`CALL 0x28` (emergency number check, UCS-2 key string, from NOKIATSY in c32exe) and waits ~9 s for
a reply the fake modem does not give; keys pressed meanwhile seem lost. Needs the `0x29` reply
layout (NOKIATSY parser not located yet). Screensaver band clipping fixed 2026-11-06 (WSERV
rotation index 0 = landscape picture, now 1280-byte lines and rotated by the fb; H-lcd-native-landscape);
orientation confirmed by the owner.

**Open: no 3D/GPU (PowerVR MBX) — "Carousel: Memory full" (found 2026-11-06 with sydb).** Carousel
(`carousel.exe` UID 0x10282bf6, the N95 8GB multimedia menu) renders through `hitchcock.dll`
(Nokia's Hitchcock UI toolkit) on OpenGL ES 1.x: `libgles_cm.dll` → `libimgegl.dll` →
`services_um.dll`, which does `User::LoadLogicalDevice("services")`. Nokia's kernel half of the
PowerVR stack (`services.ldd`, `pvrsrv.dll`, `pvromaplcd.dll`, `omaplcd.ldd`) is not in our kernel
ROM, so that returns −1 [verified, gdb], `eglInitialize` fails with `EGL_BAD_ALLOC` 0x3003
[verified], and Hitchcock (hitchcock.dll+0x3942c) maps that to `User::Leave(-4)`. Eikon shows that
as "Memory full. Close some applications and try again". It is not a real memory shortage. It
happens when Carousel builds its UI (it is in the foreground, e.g. `sydb launch 0x10282bf6`), not
while it idles in the background. Other GLES/M3G users will fail the same way. Fix = a GPU model
(PowerVR services driver or a GLES stand-in), not planned yet.

**Planned: GPU (OpenGL ES 1.1 + EGL), 2026-11-06.** Plan in [gpu-roadmap.md](gpu-roadmap.md):
- keep the ROM's `libgles_cm.dll` and redirect its 175 entry points (generated code-patches) to our
  own GLES/EGL implementation, built for a fixed address in our part of the ROM;
- software rendering on the phone first (owner: no host GPU for now), split into a front end and a
  back end so a QEMU device can take over the back end later;
- frames shown through direct screen access (as Nokia's WSEGL does), vsync-paced;
- milestones: ZigGLTri (triangle) → ZigGT2Car (textures/lighting) → Carousel/Hitchcock → M3G.
Owner decisions pending: entry-point patches vs. replacing the DLL, own renderer vs. a base library,
colour depths, where the vsync wait lives.

**Planned: phone skin (emulator window with hardware buttons), 2026-11-06.** Plan in
[phone-skin.md](phone-skin.md): a PyQt6 `sydb phone` window (VNC screen, button bar, slider). Side
keys need only keymap entries. The slider and accelerometer need stand-ins for Nokia's kernel-side
`keypad.dll` / accelerometer drivers, which are not in our ROM.

**Fixed 2026-11-06: game audio played too fast** (the DSP stand-in now takes each raw PCM stream's rate and channels from the N95's own EAP 0 / RawGwc messages, docs/research/dsp-pcm-format.md; Crash Bandicoot = 22050 Hz mono, jingle 44100 Hz stereo). Was: Crash Bandicoot Nitro Kart 3D's sounds
(MediaClientAudioStream, CMdaAudioOutputStream) play sped up on run-gui. Suspects: the stream's
sample rate not honoured by our audio path (DevSound/DSP stand-in or syborg-audio resampling), or
buffers consumed faster than real time. Check with `sydb strace` on the audio server and the
syborg-audio device's rate handling.

**Keys (2026-11-06, patch 0015 fixed):** QEMU sends F13-F19 as set-1 0x5d/0x5e/0x5f/0x55/0xe003/
0xe077/0xe004 (not 0x64-0x69), so the first 0015 filled unreachable slots. Now: F13 = EKeyDeviceA,
which akncapserver turns into the **landscape** layout switch (the N95's media slide; whole UI),
F14 = EKeyDeviceB = **portrait** (sydb key aliases `landscape`/`portrait`; `sydb phone`'s Landscape
button). F15/F16 = EStdKeyApplication19/1A (camera app captures them; camera untested).
F17 = EStdKeyApplication7: akncapserver sets P&S 0x101f876e/4 = 1, F19 = App8 sets it to 0 (meaning
unknown, not the Multimedia key; F18 = 0xe077 would index past the 0xf0-entry table).
**Multimedia key found (patch 0016):** the N95 keypad.dll matrix table (0x8014f6d4, three slide
variants) gives the codes; F20 = EStdKeyApplication6 opens Carousel, F21 = Application1D Gallery,
host Play = Application2 the music player; App3/4/5 (stop/prev/next), App1B/1C (variant keys) and
App19/1A (camera) not verified. The
"sideways Carousel" was the landscape layout left on by an F13 test; Carousel itself uses no fixed
orientation. Also open: Carousel's boot instance still dies with USER-EXEC 3 on some boots
(the race at its ConstructL); `sydb fg` on its window group then shows only the backdrop.

**RomPatcher+ (2026-11-06, owner's device): works.** It hung the phone because its `patcherShadow.ldd`
(built for the 9.2 kernel) crashed our kernel; fixed by matching the N95 kernel's driver ABI
(H-kernel-abi-92, `tools/personality/kabi.py`); it now lists and applies its patches. Old notes: Its drivers only use `Epoc::AllocShadowPage` / `CopyToShadowMemory` /
`FreeShadowPage` / `Epoc::RomHeader` / `Kern::SuperPage`. Options: (1) a 9.2 kernel-ABI personality
for RAM-loaded drivers (loader marks the code segment; imports of DObject/DLogicalDevice/
DLogicalChannelBase constructors, destructors, SetName, Close... go to 9.2-layout shims; the kernel
talks to the driver's device/channel through proxy objects that call the 9.2 vtable slots); a
contained but real job. (2) Apply the patches RomPatcher+ would apply at ROM composition
(tools/romcompose.py `--patches`), which needs no driver at all. Also seen in the log: many recovered
kernel faults in `__magic_address_readdesheader1` from `SessionSendSync` (clients passing small
integers as descriptor pointers, e.g. TmScanServer, Anykey): worth checking against exec92's IPC
argument-flag translation.
