# N8 accelerometer: what a stand-in must do to make the UI rotate

Research note (2026-11-10), mock-level only. Firmware: N8 RM-596 011.012 (core ROM), 701/808 Belle
quick check. Evidence (disassembly, raw modules, rule files) is in `build/evidence/accel/` (not in git).
Each claim is marked [verified] (read in firmware code or files) or [inferred].

## 1. Chain from driver to rotated UI

```
accelerometer.pdd + accelerometer.ldd   (kernel; in Nokia's ROM, not in ours: missing_drivers)
  ^ RBusLogicalChannel "accelerometer"
accssy.dll        (sensor SSY, ROM, loaded by sensorserver.exe; channels CXYZChannel,
                   CDoubleTapChannel, CThresholdChannel, CWakeUpChannel)
  ^ sensor server, accelerometer XYZ channel (TSensrvAccelerometerAxisData)
orientationssy.dll (ROM; Symbian Foundation source: oss.FCL.sf.os.devicesrv/sensorservices/orientationssy)
  ^ orientation channel (TSensrvOrientationData)
SensorSourcePlugIn.dll (Context Framework source, uid 0x10282DF0) -> context Sensor:Event.Orientation
  ^ CF rules Private/10282BC4/Rules/autorotateui_*.rul (rofs1)
OrientationActionPlugIn.dll (rofs1): <Orientation Mode='Alternate'/> = landscape, 'Normal' = portrait
```
- [verified] module names and the rule texts (`autorotateui_rotate.rul`: `Event.Orientation =
  DisplayRightUp` -> `Orientation Mode='Alternate'`; `DisplayUp` -> `Mode='Normal'`).
- [verified] `autorotateui_activate.rul` activates the orientation channel (sensor source command
  channelId 0x01) only while all of these hold: `Sensor:Setting.Active = On`,
  `Sensor:Setting.TurningInteraction.AutoRotation = On`, `Device:Display.Status.Light = On`,
  `Device:DeviceLock.Status = EDeviceLockOff`, `System:Keyguard.Status = EKeyguardNotActive`.
  Any of them off deactivates it; `autorotateui_alwaysinportrait.rul` forces `Normal` when either
  sensor setting turns Off.
- [inferred] the two sensor settings come from the Sensor settings cenrep (Settings > Phone > Sensor
  settings, gssensorplugin.dll); default believed On on the N8, not checked. If rotation never
  happens with good data, read those keys first.
- [inferred] `sensordatacompensationdevice.rul` keys on `Device:Flip.Status` only: identity on the N8
  (no flip), so the raw driver axes reach orientationssy unchanged.
- [inferred] how OrientationActionPlugIn applies `Alternate` (akncapserver / window server
  orientation) was not looked at; not needed if the chain above is fed.

## 2. Opening the driver (accssy.dll, N8) [verified]

```
User::LoadLogicalDevice(_L("accelerometer.ldd"))   // error other than KErrNone/KErrAlreadyExists leaves
User::LoadPhysicalDevice(_L("accelerometer.pdd"))  // same; on error FreeLogicalDevice("accelerometer")
RBusLogicalChannel::DoCreate(_L("accelerometer"), TVersion(2,0,0x861), aUnit = 0,
                             aDriver = &_L("accelerometer.pdd"), aInfo = NULL, EOwnerProcess)
```
- LDD object name "accelerometer", PDD name "accelerometer.pdd" (the client names the PDD, so a PDD
  must exist and match). [verified]
- Nokia LDD: `iVersion = TVersion(2,0,0x861)`, `iParseMask = 3` (KDeviceAllowUnit |
  KDeviceAllowPhysicalDevice). [verified] Classes DAccLddDevice, DAccLddChannel, DAccLddRingBuffer.
- Stand-in: same names, parse mask 3, version >= 2.0.0x861 (use TVersion(2,9,0) to cover Belle too,
  [inferred]); a trivial stand-in PDD named "accelerometer.pdd" whose Validate accepts unit 0.

## 3. Channel protocol

### Controls (synchronous, DoControl) — numbers [verified], layouts partly [inferred]

| # | a1 | a2 | use by accssy | stand-in |
|---|----|----|---------------|----------|
| 0 | TDes8* (TBuf8<248>) | – | right after DoCreate; LeaveIfError; 248 bytes copied verbatim into the SSY (offset 8 of the buf) | write 248 zero bytes, return KErrNone. Content NOT decoded [inferred: driver config/calibration block] |
| 1 | TInt* (value 1 seen) | TInt* or NULL | after open (value 1) and via a 2-arg wrapper | accept, KErrNone [inferred: power/mode on] |
| 2 | TInt* id | TDes8* out | get config of id 0 (6-byte TPtr8), 3, 5, 6 | write zeros of the client's max length, KErrNone [inferred: per-channel config (rate/range/threshold)] |
| 3 | TInt* id | TDesC8* in | set config of id 3 / 5 / 6 | accept and store, KErrNone [inferred] |

The id in controls 2/3 is the request number of the channel it configures (3 = XYZ data stream, 5 and
6 = the gesture/threshold channels). [inferred from call sites next to DoCancel(3/5/6)]

### Requests (asynchronous, DoRequest) [verified in the LDD dispatch]

| # | args | Nokia LDD | stand-in |
|---|------|-----------|----------|
| 0 or >6 | – | KErrNotSupported (-5) | same |
| 1 | none | status kept, PDD call | keep pending (never completes) [inferred: hardware event] |
| 2 | (not used by accssy) | status kept, PDD call | keep pending |
| 3 | a1 = TDes8* data buffer | **XYZ sample stream** | see below |
| 4 | (not used) | status kept | keep pending |
| 5 | a1 = TDes8* | double-tap channel [inferred] | keep pending |
| 6 | a1 = TDes8*, a2 = TUint8* index | indexed slot, index read from client must be < 4 else KErrArgument (-6) | keep pending |

Cancel: accssy calls `DoCancel(3)`, `DoCancel(5)`, `DoCancel((idx<<5) | 6)`: the "mask" is the request
number (plus index), not a bit mask. Complete the matching request with KErrCancel. [verified call
sites; meaning inferred]

### Request 3: sample delivery [verified layout of the copy loop, field meaning inferred]
- The LDD gets the client descriptor (Kern::ThreadGetDesInfo), then writes as many **24-byte records**
  as are queued and fit, one after the other, sets the descriptor length, completes KErrNone.
  If the buffer cannot hold one record it completes with -4 (KErrNoMemory). With nothing queued the
  request stays pending until the next sample.
- Record = 6 words: `{ TInt64 timestamp; TInt32 x; TInt32 y; TInt32 z; TInt32 0 }`, i.e. the layout
  of TSensrvAccelerometerAxisData (iTimeStamp TTime = microseconds since 0 AD, iAxisX/Y/Z).
  [inferred: ring element is 5 words copied with the first two swapped (hi/lo of a 64-bit), last word
  zeroed]
- Keep the stream going (complete every 20–50 ms with the current vector, not only on change): the
  orientation SSY confirms a new state with a timer (cenrep keys
  `KOriStateDisplayUpToDisplayRightUpTimerKey` etc.). [inferred from SF source]

## 4. x/y/z for the two views [verified from SF orientationssy source, sign convention inferred]

orientationssy (SsyOrientation.cpp `CalculateSphericalPolarCoordinates` / `CalculateDeviceOrientation`):
zenith = atan(sqrt(x²+y²)/z), azimuth = angle of (x,y) from +x towards +y. With zenith 45..135
(|z| small): azimuth 45..135 -> DisplayUp, ≤45 or ≥315 -> DisplayRightUp, 135..225 -> DisplayLeftUp,
225..315 -> DisplayDown; z dominant positive -> DisplayUpwards. Only ratios matter (scale-free), so any
magnitude inside the declared range works (e.g. G = 1000).

| view | x | y | z | orientation | UI |
|------|---|---|---|-------------|----|
| portrait upright | 0 | +G | 0 | DisplayUp | Normal |
| turned 90° anticlockwise (top of phone to the left) = our "Turn view" | +G | 0 | 0 | DisplayRightUp | Alternate (landscape) |

The vector is the "up" reaction to gravity in device axes (x to the right edge of the display, y to
the top). Avoid exact (0,0,z): it reads as face up/down.

## 5. Belle (701 RM-774, 808 RM-807) [verified, quick check]
- Same files: accelerometer.ldd/.pdd (LDD 13.4 KB vs 9.7 KB on the N8), accssy.dll, orientationssy.dll.
- accssy opens the same names ("accelerometer", "accelerometer.pdd"), major version 2 (minor not read).
- Same controls 0, 1, 2, 3 and requests 1, 3, 5, 6; **new request 7** (DoRequest(7, status, a1, a2),
  DoCancel(7)) [verified]. Stand-in: request 7 stays pending, KErrCancel on cancel. A stand-in that
  answers 1..7 serves all three phones.

## 6. Stand-in sketch (src/standins/accelldd/, style of lightldd) [inferred design]

```cpp
// LDD: DLogicalDevice name "accelerometer", iVersion TVersion(2,9,0), iParseMask = 3.
// PDD: DPhysicalDevice name "accelerometer.pdd", Validate(): unit 0 ok, version major 2.
TInt DAccChannel::DoControl(TInt aFn, TAny* a1, TAny* a2) {
    switch (aFn) {
    case 0: return WriteZeros(a1);           // 248-byte block
    case 1: return KErrNone;                 // power/mode
    case 2: return WriteZeros(a2);           // get cfg of id *(TInt*)a1 (zeros, client max length)
    case 3: return KErrNone;                 // set cfg: accept
    default: return KErrNotSupported;
    }
}
TInt DAccChannel::DoRequest(TInt aReq, TRequestStatus* aS, TAny* a1, TAny* a2) {
    switch (aReq) {
    case 3: iDataStatus = aS; iDataDes = a1; if (iHaveSample) CompleteData(); return KErrNone;
    case 1: case 2: case 4: case 5: case 6: case 7: Park(aReq, aS); return KErrNone;
    default: return KErrNotSupported;
    }
}
void DAccChannel::DoCancel(TUint aReqNo) { CompleteParked(aReqNo & 0x1f, KErrCancel); }
// Timer DFC every ~40 ms: read x,y,z from a host register (board device written by the
// "Turn view" action: portrait (0,+G,0), landscape (+G,0,0)), build
// {Kern::SystemTime() as TInt64, x, y, z, 0}, Kern::ThreadDesWrite into iDataDes, complete KErrNone.
```

## Open points (not decoded, needed only if accssy rejects zeros)
- Content of the 248-byte block (control 0) and the per-id config blocks (control 2/3): read the
  Nokia LDD control dispatch (`accelerometer.ldd` at 0x801e22e4 / 0x801e25fc / 0x801e279c, ARM) or
  watch what accssy does with them after a boot with the stand-in (sensorserver panics/leaves).
- Whether the sensor-settings cenrep defaults are On.
- Why our kernel cannot use Nokia's own accelerometer.ldd: if only the PDD is hardware-bound, keeping
  Nokia's LDD and writing just a PDD stand-in would avoid all the guessing above, but the LDD<->PDD
  vtable interface was not read.

## 7. What made it work (2026-11-11, verified on the N8 011.012)

The driver information block (control 0, 248 bytes) is the host's: QEMU `syborg-sensor` INFO registers,
filled from the profile's `[board.accelerometer] info` (`"00=1d3b0203,08=020028006400,14=0200d0070800401f0800,2a=0d00,68=01000000,7e=05"`).
accssy checks it against its own repository 20000585 [V, accssy.dll N8]:

| block offset | meaning | repository / check | failure without it |
|---|---|---|---|
| +0x00 u32 | chip id | one of keys 10/4010/8010 (0x03023B1D "LIS302DL in address 1D", 0x03023B1C, 0x03404A1D) | Leave -1 at 0x80b72d22: no channels |
| +0x08 u16 count, +0x0a u16[] | data rates | every rate of keys 1401, 1412, ... (40, 100 Hz) | Leave -20 at 0x80b7447c (channel open) |
| +0x14 u16 count, +0x16 (u16 range, u16 scale)[] | measure ranges | every pair of keys 1001/1002, 1013/1014 (2000 mg/8, 8000 mg/8) | Leave -20 at 0x80b743cc (channel open) |
| +0x2a u16 bits | channels: 0 XYZ, 2 second, 3 table | | Leave -2 at 0x80b729c2 (no XYZ) |
| +0x68 u32 count, 28-byte entries; entry 0 byte +0x7e bits 0/2 | third / fourth channel | | |

All four channels must register: the sensor server rejects a registration list that lacks any static channel
of the plug-in's ECom resource (`CSensrvSsyMediator::RegisterChannelsL`). The channel infos then keep data item
size 0; `CSensrvChannelBuffer::ConstructL` leaves with `KErrArgument`, which `CSensrvPluginProxy::DispatchMessage`
reports as `KErrNotFound` (its `TRAPD` shadows `err`). orientationssy's open of the XYZ channel failed with -1 that way.

Request 1 is a synchronous start: accssy calls `User::WaitForRequest` (0x80b730c2) and treats non-zero as
failure, so the stand-in completes it at once with `KErrNone`. Then control 3 (config) and request 3 (samples)
follow; the stream runs at 25 Hz while the context framework's rule keeps the orientation channel active.

How it was found: a context framework probe and a channel probe (`build/evidence/accel-review/`: `cfprobe.cpp`,
`chprobe.cpp`, run with `sydb rerun`) and hardware breakpoints on `User::Leave` / `User::LeaveIfError` with the
caller in accssy (0x80b72478..0x80b77b98) or sensorserver (0x82388b18..0x823924e0).

## 8. Belle (Delight) open path (2026-11-11, static read of Delight's accssy.dll 0x80b1a0a8..0x80b20a14)

Evidence: `tools/xipdis.py` of accssy.dll (scratchpad `accssy-dl.dis`) and sensorserver.exe (0x81dc7ae8); SF
sources for names. [V] = read in the code, [I] = inferred.

### What is opened: accssy's own orientation channel, not XYZ

- Belle's accssy has three channel slots (control object +0x11c/+0x120/+0x124, their channel ids at
  +0x128..), each its own class [V, open switch 0x80b1ad22]:

| slot | type id | class (ctor, size) | driver traffic |
|---|---|---|---|
| 0 | 0x1020507E XYZ (24-byte items) | 0x80b1c1a0, 0x148 | ctl 2 {3} in ctor, ctl 3 + **request 3** stream, ctl 1 {a,b} |
| 1 | 0x1020507F wake-up | 0x80b1cc7c, 0xa8 | request 5 / 6, table config ctl 2 {6} (10 bytes) |
| 2 | **0x10205088 orientation** (TSensrvOrientationData, 16-byte items) | 0x80b1e750, 0x128, CActive, vtable 0x80b20440 | ctl 2 {3} in ctor, **request 7** stream |

  [V: literal 0x10205088 in slot-2 code (0x80b1e9a4, 0x80b1f110, 0x80b1f548), 16-byte items written at
  0x80b1f330, orientation decision 0x80b1f192 with the 45/135/225/315 degree bands of SF orientationssy,
  cenrep 0x10282F0F = KCRUidOrientationSsySettings opened in its ConstructL 0x80b1ed38.]
- So on Belle the context framework's sensor source opens the orientation channel **from accssy** (Nokia
  moved the orientation state machine into accssy; the chip/LDD delivers the angles). The measured
  "ctl 2 {id 3} -> request 1 -> request 7" is slot 2's constructor (0x80b1e7c8, 12-byte TPtr8 at this+0x7c,
  identical to slot 0's at 0x80b1c232), the driver start, and the orientation stream. ctl 3 / request 3 are
  slot 0's and are never needed for rotation on Belle. The registration list's second channel is this one
  ("0x10205081 size 16" in the earlier note is most likely 0x10205088 [I]).

### Open sequence and the availability flag (answers questions 1 and 2)

1. Control open 0x80b1ac46: channel id -> slot, `AllocZL`, ctor (ctl 2 {3}), vfunc+0x18 ConstructL
   (0x80b1ed15: CPeriodic, CRepository 0x10282F0F, settings 0x80b1e8e8), then if the driver is not started
   (`[drv+0x114] == 0`) 0x80b1abd2 = **request 1 + User::WaitForRequest, LeaveIfError**; it sets started = 1 and
   **clears [chan+32] and [chan+36] of all three slots**; then ChannelOpened is posted through a
   self-completing AO (0x80b1f62c). [V]
2. `[chan+32]` = current availability, `[chan+36]` = availability as set. GetPropertyL guard 0x80b1b336: while
   `[chan+32] == 0` only KSensrvPropIdPower (3) and KSensrvPropIdAvailability (4) may be read, anything else
   leaves KErrNotReady. SetPropertyL 0x80b1b91a (reached through the control): id 2 DataRate -> u16 at
   drv+0x138; id 3 Power -> value != 0 and not started: request 1; value 0 and started: 0x80b1aae2 =
   **ctl 1 {1} = stop** (started = 0, all availabilities cleared); id 4 Availability -> `[+32] = [+36] = value`;
   id 5 MeasureRange -> u16 at drv+0x134. Slot 2 overrides it (0x80b1ef72): ids 2, 5, 20 accepted, 4 ->
   `[+32] = value`, others KErrNotSupported. [V]
3. 0x80b1b850 (vfunc+0x38 of every channel class) is "another channel's activity changed(mask)":
   `[+32] = [+36]`, cleared again when a conflicting function is active (`[+0x4a] & (mask & ~[+0x48])`) or
   when this channel is idle (`[+0x38] == 0`) and its own function bit (`[+0x48]`, bit 0 for slots 0 and 2)
   is in use. It is called for the *other* slots from 0x80b1af0e (start/stop of a channel). It is not what
   makes a channel available. [V]
4. The sensor server itself sets the availability: SF `CSensrvChannel::HandleDependencyOnOpen` reads
   property 4 (allowed while inactive), gets False, asks `CheckPropertyDependenciesL` (accssy 0x80b1b8c6: no
   case for id 4 -> no affected channels) and calls `SetChannelAvailability(True)` -> SetPropertyL(4, 1) ->
   `[+32] = [+36] = 1`. [V in SF source; Belle's sensorserver builds property id 4 at 0x81dc881a and
   0x81dc8a56, same logic [I]].
5. The KErrNotReady seen at 0x80b1b38e is **harmless**: Belle's `CSensrvChannel::ChannelOpened`
   (0x81dc8eac) TRAPs `GetPropertyL(KSensrvPropIdDataRate)` (0x81dc8ef2..0x81dc8f1e) and on any error takes
   0x81dc8fec: default max buffering count 0x40, state = open, transaction completed. [V]
6. `StartChannelDataL` (base 0x80b1b75c): KErrInUse if `[+0x38]`, **KErrNotReady if `[+32] == 0`**, then
   `[+0x38] = 1`, activity(1), buffer/count, vfunc+0x2c (slot 2: 0x80b1f0da -> **DoRequest 7** unless the AO is
   active). The request 7 we see therefore proves that open, availability and start all succeeded on the
   SSY side; nothing in the open path has to change. [V]

So: our ctl 0 / ctl 2 / request 1 answers are accepted; the flag question was a red herring. ctl 2 {0}
(6 bytes, driver level, written back with ctl 3 {0} from 0x80b1b7fe) and ctl 2 {3} (12 bytes, zeroed
as {u8 @0, u16 @2, u8 @4, u32 @8} by 0x80b1f9f6, i.e. {enable, rate, range, ?} [I]) may stay zeros [I: the
slot-2 copy at this+0x7c is only traced]. drv+0x48/+0x4a (slot 0's `[+0x4a]` conflict mask comes from
drv+0x48) are driver-object fields, zero is fine [I].

### Request 7 (answer to question 3) [V unless marked]

`DoRequest(7, iStatus, a1 = &this+0x8c, a2 = &this+0x90)` from the orientation channel (0x80b1fa2c); the two
words are TInt **angles in degrees: a1 = zenith (0 = face up .. 180 = face down), a2 = azimuth of (x, y) from +x
towards +y (0..359)**, exactly the spherical coordinates SF orientationssy computes from the XYZ vector
[V compares; the LDD computing them from the chip is I]. RunL 0x80b1ed90:
- KErrCancel: nothing. KErrTimedOut (-33): re-issue. Other errors: `BufferFilled(id, err, ..)` and
  StopChannelDataL (the channel stops itself).
- KErrNone: vfunc+0x4c = 0x80b1f466 -> if the server buffer has room, 0x80b1f2aa:
  - o = 0x80b1f192(): zenith < 45-h -> 5 DisplayUpwards; zenith > 135+h -> 6 DisplayDownwards; zenith in
    the band: azimuth <= 45-h or >= 315+h -> 4 DisplayRightUp, 45..135 -> 1 DisplayUp, 135..225 -> 3
    DisplayLeftUp, 225..315 -> 2 DisplayDown (h = per-state hysteresis from cenrep, 0 by default).
  - no orientation yet (`[+0xa0] == 0`): deliver {HomeTime, o} (0x80b1e5e2 picks the state's settings entry),
    `BufferFilled` when the buffer is full (0x80b1be28; a 0 max count stops the channel), re-issue 7.
  - o differs from `[+0xa0]` and the vote timer is idle: CPeriodic with the state transition timer from cenrep
    0x10282F0F (0x80b1e588, >= 1 ms), re-issue 7. During the window **every completion** appends o to a vote
    array (0x80b1fad8); the callback 0x80b1e650 takes the share of the last value, and if it is >= the
    constant at 0x80b1e994 (0x3FEB333333333333 = 0.85) and differs, commits: `[+0xa0] = o`, deliver, timers.
    **No sample in the window = change dropped** (0x80b1e73c resets the array).
  - unchanged o: just re-issue 7.
- Hence request 7 is a **stream**: it must complete again and again with the current angles (the LDD's own
  -33 time-out handling shows it completes periodically even without change [I]). It must never be completed
  synchronously inside DoRequest (RunL re-issues at once -> busy loop).

### What our stand-in does wrong (answer to question 4) and the fix

Today the first request 7 after a request 1 is completed at once with {0, 0} and later ones stay pending:
zenith 0 -> orientation 5 (DisplayUpwards) is delivered once, the next request 7 never completes, so the
context framework never sees DisplayUp / DisplayRightUp. The "cancel 7 + ctl 1 {1}" ~20 ms later is
CloseChannel (0x80b1ae24: StopChannelDataL at 0x80b1aeaa -> CActive::Cancel -> DoCancel(7), delete, no channel
left -> 0x80b1aae2 stop): the client closed the channel after that item [I: the client side was not read;
with correct angles the first item is DisplayUp and the real phone keeps the channel open].

Proposed stand-in behaviour (src/standins/accelerometer/accelerometer.cpp), no profile change:
- Request 7: park it like request 3 (`iStatus[7]`, remember a1/a2) and run the same KSampleMs (40 ms) timer;
  on every tick with request 7 pending: read the host vector (x, y, z), compute
  `zenith = atan2(sqrt(x*x + y*y), z)` and `azimuth = atan2(y, x)` in whole degrees (azimuth normalised to
  0..359; integer approximation is enough, only the 45/135/225/315 bands matter), `Kern::ThreadRawWrite` a1
  <- zenith, a2 <- azimuth (4 bytes each), `Kern::RequestComplete(KErrNone)`. Drop `iReq7Ready` and the
  immediate completion. Portrait (0, +G, 0) -> (90, 90) = DisplayUp; the "Turn view" vector (+G, 0, 0) ->
  (90, 0) = DisplayRightUp; (0, 0, +G) -> (0, any) = DisplayUpwards.
- DoCancel(7): KErrCancel (as now). Request 1: synchronous KErrNone (as now). ctl 1 {1} (stop) and ctl 1
  {a, b} (slot 0's mode, 0x80b1fa3c): accept (as now). ctl 0 / ctl 2 / ctl 3: unchanged.
- Keep request 3 as it is for the XYZ channel (slot 0) in case a client opens it (orientationssy, if it is
  also registered, or a game).
- Change rate at 25 Hz gives ~12 equal votes in a 500 ms transition window, well above 0.85.
