# Startup.exe: `ViewSrv 11` during the welcome animation (static analysis)

Problem: ~10 s into boot `Thread Startup::startup Panic ViewSrv 11` (and the same for the
process's `MMFControllerProxyServer-9d` thread), no "hands" animation, only splash → wallpaper.
Evidence tags: **[verified]** = read from the decompile/disassembly/image bytes,
**[inferred]** = reasoning from Symbian/S60 source knowledge not re-checked in the binary.

Inputs: `build/n95/ghidra/startup.exe.c`, `viewsrv.dll.c` (decompiled now),
`MediaClientAudio.dll.c` (decompiled now), `MmfAudioServer.exe.c`; disassembly of `startup.exe`
(`tools/xipdis.py`); ROFS2 `private/10202be9/101F8763.txt`; `build/logs/modern-land-serial.txt`.

## 1. What `ViewSrv 11` is

- The view server (`viewsrv.dll`, 0x80750000) panics a *client* with category `ViewSrv` through
  `RMessagePtr2::Panic` in `FUN_807511d4` (0x807511d4) **[verified]**.
- Code 11 (0xb) is raised at exactly one place, `FUN_80752586` (0x80752586), the time-out handler
  of a pending server event: `if (aTimeoutType == 2) { ... FUN_807511f4(session, 0xb); }` — i.e.
  the client that was sent a view-server *event* (activation/deactivation/screen-device change)
  did not answer within the *server event time-out* **[verified]**. The other branch
  (`aTimeoutType == 1`, client-request time-out) completes the request instead of panicking.
- The time-out machinery is a `CPeriodic` (`FUN_80751fbc` / `FUN_80751f5a`); the only large
  literal in the DLL is `10000000` µs at 0x80750a58 → default 10 s **[verified]** (matches the
  "~10 s into boot" observation). In Symbian source this is `EVwsViewEventTimeOut`
  ("client did not respond to a view event in time") **[inferred, name only]**.
- Both threads of the process show the panic because EKA2 kills every thread of the process with
  the main thread's exit info; the `MMFControllerProxyServer-9d` thread is the MMF controller
  thread that `CMdaAudioPlayerUtility` created inside Startup.exe — proof that the tone player
  *was* created and had loaded its controller **[verified from the log]**.

Conclusion: the Startup main thread's active scheduler was blocked for > 10 s, so the view-server
event could not be processed. The view server was *not* waiting for a view activation of
Startup's own; it was waiting for Startup to acknowledge any event (likely a deactivation /
screen-mode notification caused by the other apps starting) **[inferred which event]**.

## 2. Startup.exe structure (0x833a6e98, 22 756 bytes, XIP)

Class names present in the binary (RTTI strings at 0x833ab81c..): `CStartupAppUi`,
`CStartupTone`, `CStartupWelcomeAnimation`, `CStartupOperatorAnimation`, `CStartupUserWelcomeNote`,
`CStartupPSObserver`, `CStartupSubscriber`, `CStartupEventListener`, `CStartupMediatorObserver`,
`CStartupQueryDialog`, `CStartupPopupList` **[verified]**.

### AppUi members (offsets from `this`) **[verified]**

| offset | meaning |
|---|---|
| +0x78 | `CStartupWelcomeAnimation*` (control, created in ConstructL by `FUN_833a8cb4`) |
| +0x7c | `CStartupOperatorAnimation*` (created in state 11 by `FUN_833a912a`) |
| +0x80 | `CStartupUserWelcomeNote*` (created in state 13 by `FUN_833a87de`) |
| +0x84 | `CPeriodic` "animation finished" timer |
| +0x88 | `CPeriodic` user-welcome-note timer |
| +0x90 | `CStartupTone*` startup tone (type 1) |
| +0x94 | `CStartupTone*` operator tone (type 2) |
| +0x9c | `iInternalState` |
| +0xa4 | animation-finished latch |
| +0xd0 | `CStartupPSObserver` (`FUN_833a9b56`) |

### ConstructL order (`0x833a82e0..0x833a83b0`) **[verified]**

FeatMgr → `CStartupMediatorObserver` (`FUN_833a96ac`) → welcome-animation control
(`FUN_833a8cb4`, adds `CAknBitmapAnimation` from resource, 4200 ms nominal length stored at
control+0x44 = `DAT_833a8f44 = 0x1068`) → `AddToStackL` → three `CPeriodic`s →
**`CStartupTone::NewL(this, 1)` and `CStartupTone::NewL(this, 2)`** (`FUN_833a93d2` ×2 at
0x833a8364 / 0x833a836e) → DosServer client → PS observer → `SysStartup::State()`.

### State machine `FUN_833a7eea(this, newState)` (RVCT `__ARM_switch8` on the current state) **[verified]**

| cur → new | action |
|---|---|
| 0 → 2 | `FUN_833a7edc`: start-up checks (`FUN_833a80d4`: startup reasons, charging/alarm/normal) |
| 2 → 3 | `SysStartup::SetState(0xd3)` then → 5 |
| 2 → 4 | charging mode: `SetState(0xfa)`, global note |
| 5 → 6 | `FUN_833a7eb0`: SIM/PS check → 7 |
| 6 → 7 | `FUN_833a7e28`: offline-mode query (`CAknQueryDialog`, CenRep 0x101F8763 key 1) → 8 |
| 7 → 8 | `FUN_833a7d56`: `SecUi` security check, then `FUN_833a7d12` → 9 |
| 8 → 9 | `FUN_833a7cf2`: mediator command 0x65 or → 10 |
| 9 → **10** | **welcome animation**: control state 7, `DrawNow`, `FUN_833a7cac`, control state 9 |
| 10 → 11 | create operator-animation control, `FUN_833a7c4a` → 12 |
| 11 → 12 | `FUN_833a7c08`: operator animation (if any) + operator tone |
| 12 → 13 | user welcome note (3 s timer, `0x2dc6c0`) |
| 10/12 → 14, 13 → 15, 15 → 16 | first boot / RTC time query (`FUN_833a7972`), finish |
| any → 17 | fatal: `SetState(0xfc)`, global note |

### Animation and tone, state 10 (`FUN_833a7cac`, 0x833a7cac) **[verified]**

```
if (HiddenReset()) { tell Starter; state 14; return }
err = iStartupTone->Play();                       // FUN_833a9442
if (err != 0) KeySounds()->PlaySound(0x3fa);      // Avkon key-sound fallback (silent with H-keysound-nommf)
iAnim->iRunning = 1;                              // control+0x58
FUN_833a7c78: iTimer(+0x84).Start(len*1000, len*1000, AnimationFinished)   // len = control+0x44 (ms)
              iAnim->StartAnimationL()  (FUN_833a8e12 → CAknBitmapAnimation::StartAnimationL, Flush)
```

`AnimationFinished` (`FUN_833a7aa4`): cancels timer, sets control state 9; **if the tone is still
playing (`CStartupTone+0x10`) it sets tone+0x24 = 1 and waits for `MapcPlayComplete`**,
otherwise → state 11. `MapcPlayComplete` (0x833a94c8): `iPlaying = 0; if (+0x24) TRAP(FUN_833a81d6)`
→ state 11 (type 1) / 13 (type 2). A key press during the animation (`OfferKeyEventL`,
`FUN_833a8d96`) cancels it via `FUN_833a8d52`.

So the animation does **not** wait for `MapcInitComplete`; it only waits for `MapcPlayComplete`
if `Play()` succeeded. There is no time-out on that wait **[verified]**.

### `CStartupTone` (`FUN_833a9208` ctor, `FUN_833a92fe` ConstructL) **[verified]**

Layout: +4 `MMdaAudioPlayerCallback` vptr, +8 `CMdaAudioPlayerUtility* iTone`, +0xc `iToneLoaded`,
+0x10 `iPlaying`, +0x14 tone type, +0x18 AppUi, +0x1c hidden-reset, +0x20 volume, +0x24
"notify AppUi on play complete".

ConstructL:
```
iVolume = FUN_833a9220();           // ProfileEngine 0x101F8798: ringing type (0x7E000002) != 4 (silent)
                                    // and ringing volume (0x7E000008) not 0/1  → startup CenRep 0x101F8763 key 2 (=4)
                                    // else 0
iHiddenReset = HiddenReset();
if (!iHiddenReset && iVolume != 0) {
    path = CenRep 0x101F8763 key 3 (type 1) or key 9 (type 2);   // "Z:\system\sounds\Wakeup tone.aac" / ""
    if (TFindFile::FindByPath(path) == KErrNone)
        iTone = CMdaAudioPlayerUtility::NewFilePlayerL(path, *this, 50, 0x00310001, NULL);   // 0x833a93c4
}
```
`MapcInitComplete` (0x833a94b2): `if (err == 0) iToneLoaded = 1` — nothing else.
`Play()` (0x833a9442): `if (!iToneLoaded || iHiddenReset || !iVolume || !iTone) return KErrNotReady(-18);`
else `SetVolume(min(vol,10)); iTone->Play(); iPlaying = 1`.

ROFS2 defaults (`private/10202be9/101F8763.txt`, KCRUidStartupConf) **[verified]**:
`0x2 int 4` (startup-tone volume), `0x3 "Z:\system\sounds\Wakeup tone.aac"`, `0x9 ""` (operator
tone, so only the type-1 tone creates a player).

## 3. Where it blocks

- The design tolerates MMF *errors*: a failed open leaves `iToneLoaded = 0`, `Play()` returns
  −18, the key-sound fallback is used and the animation runs. It does **not** tolerate MMF
  *never answering* **[verified]**.
- `NewFilePlayerL` returns immediately, but the open continues from the main thread's active
  scheduler (`MediaClientUtility` `CMMFFindAndOpenController`, ords 12/13/19–24/32 used by
  `MediaClientAudio.dll.c` lines 60–82, 288–294, 740–749): controller load, `AddDataSource`,
  `AddDataSink` are each a synchronous `SendReceive` to the controller thread
  (`MMFControllerProxyServer-9d`) **[verified that the steps go through MediaClientUtility;
  synchronous IPC per step inferred from the MMF design]**.
- `AddDataSink(audio output)` makes the controller thread create `CMMFDevSound`, which
  connects to `!MMFAudioServer`, starting `MmfAudioServer.exe` on demand **[inferred]**. In the
  log the server starts (`Hwa open … by MmfAudioServer.exe::MmRmServerThread`, line 663), its
  resource-manager thread dies (`KERN-EXEC 0`, line 666, H-dspbridge), the main thread survives
  (`FUN_8040f85a`: `RProcess::Rendezvous(0)` then `CActiveScheduler::Start`). Each client session
  then spawns a DevSound server thread with a synchronous `Rendezvous`+`WaitForRequest`
  (`FUN_8040f490`, lines 272–287) and talks to the dead resource manager; the request never
  completes **[verified server structure; the exact never-completing request inferred]**.
- Consequence chain: controller thread stuck in `AddDataSink` → Startup main thread stuck in the
  synchronous `SendReceive` inside a `RunL` → active scheduler blocked → view-server event
  unanswered for 10 s → `ViewSrv 11`. Confirmation from the log: 7 lines after the panic
  `MmfAudioServer.exe::Main exited (type 0 reason 0)` — the server's normal "last session
  closed" exit, i.e. Startup's DevSound session *was* open and pending when Startup was killed
  **[verified from log ordering]**. The block happens right after ConstructL (states 2–9 are
  waiting on asynchronous PS/Starter events), so state 10 is never reached and the animation
  is never started.
- The only `User::WaitForRequest` in Startup.exe (0x833a8918) is inside the user-welcome-note
  class, not on the audio path **[verified]**.

## 4. Proposed fix

### (a) Byte patch (preferred; same shape as H-keysound-nommf) **[verified bytes]**

`CStartupTone::ConstructL` at 0x833a93c4: `blx CMdaAudioPlayerUtility::NewFilePlayerL`
(`f001 ecaa`, memory `01 f0 aa ec`) → `movs r0,#0 ; nop` (`00 20 c0 46`). The next
instruction `str r0,[r4,#8]` then stores `iTone = NULL`; `Play()` returns `KErrNotReady`,
`FUN_833a7cac` falls back to `KeySounds()->PlaySound(0x3fa)` (already silent), the animation
starts and `AnimationFinished` goes straight to state 11 because `iPlaying` is 0. Both tone
objects (startup and operator) use this single site. No MMF controller thread is created in
Startup.exe any more, so nothing touches `MmfAudioServer`.

Entry for `rom/hybrid/code-patches.txt`:

```
# H-startuptone-nommf: Startup.exe CStartupTone::ConstructL creates a CMdaAudioPlayerUtility for
# the startup tone; without the DSP bridge the MMF open blocks the main thread in a synchronous
# controller call, the view server times out (ViewSrv 11, 10 s) and the welcome animation is never
# shown. Replace "blx NewFilePlayerL" with "movs r0,#0; nop": iTone stays NULL, Play() returns
# KErrNotReady, the key-sound fallback is used (silent) and the animation runs.
833a93c4  01f0aaec  0020c046   # startup.exe CStartupTone::ConstructL
```

Original bytes checked in `build/n95/core-flat.img` at 0x33a93c4: `01f0aaec` (next word
`a0607fb0` = `str r0,[r4,#8]; add sp,#508`).

### (b) Settings-only alternative (no code change)

ROFS2 `private/10202be9/101F8763.txt` (KCRUidStartupConf 0x101F8763): set `0x2 int 0`
(startup-tone volume; `ConstructL` skips the player when the volume is 0) or point `0x3` at a
non-existent file (`TFindFile` fails → player skipped). Caveat: CentralRepository persists the
repository on C: after first use; a stale copy would keep the old values. Setting the active
profile to silent (0x101F8798 key 0x7E000002 = 4) also works but changes phone behaviour.

### (c) Not recommended

Raising the view-server event time-out (`CVwsSessionWrapper::SetServerEventTimeOut`) only delays
the panic; the MMF call never completes without an audio server.

### Expected result after (a)

Welcome animation (4.2 s, skippable by a key) → operator animation skipped (key 0x9/0x7 empty) →
user welcome note → first-boot/RTC time query (`FUN_833a7972`, `CAknTimeQueryDialog`, needs a
key press if the RTC is invalid) → state 16 → Startup exits normally and the idle screen loads.
If a different hang appears later, check whether another process (SysAp/Phone/profile) opens
MMF the same way.

## 5. After the tone patch: why Startup.exe parks in state 2 (no animation yet)

Scope: static analysis of `startup.exe`, `Starter.exe`, `SysAp.exe`, `Phone.exe`, `sysutil.dll`,
`DSClient.dll`, `DosSrv.dll`, `CommonDSY.DLL`, `NokiaDSY.DLL`, `sae.exe` (decompiles in
`build/n95/ghidra/`, disassemblies via `tools/xipdis.py`). Two helper agents did the DOS-event
trace and the Starter resource parse; their key addresses were re-checked where noted.

### 5.1 Global system state (KPSUidStartup 0x101F8766 / key 0x41)

`SysStartup::SetState`/`State()` (sysutil.dll 0x838e4be0 / 0x838e4bf0) are plain
`RProperty::Set/Get(0x101F8766, 0x41)` **[verified]**, so the kernel `SYBPROP` trace shows every
state change. Observed transition graph (writer → value) **[verified unless marked]**:

| value | written by | when |
|---|---|---|
| 0xc9 201 | Starter `FUN_83900b24` | after reading start-up reasons (0x42 = WD2 reason, 0x421 = SW reason 100) |
| 0xca 202 | Starter `FUN_83901c44` (0x83901d36) | right after the splash phase, before the first list item |
| 0xd3 211 | **Startup.exe** phase 3 (0x833a7f54) and `FUN_833a80d4` (0x833a812c) | once its UI-ready gate passes (see 5.2) |
| 0xd5 213 | Phone.exe 0x81706bfa | when Phone's engine is up and `State()==0xd3` |
| 0xd4 212 | **Startup.exe** phase 16 (0x833a72c4) | end of the start-up sequence (after animation, welcome note, time/date) |
| 0xcb 203 | SysAp `FUN_8338a5d2` | on observing 0xd4 (`FUN_8338ece6`) |
| 0xcd/0xce | SysAp | on 0xd3 (RF on / RF off by start-up reason), on 0xd1/0xd2 |
| 0xcf / 0xd1 | NokiaDSY `FUN_808be194` | MTC_STATE_INFO_IND(state 1, action 3) while state ∈ {0xcd,0xce} / (3,3) while 0xce |
| 0xfa / 0xfc | Startup (charging) / Starter, Startup (fatal) | |

Names: 201/202 = the Starter "UI services started / critical apps starting" phases; 211 =
"critical phase OK" (Startup); 213 = "phone ready" (Phone); 212 = "start-up UI finished" (Startup);
203 = SysAp's "self-test OK" response to 212; 0xcd/0xce = normal RF on/off; 0xfa charging; 0xfc
fatal **[names inferred from behaviour; TPSGlobalSystemState enumerators not available]**.

### 5.2 What Startup.exe waits for, state by state **[verified from the state machine FUN_833a7eea]**

| state | gate | who provides it |
|---|---|---|
| 0→2 | entered at the end of ConstructL (0x833a83da) | – |
| 2→3 (`FUN_833a80d4`) | AppUi+0xe0 ∈ {2,3}: **DosServer event id 1** (4-byte TInt; 3 = normal → state 3, 2 = failed/charging → state 4) **and** `State()` ∉ {0xcf (→3 directly), 0xfa (→4), 0xfc (→17)} | DOS self-test result, see 5.3. Shortcut: if `State()==0xcf` at ConstructL (0x833a83c0) the value 3 is injected without DOS (0x833a83c4) |
| 3→5 | immediate: `SetState(0xd3)` | – |
| 5→6 (`FUN_833a8190`) | PS 0x101F8766/0x41 == **0xd5** (handler `CStartupPSObserver` 0x833a9c18; 0xfc → fatal) | Phone.exe 0x81706bca: requires `State()==0xd3` when its init completes plus its own ready flags (+0x144 bits 1,2) |
| 6→7 (`FUN_833a7eb0`) | if FeatMgr feature 88 (SIM support, AppUi+0xf0): KUidSystemCategory 0x101F75B6 / key 0x10005301 != 0 (`FUN_833a997e`) else immediate | sae.exe (System Agent emulator) publishes the legacy KUidSystemCategory keys |
| 7→8 | offline-mode query (`FUN_833a7e28`; feature 7, CenRep 0x101F8763 key 1, DSClient #40/#39 = sub-session 0x66 op 0x196): dialog only if needed; sets 0x101F8766/0x11 = 0x65/0x66; then → 8 immediately | user (only when the query is shown) |
| 8 (`FUN_833a7d56`) | `SecUi.dll` #6 (security/PIN check, synchronous), then `FUN_833a7d12` needs AppUi+0xd4 (= "SIM OK": `(0x100052e9 == 8 or no SIM feature) && 0x100052f0 == 1` in `CStartupPSObserver`, 0x833a9cc4..0x833a9d78) and +0xec == 0 (key 4 == 0x65). If `0x100052e9 ∈ {2..7,9,10,11}` or `0x100052f0 == 2` or `0x100052f4 == 2` it shows the "SIM problem" control (state 5) and waits | sae.exe / Phone (SIM, network status) |
| 9→10 (`FUN_833a7cf2`) | if feature 3 (cover display, AppUi+0xf8): Mediator command 0x65 (`FUN_833a971c`) and wait for `FUN_833a81d6`; else immediate | cover UI (absent on N95: FeatMgr must return 0) |
| 10 | **welcome animation** (section 2) | – |

Current boot: the trace shows only 201, 202 and `Startup sets 0x4 = 101` (its own observer
init) — no 0xd3 — so Startup is parked in state 2 waiting for DOS event 1 **[verified by
elimination; ties in with the log line right after it: `ISCPEER: ch 24 no reply for resource 35 msg 02`]**.

### 5.3 DOS event 1 = DSY self-test result, driven by ISI resource 0x35 / 0x59 **[verified by the helper agent, spot-checked]**

- Startup registers `CDosEventListenerBase::StartListeningL(1, 4, 1)` (DSClient #20 at 0x833a83a0)
  and, unless `State()` is 0xcf/0xfc, sends DSClient **#50 = op 500** on the type-0x67 sub-session
  (0x833a83d0). Startup.exe is the only ROM binary importing #4/#50; sae.exe/SysAp do not listen
  to event 1.
- DosSrv `DosSrv_ord37` (0x838eb72a) accepts op 500 only and calls CommonDSY `FUN_838e833e`
  (DSY request group 0, id 0x19). NokiaDSY component A (`FUN_808bc266` state machine, ctor
  0x808bc16c) answers with a 4-step self-test sequence executed by component B as ISI traffic:
  1. res **0x35 msg 0x02** data `[01]` → expects **0x35/0x03**, byte[10] ∈ {0,10,11} (1/2 = retry, 12 = fail);
  2. res **0x35 msg 0x00** data `[02]` → expects **0x35/0x01**, byte[10] as above;
  3. res **0x35 msg 0x02** data `[04]` → expects **0x35/0x03**;
  4. res **0x59 msg 0x42** (no data) → expects **0x59/0x43** (any payload) within 10 s
     (`CPeriodic` 0x989680 µs, 0x808bc22a), else failure path (value 2 + MTC_STATE_REQ(8)).
  On success `FUN_808bc1ae(obj, 3)` raises DSY event 0x10000001 → CommonDSY bridge
  `FUN_838e8f98` → `DosSrv_ord29` (0x838eafe6) = DOS event id 1 with value 3 → Startup
  `FUN_833a9e60` → `FUN_833a8166` → `FUN_833a80d4` → `SetState(0xd3)` → state 5.
- Our fake modem answers neither 0x35 nor 0x59 (the log's unanswered `35 02`), so the sequence
  stalls at step 1 forever (no timer on steps 1–3).

### 5.4 What Starter waits for after 202 **[verified by the helper agent from Starter_Arm.rsc + code]**

Starter's boot list (resource 3, start-up reasons (0,100), 57 items) sets 0x41 = 202 (type 22),
starts Sae, SysAp, Phone, idle, clknitzmdls, ScreenSaver, queues **wait-for-state 212** (type 9,
0x83901e7a), starts Startup.exe (no monitor) and npaprivlistener, then parks on a type-12 WAIT
item. The only resume is its 0x41 subscriber (`FUN_83901a8a`) seeing **0xd4 = 212**, which
Startup.exe writes at the very end of its own sequence (phase 16, 0x833a72c4). The remaining
~35 items (Watcher, NcnList, BtServer, SatServer, CalenSvr, Autolock, GSServer, …) run only after
that, so the idle screen's full set of servers and the task list (`type 20`, 0x41 re-set to 202 +
`REikAppUiSession::EnableTaskList`) wait for Startup too. Nothing in Starter depends on 203–211;
SysAp drives those (0xd4 → 0xcb, 0xd3 → 0xcd/0xce). Starter never writes 0xd4 itself.
Charging/alarm lists (resources 6/7) wait for 211 instead.

### 5.5 Fix proposals

**(a) Fake-modem replies (preferred; the real mechanism, no code patch).** Add canned replies to
`platform/isi/replies-bb5.txt + devices/<id>/isi-replies.txt` (regenerates `build/devices/<id>/gen/isireplies.h`); the table matches on
(resource, request id) regardless of the request data, which is fine because steps 1 and 3 accept
the same answer:

```
res 35 PN_SELFTEST?           inferred  # NokiaDSY start-up self-test (DOS event 1), 3 requests at boot
res 59 PN_UNKNOWN_59          inferred  # 4th self-test step, 10 s timeout in NokiaDSY
msg 35 02 SELFTEST_RUN_REQ?           inferred
msg 35 00 SELFTEST_STATUS_REQ?        inferred
msg 59 42 SELFTEST_FINAL_REQ?         inferred
reply 35 02 03 00 00   # self-test step 1/3: status 0 = passed (NokiaDSY 0x808bc3e2/0x808bc3a4 accept 0,10,11)
reply 35 00 01 00 00   # self-test step 2: status 0 (0x808bc366)
reply 59 42 43 00 00   # self-test final: any payload accepted (0x808bc362)
```
Expected effect: DOS event 1 = 3 → Startup sets 0x41 = 0xd3 → SysAp sets 0xcd/0xce → Phone
(if its engine comes up on our ETel/NOKIATSY stand-ins) sets 0xd5 → Startup state 6 → … →
state 10 animation. Replies must carry the request's transaction id (the table path does).

**(b) Byte patch alternative (bypass the DOS round trip).** In Startup's ConstructL the
`State()==0xcf` shortcut injects value 3 without DOS (0x833a83c4). Patch the compare at
0x833a83c0 `cmp r0,#0xcf` (halfword 0x28cf, memory bytes `cf 28`) to compare against 0xca, the
value Starter has set by the time Startup runs:
```
833a83c0  cf28  ca28   # startup.exe ConstructL: treat State()==202 like 0xcf -> self-test result 3 injected
```
Original bytes verified in `build/n95/core-flat.img` at 0x33a83c0: `cf28` (`cmp r0,#0xcf`).
Caveat: only valid while 0x41 is 0xca when Startup constructs (true in our boots); the DSY
self-test request is then never sent.

**Next gates after this one (expected, not yet exercised):** 0xd5 from Phone.exe (needs
`State()==0xd3` when Phone's init completes — if Phone finished earlier it leaves with
KErrNotReady at 0x81706c3a and never sets 0xd5; check the log for a Phone exit), then the SIM/phone
status keys 0x100052e9 (== 8) / 0x100052f0 (== 1) under KUidSystemCategory published by sae.exe,
which depend on ETel (NOKIATSY) and DosServer events; with "no SIM" from the fake modem the
"insert SIM" control (state 5) may be shown instead of the animation unless feature 88 is off.

### 5.6 Cross-check against run r47 (`build/runs/r47/report.txt`, after the light-control fix)

- `12.49 s Startup::startup sets 101f8766/4 = 101` is immediately followed by
  `12.489 s ISCPEER: ch 24 no reply for resource 35 msg 02` (serial.log line 87420): that is
  step 1 of the DSY self-test from 5.3, sent from Startup's ConstructL via DSClient #50 → DosServer
  (channel 24 = NokiaDSY) **[verified]**. The 0x59/0x42 request never appears because the DSY
  stalls on step 1.
- `Startup::startup → !Windowserver M:65541` is the normal idle event wait of a parked UI
  application; Phone.exe (thread `Telephone::Telephone`) is alive, so the 0xd5 gate can still be
  satisfied once 0xd3 is written **[verified from the process/thread lists]**.
- The newly started `btmanserver`, `mpxcollectionserver`, `mpxplaybackserver`, `remconserver`
  are not items of Starter's boot list before its WAIT item (5.4); they are launched on demand by
  SysAp/Phone/media clients. Starter itself is still parked on the type-12 WAIT until 0x41 = 212,
  consistent with 0x41 staying at 202.
- The other unanswered ISI requests in r47 (GPDS 0x31, NET `0A`, PHONE_INFO `1B 0B`, `54 0D`,
  `1C 38`) are not on the Startup state-2 path; `1C 38`/`54 0D` and the ETel waits (sae → EtelServer
  40018, CLKNITZMDLS 24015, mpmserver 40022) may matter for the later gates 5→6 (Phone 0xd5)
  and 8→9 (sae SIM/network keys) **[inferred]**.
