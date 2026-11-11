# N8 (RM-596) bring-up notes (2026-11-07, in progress)

Firmware 011.012 (Europe, the launch software of 2010-09-18; product 0598980 euro1_dg_uk_ireland: core +
Euro_1 rofs2 + 00.01 rofs3). Symbian^3. The archive is in roms/_archives/RM-596_011.012_euro.zip. Until
2026-11-07 the bring-up used the China package (product 0598807, Chinese_PRC rofs2 + C01.01_PRC rofs3); its
core image is byte-identical to the Euro one, so only ROFS2/ROFS3 changed.

What the probes said [V, tools/fw/probe.py, inventory]:
- **Kernel ABI = ours**: every kernel call identified through exports has our own number (our EKA2
  sources are the Symbian^3 kernel), so `platform.personality = "native"`: the first device without one.
- **Memory model: flexible.** Same ROM base (0x80000000) and kernel data address (0xC8000000) as the
  multiple model, which first misled probe.py; the ROM's DLL data at 0x7f000000 (the flexible model's
  local data end) tells them apart (probe.py header now says so). Kernel build `syborgfx`
  (`tools/baseport_variant.py syborgfx --memmodel flexible --cpu arm1176`), bootstrap table shared with
  the multiple model (patches/qemu-baseport/0018), and romcompose now copies the firmware's DLL data
  fields (iUserDataAddress/iTotalUserDataSize) into the composed header: the flexible model asserts
  the address and reserves the area.
- **CPU: ARMv6K.** EUSER reads the user thread-ID register (`mrc p15, 0, r0, c13, c0, 2`, TPIDRURW) for
  its thread-local data. ARM1136 r0 (our other phones' CPU) faults on it. ARM1176 kernel build
  (CFG_CPU_ARM1176 / __CPU_ARM1176__: the kernel then switches TPIDRURW per thread; now part of
  `syborgfx`), QEMU `-cpu arm1176` with a valid cache type (patches/qemu-modern/0004), and a bootstrap fix
  (patches/qemu-baseport/0017: ARM1176 is ARMv6 *with* memory type remapping).
- **Flash: 136 MB of ROFS** (ROFS1 99 MB + ROFS2 44 MB): `board.flash.size_mb = 256`. The size was a
  bb5-s60 family value; it moved into every member's profile (docs/devices.md "Families").
- **Display**: 360x640, natively portrait (wsini rotation 0), Color16MAP; window server render stages
  `alf` + `compositor` (ScreenPlay on the Broadcom VideoCore GPU: vchi, vcdriver, vcdispman, khegl,
  surfacemanagerdriver). Not reached yet.
- **Modem**: ISA Access (isaaccessldd.ldd, logical device "IADDriver") replaces the ISC driver stack;
  the same IscApi.dll and ISI frames. Without it CTelServer panicked `IADUserApiPanic 5` (loading the
  LDD failed), then KERN 4. Now the `isaaccessldd` stand-in: iscpeer.inl built with ISC_ISA_ACCESS
  (channels 0..63 by info descriptor, other control numbers, cancels via DoCancel);
  docs/devices/nokia-n8-rm596/isa-access.md (research report).
- **Personality symbian3** (fixed 2026-11-07): start-up reached system state 100, then tzserver died
  KERN-EXEC 1 and sysstatemgr's CleSrv STDLIBS-INIT 1, in a loop. KERN-EXEC 1 there is `EAccessDenied`
  from `ExecHandler::MutexSignal` (not a bad handle): `TZ_GlobalMutex` was signalled but never held.
  The N8's `RMutex::Wait()` passes no timeout (9.2 shape) and our kernel read a garbage `r1` < -1 as
  the timeout: `KErrArgument`, mutex not taken. The exec numbering is ours, so personalities/symbian3
  is our table plus the `MutexWait92` shim (registry 4). The RHeap::Alloc frames were stale words of
  the call-stack scan. Found with `sydb catch add --at` on the access-denied branch (sydb's paused
  start and the per-device User::Panic address had to be fixed first).
- **Window server, EGL, composition** (2026-11-07): gcebe (H-gcebe) keeps the window server alive;
  our libEGL/libOpenVG (H-khr) give alf its window surface, composed by gcebe. A diagnostic build with
  the RI's own (software) rasteriser, not kept, showed the white Nokia start-up screen with the logo:
  every stage from alf to the display memory works; M2 puts the drawing on the host GPU.
- **Start-up through the audio server** (2026-11-07): the system-permanent !AudioServer thread came
  up only after: AUDIO 0B 04 and 0B 5D replies (Aamm.dll dispatch 0x805e75be), the FM radio control
  made sessionless (H-fmradio-nosession: the radio HAL needs the BT/FM chip), a USB audio driver
  stand-in, and the RAPIDO DSP reply F3 31 at the N8's length (seven entries, 68 bytes;
  `tools/isi.py` replies may now carry 64 bytes). Found with `sydb catch` break rules on
  `User::Leave` / `__cxa_throw` filtered to the thread. Start-up then runs on: startup.exe,
  phoneui.exe, menu and widget servers start; KPSUidStartup 41 -> 103. Screen: white (Startup stage).
- **SIM/accessory replies** (2026-11-07, docs/devices/nokia-n8-rm596/isi-unanswered.md, research
  report): 09 DE, 09 09 (NOKIATSY's request at ~61 s) and 1C 38 (SAServer) answered and delivered;
  the phone runs on (lights adaptation at ~100 s) but start-up stays at 41 = 103 with a white screen;
  next unanswered: 09 13 (SIM), and the haptics 1C 9E.
- **MDAIF buffer on the flexible model**: the stand-in's DSP window is now a shared kernel chunk opened
  in the client's process (a user-RW DPlatChunkHw is not user-accessible on the flexible model). The
  N8's AudioProcessingEngine still writes exactly past the end of any window size (RAPIDO shared-memory
  state not modelled) and dies KERN-EXEC 3; it is not system-critical and start-up continues.
- **Start-up to the first-boot UI** (2026-11-07), found by reading the code (startup.exe decompiled;
  the SSM command lists' SF sources in oss.FCL.sf.os.devicesrv):
  - Startup's state machine: state 1 -> 8 waits for KPSGlobalSystemState (101f8766/0x41) in
    {104 critical phase OK, 105, 109..111}; state 9 -> 11 waits for KPSStartupAppState (100058f4/1) = 2
    (EStartupAppStateStartAnimations, set by the "UI started" SWP list after the device-lock check).
  - The system sat at 103 (ESwStateSecurityCheck): the security-check list's SIM security custom
    command waits for the start-up adaptation, which asked RTC 19 19 (alarm query) -> reply 19 1A
    status 0 (no alarm). Then 104.
  - Startup then waited in its animation for the start-up tone (H-startuptone-none, as the 5800).
  - Result: Startup's phases 101 -> 105, app state 3 (finished); the first-boot language query is
    on screen, drawn through our EGL/OpenVG on the host GPU (sydb `tap X Y` added for touch tests).
  - Also fixed: the exyin stand-in queued its DFCs from a thread with the kernel unlocked (the udeb
    kernel faulted on the first tap); kernel patch 0019 logs KPSUidStartupApp and KPSUidUikon.
- **RAPIDO DSP ring** (2026-11-07, docs/devices/nokia-n8-rm596/rapido-audio.md): the engine's
  KERN-EXEC 3 was aamdaif.dll wrapping its write position at a ring size of 0, the out word of
  MdaifDriver ctl 8 we never wrote. The stand-in now returns a ring per stream (`ring_units = 16`
  x 1920/960/320 bytes in its own 128 KB of the window) and the fake modem answers F3 0x34 (data
  written) after half a ring's play time (a virtual DSP clock, `DIscPeerChannel::ReplyLater`), plus
  F3 0x36 -> 0x37. Result: !AudioProcessingEngine lives, a stream writes 0x4b00-byte blocks every
  ~82 ms (write position wrapping at 0x7800) and stops cleanly; no audio panic left in the log.
- **Open** (2026-11-07): hwrmhapticsserver repeats ISI 1C 9E (no reply); EwSrv's FeedbackPlayerThread exits -33 repeatedly;
  radioadaptationserver still crashes (null plugin) whenever a client connects; ESock_Bt on demand;
  unanswered 09 DE / 09 09 (SIM) at ~61 s.
- **Earlier blocker** (2026-11-07): the window server. `CVCCompositionBackendServer::ConstructL() -
  error loading dispman logical device`, panic `GCEBE Srv Thread 2` in EwSrv's
  VC_COMPOSITION_BACKEND_SERVER thread, then `Wserv` KERN-EXEC 0 and KERN 8 (system-permanent).
- **Not blocking, left for later**: `sospmmhandler` panics "No PMM partition found from NAND -5" (the
  product-data partition is not on our flash); MobileCrash (`mobilecrashstarter`): `mc_isiserver` opens
  ISA channel 55, closes it and is killed -1 (no ISI sent, nothing unknown asked of the stand-in), then
  `mc_useragent` panics KERN-EXEC 0 in `ChannelRequest` (slow exec 10) on a channel it never opened.
- **Touch**: Synaptics controller (synaptics.dll) behind exyin.dll: the exyin stand-in (single touch).
- **Drives**: E: = 16 GB eMMC on local drive 3, F: = SD card on 2 (the eMMC content image
  RM596_029_000_M025_16G.fpsx is not in the data package; not needed to boot).

## Status 2026-11-07 evening

- Firmware: the Europe package (product 0598980). Fresh boot: home screen, then the first-boot wizard
  (language, Phone Switch, country, date, time); start-up done (KPSUidStartupApp 3) at ~27-30 s.
  Apps open (clock ~1 s); some hang or do not work yet.
- Fixed today: alignment faults in Nokia code (kernel built without alignment checking: the thumbnail
  server and media apps), alf's out-of-heap crash on a 360x640 image copy (RI patch 0006), missing
  icons (images rendered on the host read back on CPU access, RI patch 0005), the 3 s stall of every
  touch feedback (H-haptics-fail).
- Audio is silent by design for now: the RAPIDO ring model plays nothing to the host, and the start-up
  tone is patched out (H-startuptone-none).
- Graphics: frames composed and presented on the host GPU, 8x MSAA, gradients
  (docs/devices/nokia-n8-rm596/vg-host.md, composition.md); review and plans in docs/gpu-host-review.md.
- Speed: docs/perf.md.

## Firmware 013.016 (Russia), 2026-11-07

`models/nokia-n8-rm596/firmware/013.016` extends this profile (013.016 of 2011-01-27: the update after the
launch software, still Symbian^3, not Anna 022.x). Files: roms/RM-596/013.016 (Russian
rofs2 04.01 + generic rofs3 00.01; archive roms/_archives/RM-596_013.016_russian.zip). What had to
change, all addresses: the user window starts at EDISP.DLL 0x8041F810; fmradioaudiocontrol.dll moved
by -0x4390 (code unchanged), so the H-fmradio-nosession patches move with it; gcebe's imports resolve
against this firmware's symbol index (src/gcebe/build.sh now takes a variant's import list from the
parent). Everything else (stand-ins, ISI replies, ROFS patches, khr, composition) works unchanged.
Result: start-up done at 23.2 s, language query (Bulgarian, English, German, Hungarian, Romanian,
Russian, Ukrainian), wizard, home screen, clock and Photos as on 011.012; the same known panics.
`SYBORG_DEVICE=nokia-n8-rm596-v13 python3 tools/sydb/sydb.py phone --fresh`.

## Firmware 025.007 (Symbian Anna, Russia), 2026-11-08

`models/nokia-n8-rm596/firmware/025.007` (025.007 of 2011-09-16, product 059G159: Russian rofs2 05.01 NO_FMTX +
generic rofs3 00.01; files in roms/RM-596/025.007). The kernel ABI is the Symbian^3 one (the
SWI report is identical to 011.012's), so the variant needs only addresses: user window at EDISP.DLL
0x8041C5D0, fmradioaudiocontrol.dll at 0x80fd8d98 (code unchanged), gcebe rebinding from its own index.

One real bug found on the way, in our baseport (patches/qemu-baseport/0019): the LCD extension
allocated the secure frame buffer separately but mapped `ivRamPhys + vSize`, i.e. assumed it adjacent to
the first one. Anna's smaller ROM moves RAM so that the page after the frame buffer holds a page table:
DHardwareMemoryManager::AssignPage asserted at 0.47 s. Now it maps the address it was given.

Result: first-boot queries (language, country, date, time), home screen and menu; the agent comes up
through the boot hook. The idle P&S value (101f8766/46 = 104) is set by CherryWatchdog on Anna, so the
variant's milestone matches it from any process.

The black home screen (fixed 2026-11-08): the home screen blocked in its connect to
!ConnectionMonitorServer, which was blocked in an ETel request (20054) that the TSY could not finish:
the Symbian^3 TSY sends the network messages to PN_MODEM_NETWORK (0xC8) instead of PN_NETWORK (0x0A),
and NET_RAT_REQ (36) and NET_MODEM_REG_STATUS_GET_REQ (00) went unanswered there. Now answered like their
0x0A twins (platform/isi/replies-bb5.txt). Found with the IPC trace build: last MsgAc without MsgCo,
followed server by server to the ISI send. 011.012 sends the same requests but nothing waits on them.
`SYBORG_DEVICE=nokia-n8-rm596-v25 python3 tools/sydb/sydb.py phone --fresh`.

## Firmware 111.040.1511 (Belle Refresh), 2026-11-08: in progress

`models/nokia-n8-rm596/firmware/111.040.1511` (111.040.1511 of 2012-07-28; core + Russian rofs2 04.01 + generic rofs3
C00.01; files in roms/RM-596/111.040.1511). Belle changes the kernel ABI; what is done:

| Step | Finding | Change |
|---|---|---|
| exec numbering | the Symbian^3 numbering up to S10a, then ten new slow calls; S10b..S10d run on every process start and library load and go on only on 0 | personality `sym101` (personalities/sym101, kernel patch 0020): shim Ok101 for S10b..S10d, S10e..S114 unmapped (logged) |
| user stack top | Belle's TLocalThreadData has one more word at +12: the thread's own ID, which `RThread::Id()` of the current thread reads (no kernel call). Its DLL TLS array (now user side) is at +20 and ran past the stack top (efile KERN-EXEC 3 at 0.34 s) | the kernel reserves the personality's size (0x38) and stores the thread ID at +12 (KernLocalThreadDataSize / TlsShift / IdOffset); without the ID every drive thread failed F32's IsDriveThread check (Fserv fault 122) |
| C: first boot | after formatting the blank C: the file server forces a media change that reopens the media driver; our NVMemory media refused it (not a "removable device"), the file server tried to remount the old (blank) volume, failed (KErrGeneral), left the drive inconsistent: scan drive -20, every open -14 | NVMemory media (baseport patch 0020): reopenable like the phone's socket media, a real destructor (interrupt, DFCs, transfer buffer), V4 sector geometry in the caps, requests it does not implement refused before they block the queue |
| GLES forwarders | libGLESv1_CM.dll reads the VideoCore chip type from P&S 0x2726BEEF/0 and panics "LibraryLoader::L -1" without it; 0 = VideoCore III (the N8's BCM2727) -> khclient_vc3.dll | stand-in `bootprops` (kernel extension, profile `config = { props = "2726beef/0=0" }`) |
| TV-out | the window server's TV-out plugin sends DisplayDriver control 3 (1,0,1,1); our d_display read it as the N95's partial display mode and refused it, the plugin panicked the window server | `d_display.partial = 0` for the N8 (its driver has no partial mode) |

Next stop (~5 s, then the phone restarted): apsexe died with KERN-EXEC 3 in backend.dll (P.I.P.S.): its
static data had been initialised a second time in the same process. The KDLL trace shows why: in
sysstatemgr, attaching a library with 78 entry points runs Belle's GLES forwarder constructor, which loads
khclient_vc3.dll; that nested LibraryAttach ignored the outer library (still EAttaching), listed backend.dll
and its neighbours again and DEpocCodeSeg::InitData zeroed their data under the constructors. For sym101
the kernel now counts an attaching library as loaded (only this thread can be attaching: it holds the
process DLL lock), KernNestedAttachCountsAsLoaded.

Update 2026-11-08 (later): with the PN_MODEM_NETWORK replies Belle's UI runs too: status bar, clock,
the home screen with its widgets still loading (spinners), the bottom bar. The UI phase stays at 105
(101f8766/46), so the idle milestone does not fire yet. New unanswered request: RES_C9 msg 24.

## Delight 6.7 (custom firmware on Belle Refresh), 2026-11-08

`models/nokia-n8-rm596/firmware/111.040.1511~delight-6.7` extends the Belle Refresh profile: the owner's real N8 runs it. Its core
image is modified (164 MB vs 131 MB stock; sw.txt dated 2012-08-27), with the stock Euro_2 rofs2 02.01 and
the Germany rofs3 C14.01. What it needed:
- the ROFS set is 208 MB: QEMU's generic loader caps a raw image at the RAM size, so the board now reads
  the ROFS file into its flash window itself (`syborg-platform.flash-file`) [V];
- Delight's ROFS1 adds kernel drivers for the real phone (lightpdd.pdd, patcherS3.ldd, MemSpyDriver.ldd,
  wlanpdd.pdd). Its lightpdd.pdd shadowed our LightPdd stand-in on the composite Z: and lightsaserver
  left -5 -> KERN 102; replaced by our stand-in (H-delight-own-drivers) [V].
Then it boots to the home screen (fresh drives: no wizard shown at 120 s). Deaths are the same set as
stock Belle (sospmmhandler, mc_*, AccServer ASY threads, usbsvr, radioadaptationserver KERN-EXEC 3,
wlmserverexe -5). Next: the oracle on the owner's phone, then the differences.

## Open issues reported by the owner (2026-11-08)

- All N8 firmwares: a "widget removed" note at boot (a home-screen widget fails to initialise).
- Belle: all widgets keep a loading spinner.
- Anna: opening the menu turns the screen black and the phone seems to hang (the menu opened in the
  earlier test, before the home screen was unblocked; to recheck from the seeded home screen).
- Slow UI: some actions take seconds (Settings -> any item), as on the 5800.
- Boot splash, NOKIA wordmark missing above the dots (revised 2026-11-10): not a rendering bug, and not an
  opt-in tweak. Stock SplashScreen.exe plays only the dots loop (icons 11-18 of `Z:\resource\apps\
  splashscreen.mif`); the wordmark comes from Delight's splash mod (`Z:\data\sys\bin\SplashScreen.exe`,
  `Z:\data\resource\apps\splashscreen.mif` with the wordmark frames, `Z:\data\system\domainsrv\
  DomainExc.cfg` = `START c:\sys\bin\splashscreen.exe` for its modded domainsrv.exe). Delight installs it
  by itself on its first boot [V]: `Delight.exe` is in the start-up list `Z:\private\2000D75B\startup\0\
  noncriticalcmdlist_ext.rsc`; it runs MiniCMD (`Z:\system\Apps\MiniCMD\Delight.ini`: drives ECZ,
  `vlls\Delight.txt`), whose `Z:\vlls\Delight.txt` ("Welcome to Delight!", until `C:\system\Cfw.txt`
  exists) chains to `Z:\vlls\D02.del`: `copy -o -r z:\data\{resource,sys,private,system}\ c:\...`,
  writes Cfw.txt and restarts. In qembian that never happened: no program of the non-critical start-up
  list starts at all (Delight.exe, lockapp, contentharvester, schexe, satserver, javacaptain, ...: no
  SYBCODE line) [V], so the drives never got Delight's first-boot copy. Open: why the non-critical list
  does not run (likely also the Belle widgets' endless spinner: contentharvester).
- OpenGL ES: 1.1 through sygl (ZigGLTri, ZigGT2Car) and 2.0 remoted to the host GPU (Angry Birds 2.0
  title screen), docs/devices/nokia-n8-rm596/gles2-host.md. Open: landscape apps' layers (source
  rotation of the compositor render stage) are shown unrotated.
- Factory user content is not on our drives: the package's UDA (`*_U01.01_79u.uda.fpsx`, a FAT32 image of
  the user data area) and `APE_ONLY_ENO` are not unpacked; no ROFS layer has stock wallpapers, so they
  are most likely in the UDA [I]. All three ROFS layers are loaded; the app registry lists 169 apps on
  Delight (its winRAR and ROMPatcher+ included).
Plan and first steps: docs/roadmap.md "Open issues reported by the owner".

## Start-up reboot loop after the modem moved into QEMU (2026-11-10, fixed)

Symptom: Delight and stock Belle stop at KPSGlobalSystemState 103, set 117 (fatal) ~10 s later, saserver sends
MTC_RESET_GENERATE_REQ and the phone restarts, again and again [V].
- Bisect [V]: a ROM with the stand-ins of fdaec45 (modem still in the guest) on a fresh copy of the owner's
  drives reaches 105; the current ROM loops. NVMemory patch 0022, code patches, QEMU arguments ruled out;
  the ISI bytes of both boots are identical.
- Mechanism [V, KIPC trace]: the state manager's transition to normal (DmManagerServer) waits for every
  domain member. phoneui never acknowledges the second transition because its main thread is blocked in
  RPhone::SetEmergencyClient (ETel fn 88). ETel runs fn 88 with KReqModeFlowControlObeyed
  (SETEL/ET_CORE.CPP), so it waits behind an earlier flow-controlled TSY request that never completes; it
  never completes on the good build either (phoneui makes the call ~2 s after acknowledging there).
- Why the order changed [V, A/B traces]: the QEMU modem raised its receive interrupt for replies the guest
  reads back in the same send. The ISR + DFC let other threads run between saserver's SIM_STATUS_RESP and
  the SIM indication behind it (`09 EF 05 06`, [modem].indications): the TSY's next request went out first,
  saserver's SIM-status and MTC requests swapped and the state manager ran a second transition while
  phoneui was starting. Fix: no interrupt for frames produced during a send (syborg_modem.c `hold`).
  Afterwards the modem order matches the in-guest modem's, 105 at 22.3 s, home screen.
- The request in front of fn 88 [V, fixed 2026-11-10]: emergencyservice.exe's ETel fn 23036, for which
  nokiatsy sends `C9 24` on channel 7; unanswered, it never completed. Reply `C9 25 00 00` (no cause
  sub-block = KErrNone, parser nokiatsy 0x80e8d940; platform/isi/replies-bb5.txt): fn 23036 completes in
  2 ms, fn 88 returns -5 at once, phoneui runs on (its Options menu opens; without the reply it does not
  react). The 701/808 send the same request.
- Open: the Phone app's dialer area is black (toolbar and menu draw).

## Belle: Menu and Power keys do nothing (2026-11-10, open)

Owner: broken on N8 Belle (111.040.1511, Delight, Reborn = personality sym101); fine on 011.012, Anna, 701
and 808 (sym102). Found so far [V], Delight copy:
- The keys arrive: F8 -> QEMU sends raw 0xB4 (EStdKeyApplication0, keymap.qemu, bit 30 = no table) ->
  Kern::AddEvent; each press resets inactivity and the light server turns the backlight on (ISI ch 52).
- Nothing else happens: a KIPC trace over a Menu press shows no window-server completion to any client
  (22 messages in 7 s), short and long press alike; focus stays on Idle.
- Same on N8 Belle and the 701: keymap.toml, EKTran.dll (3888 B), EKData.dll (3012 B), keyrouter.dll
  (1094 B), wsini.ini key settings (only a render stage differs). sym101 and sym102 exec maps differ only in
  three slots (none event-related); the UserSvr event calls and the security-info calls map "SAME"
  (abi/swi-n95-vs-ours.txt).
- Hypothesis [I]: Belle's key router (KRestrictedKeys in the SF keyeventrouting source: Menu only for
  HomeScreen/SysAp/AutoLock/Phone/AknCapServer, Power only for SysAp/AutoLock, matched by the capturing
  app's UID) refuses the captures, e.g. because the UID it sees for those processes is wrong on sym101.
Next: on N8 Belle vs 701, read keyrouter.dll's AddCaptureKeyL result for sysap/akncapserver (gdb on the
running copy, or a catch at its IsRestrictedKey), and what UID wserv passes.

### Review of the key diagnosis (2026-11-10)

Static review only; no new guest reproduction or proven root cause. The scope above matters: this is
reported for **N8 Belle**, not all Belle/newer firmware (701/808 are reported working). The model's older
`power key: no power menu` notes also mean Menu and Power must not automatically be assigned one cause.

The GUI/QEMU/baseport path is consistent with the recorded raw codes: the N8 keymap maps F8 to 0xB4,
F4 to 0xA6; `0014-keyboard-qemu-translation.patch` strips the QEMU translation marker and calls
`Kern::AddEvent` with EKeyDown/EKeyUp. No new evidence supports changing these scan codes.

There are two gaps in the previous interpretation:

- Capture restrictions use the **client SecureId**, not the executable UID3 or PID. In the available SF
  source, `windowing/windowserver/nga/SERVER/CAPKEY.CPP` (`CmdToRequest`, line 55) fills `iAppUid` from
  `iWsOwner->SecureId()`. `openwfc/CLIENT.CPP:2015` gets it from `iClient.SecureId()`.
  Inspect the actual capture's SID and result: SysAp 0x100058f3, AknCapServer 0x10207218,
  HomeScreen 0x20022f35. `CheckCaptureKeyL` can reject malformed modifiers/priority with -6 as well as
  disallowed identity with -46. Matching executive-call numbers alone does not prove correct returned
  security data, but there is no measured bad SID yet.
- In that reference `keyeventrouting/src/keyrouter.cpp:304`, **no matching capture falls back to the
  focused window group**. Restrictions reject capture registration; they do not themselves silently
  discard every uncaptured key. Therefore “no WSERV IPC completion” does not establish a restricted-key
  failure. An already queued event/no outstanding client request can also change completion observations.

An overlooked earlier interception point is worth checking before concentrating on captures:
`windowing/windowserver/nga/SERVER/EVENT.CPP:1166`, `TWindowServerEvent::ProcessRawEvent`, offers input
to registered raw-event/animation handlers **before** key translation or routing. If any
`OfferRawEvent` returns true, it returns without reaching `QueueKeyUpDown`/`ProcessKeyPress`.
That can explain input activity with no application key event, without blaming the compositor.
The reference NspsWsPlugin additionally forwards raw keys to lights through P&S and returns false;
lighting activity is therefore not proof that a key reached normal application routing.
Do not disable these handlers wholesale: they implement legitimate system input behavior.

Recommended minimal differential trace on isolated N8 Belle and working 701 instances:

1. One Menu down/up: record `RequestEvent` completion and the raw event bytes as WSERV reads them.
   If absent/wrong, inspect kernel event delivery, request buffer layout and status completion first.
2. At `ProcessRawEvent`, log each raw handler's code address/module and boolean return. If consumed,
   investigate that handler's state/dependencies; stop guessing about keyrouter.
3. If not consumed, record `TranslateKey` result and raw up/down routing separately. At `RouteKey`,
   record type, scan/key code, modifiers, focus SID, matched capture and output destination/result.
4. Only if the capture is missing/wrong, trace registration during startup: SID, key/type,
   modifier mask/value, priority and success/leave code. Capture registration predates a key-press-only
   IPC trace. Check whether SysAp/HomeScreen reached registration at all before assuming rejection.
5. If routing queues the event, inspect the destination's outstanding event request/queue and event
   handler, including startup/keyguard state. Record Menu and Power separately.

The SF sources are guidance, not proof of the proprietary Belle binary's exact implementation. Resolve
these functions/indirect calls in the loaded ROM first; the existing note's equal DLL sizes alone do not
prove equal contents or caller/plugin structure layouts. This review deliberately leaves the cause open
until the first divergent stage is observed.

## Assassin's Creed: direct screen access in landscape (2026-11-10)

- [V] The game (S60 3rd edition GL API, libGLES_CM) first plays its intro video (`assassins.mp4`,
  CVideoPlayerUtility with a window: direct screen access). WSERV refused DSA every 3 s: "** Current Rotation
  Mode not supported by the DSA device" (nga/SERVER/openwfc/screen.cpp DoCreateDsaScreenDevice), because the
  screen device offered only 0/180 degrees: CScreenDeviceHelper::DeviceOrientationsAvailable asks HAL
  EDisplayOffsetBetweenLines with the flipped flag (0x80000000) and our display driver refused that mode.
  Fixed: patches/qemu-baseport/0024-fb-flipped-mode-info.patch.
- [V] Open: the game still shows black and loops every ~2 s (a WSERV command returns -6, a Z: file request
  -5; it polls CenRep fn 5 and ETel fn 39 every ~47 ms); no MMF video controller thread, no GL context yet.
- [V] Fixed (2026-11-10): the -6 was EWsDirectOpGetRegion: Initiate -> CWsWindow::AddDSA -> the window's
  background surface = the screen's DSA surface (TSurfaceId type 1, EScreenSurface: screen 0, HAL mode |
  flipped, ARGB_8888_PRE), which gcebe could not register (it opened every surface through the surface
  manager; screen 1, TV-out, has no display memory). The game resolved the error text (ERRORRES.R01) every
  frame. gcebe now registers screen surfaces (the display memory; without pixels on a screen it is not
  for), and the host composition no longer writes its output into the display memory: the composed screen
  stays on the host (syborg-gl keeps it; syborg-fb, the phone window via the frame file and screenshots show
  it), so the display memory is DSA's, as on the phone. The intro video (Helix, assassins.mp4) has no
  decoder (VideoCore: "Pictures Received : 0", ivepolicyserver killed -19): a tap skips it; then the
  loading screen and the title screen (OpenGL ES 1.1, 640x360) show. Found with the hex dump of binary
  IPC reads (kernel trace patch 0002: `IPCRD   hex`).

## N8 Belle start-up ends in "emergency calls only" (2026-11-11, fixed: belle-emergency-only.md)

Likely the common cause of the Menu/Power keys, the missing Autolock (DeviceLock context uninitialised) and the
context framework stuck at "Initializing" on N8 Belle (111.040.1511 stock, Delight, Reborn) [I for the keys].
Measured on fresh isolated boots [V]:
- `KPSGlobalSystemState` (101f8766/0x41) goes 100, 101, 102, 103 (security check), then **105
  ESwStateEmergencyCallsOnly** on N8 Belle. N8 011.012 goes 103 -> 104 CriticalPhaseOK -> 110 NormalRfOff;
  701/808 (Belle FP2) 103 -> 104 -> 110.
- The split is the SIM status (`KPSSimStatus` 101f8766/0x31; 103 = ESimNotReady, 104 = ESimNotPresent):
  011.012: 103 then 104 (final 104). N8 Belle: 104, 104, then **103** right after the SIM_STATUS service 01
  answer, then 105.
- Channel 35 (SAServer start-up service) sequence on N8 Belle: `09 C0 00` -> our `C1 00 06` (no SIM);
  profile indication `09 EF 05 06`; **`15 01 {0A 00}` (PN_MTC MTC_STATE_REQ, asked only by N8 Belle,
  not by 011.012 or 701/808)** -> our generic `64 {00 00}`; `09 C0 01` -> `C1 01 01`
  (sim_service_status 01=01); then KPSSimStatus = 103 and state 105.
- N8 Belle's sastartupservice maps the service-01 status like 011.012 (1 -> 111, 8/9/10 -> 108..110, 0x24 retry
  timer) [V disassembly 0x80e71a4e]; channel 35 does not subscribe to MTC indications (`15 C0`): an
  MTC_STATE_INFO_IND experiment changed nothing (not delivered) [V].
- 701 channel 35 subscribes PN_UICC (8c 05/0b/26), N8 Belle PN_SIM (09 EF) [V].
