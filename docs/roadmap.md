# Roadmap

Decision (2026-11-04, by the project owner): **option 1 first, then option A.** Options are
defined in [n95-compatibility.md §6](n95-compatibility.md).

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
  finish (kernel `N95PROP:` trace, patch 0004); then why AknIconServer renders SVG icons empty.
- Tooling added: `tools/pcsample.py` (guest PC sampling), `SYBORG_GDB=1|wait` for
  `scripts/hybrid-boot.sh`, `romcompose.py --patches` (verified byte patches), stub control hooks.

**Audio (paused 2026-11-05, owner's call):** QEMU `syborg-audio` device (PCM, tones, MP3/AAC via
FFmpeg; `scripts/run-gui.sh` plays through PipeWire), DSP stand-in streams the raw PCM node (startup
jingle works on `build/n95/hybrid-exp.img`, composed with the key-sound/startup-tone byte patches off by `scripts/compose-exp.sh`).
Open: key tones (DevSound request refused; audio-route reply 77/01→09 added, unverified), AAC/MP3
nodes untested (trigger: Profiles → ringing tone preview), working ROM still has the audio patches.
Details: docs/nokia-dsp-playback.md, docs/hacks.md (H-dspbridge).

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
layout (NOKIATSY parser not located yet). The screensaver band may be clipped: on the N95 8GB the
screensaver runs rotated (WSERV rotation index 0), check the partial-display row/column mapping.
