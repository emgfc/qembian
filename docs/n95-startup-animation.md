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
