# N8 ISA Access (isaaccessldd.ldd, "IADDriver"): what a stand-in must answer

Firmware 011.012, core ROM `build/roms/nokia-n8-rm596/<fw>/core-flat.img`. Disassembly:
`python3 tools/xipdis.py build/roms/nokia-n8-rm596/<fw>/core-flat.img build/roms/nokia-n8-rm596/<fw>/core.json iscapi.dll`.
Tags: **[V]** seen in code, **[I]** inferred. Proposed stand-in: `src/standins/isaaccessldd/PROPOSAL.cpp`
(not built). The ISI content is the same as on ISC. Only the channel plumbing changes, so the stand-in is
`iscpeer.inl` built a third time with an `ISC_ISA_ACCESS` control table.

## 1. Who implements the "IAD user API"

- The user-side library is still **`Sys/Bin/iscapi.dll`** (code `0x804e3d98`, 34 exports). It keeps the
  N95 `RIscApi` ordinals and object layout (`RBusLogicalChannel` at +0, four `TPtr8` at +0x0c..+0x30,
  `TUint16 iChannel` at +0x40, 0xFFFF = not open), but it now talks to the ISA Access LDD. Its literals
  (`0x804e4cd4`) are `"IADUserApiPanic"` (+0x00), `"IADDriver"` (+0x24) and `"isaaccessldd.ldd"`
  (+0x3c). **[V]**
- Clients in the core ROM (modules whose import tables hold iscapi.dll export addresses) **[V]**:
  nokiatsy.dll (the TSY under c32exe's CTelServer), einfoserver.exe, Aamm.dll, dataport.csy, bcatoisc.dll,
  saiscservice.dll and LightSAIscService.DLL (SA server), poweradaptation.dll, hapticsadaptation.dll,
  radioadaptationserver.exe, modematcontroller.exe, usbpnserver.exe, ObcManagerId.exe, sospmmhandler.exe,
  nlacommon.dll, locationconfigurationcontroller.exe, wlanhwinit.dll, H4plushciv2_initialisor.dll.
  mc_isiserver.exe lives in ROFS (its imports are compressed there). It panics the same way, so it uses
  the same DLL. **[I]**
- Kernel-side isaaccessextension.dll, isimessagekernel.dll, SecLdd.LDD and securityKext.dll are kernel
  clients of the extension. The user API does not need them. **[V strings / I role]**

## 2. Opening a channel

`#21 Open(TUint16 aChannel, TRequestStatus& aStatus, TOwnerType aType, const TDesC8* aResources)`
(`0x804e400e`) **[V]**:
1. `User::LoadLogicalDevice(_L("isaaccessldd.ldd"))`. If the result is neither `KErrNone` nor
   `KErrAlreadyExists`, it raises **panic `IADUserApiPanic 5`**. This is today's boot killer: the file is
   in `[kernel].missing_drivers`.
2. If `iChannel != 0xFFFF`, it completes `aStatus` with `KErrInUse` (-14).
3. If `aChannel >= 64`, it raises panic 1.
4. `DoCreate(_L("IADDriver"), TVersion(0,0,0), KNullUnit (-1), NULL, &TBuf8<1>{aChannel}, aType)`. This
   gives **one logical device for all channels, with the channel in the info descriptor**, not the unit.
   The kernel passes `aInfo` to `DLogicalChannelBase::DoCreate` as a **user pointer**
   (`kernel/eka/kernel/device.cpp:673`), so read it with `Kern::ThreadDesRead`. If `DoCreate` fails, the
   status is completed with that error.
5. It runs `DoControl(0, &{TRequestStatus* local, TUint16* &iChannel, const TDesC8* aResources})`, then
   `User::WaitForRequest(local)`. This means **open is synchronous**: the kernel must complete `local`
   before it returns or soon after. If the result is `KErrAlreadyExists`, the client closes the handle.
   `aStatus` is then completed with the result.
6. Nothing on the user side writes `iChannel` after the open, yet every later call panics 2 when it is
   still 0xFFFF, and Close tests it `< 64`. **So the kernel writes the channel number (TUint16) through
   the pointer in word 1.** [V that the user side doesn't write it / I that the kernel does]

`#34` (`0x804e418a`, used by bcatoisc.dll) is a second, synchronous open. It does the same
LoadLogicalDevice and DoCreate, then `DoControl(29, &{TUint16* &iChannel})` and checks the return value
(non-zero closes the handle). **[V]**

There is one channel object per `RIscApi`. Several clients may open different channel numbers on the same
device. **[V]**

## 3. Control table

Everything goes through `DoControl(n, a1)`, which reaches the kernel as `Request(n, a1, 0)`. `a1` points at
a stack struct of 32-bit words. Cancels go through `DoCancel(n)`, which reaches the kernel as
`Request(KMaxTInt, (TAny*)n, 0)`. **n is the number of the request to cancel, not a bit mask.**
`DoRequest` is never used. Every call except Open and #19/#12 first checks that `iChannel != 0xFFFF`
(panic 2). All rows are **[V]** except the "kernel must" column where marked.

| Export | DoControl | a1 -> | Kernel must |
|---|---|---|---|
| #21 Open | **0** | `{TRequestStatus*, TUint16* iChannel, const TDesC8* resources}` | write channel (u16) to word 1 [I]; complete status `KErrNone` |
| #34 Open (sync) | **29** | `{TUint16* iChannel}` | write channel; return 0 |
| #24 Close(TRequestStatus&) | **1** | `{TRequestStatus*}` | complete now (the handle is closed right after); return 0, otherwise panic 4 |
| #25 Close() (also dtor #31/#32) | **12** | none | return 0, otherwise panic 4 |
| #33 close (of a #34 channel) | **30** | none | return 0, otherwise panic 30 |
| #23 Send(const TDesC8&) -> TInt | **13** | `{const TDesC8* msg}` | answer the ISI request; return 0 |
| #22 Send(TRequestStatus&, const TDesC8&) | **2** | `{TRequestStatus*, const TDesC8*}` | send; complete `KErrNone` |
| #28 DataSend(const TDesC8&) | **14** | `{const TDesC8*}` | return 0 (drop) |
| #27 DataSend(TRequestStatus&, const TDesC8&) | **4** | `{TRequestStatus*, const TDesC8*}` | complete `KErrNone` |
| #26 Receive(TRequestStatus&, TDes8& buf, TUint16& needed) | **3** | `{TRequestStatus*, TDes8* buf, TUint16* needed}` | as ISC ctl 4: write the whole frame, complete `KErrNone`. If it is longer than `buf.MaxLength()`, write the length to `*needed` **raw (2 bytes, not a TPtr8 as on the N95)**, complete `KErrOverflow` and keep the frame. One Receive per channel. |
| #3 DataReceive | **5** | same layout | park |
| #17 NotifyConnectionStatus(TRequestStatus&, TInt&) | **6** | `{TRequestStatus*, TInt*}` (raw pointer) | park (link never changes) |
| #15 NotifyFlowControlStatus(TRequestStatus&, TInt&) | **7** | `{TRequestStatus*, TInt*}` | park |
| #6 async custom function (fn 9/10/12) | **8/9/10** | `{TRequestStatus*, arg}` | park [I]; other fn: panic 3 |
| #7 CustomFunction (fn 23/24/25) | **19/20/21** | `{arg}` | **19 = subscribe**: arg = `TDesC8*` of (resource, msg id) byte pairs, as ISC ctl 23. Return 0. Other fn: panic 3. |
| #10 MaximumDataSize() | **18** | none | return 4096 (dataport.csy) |
| #11 | **15** | none | return 0 [I: ConnectionStatus, 0 = OK] |
| #14 | **16** | none | return 0 [I: FlowControlStatus, 0 = off] |
| #4 | **22** | none | return 0 [I: ResetBuffers] |
| #9 GetChannelInfo(TUint16 ch, TDes8& out) | — | user side only | writes `{ch >> 8, ch & 0xff}` and returns 0, or `KErrArgument` if `MaxLength < 2`. Same value as our ISC ctl 21 answer. |
| #19 (Initialize(TRequestStatus&) on the N95) | — | user side only | completes `KErrNone` |
| #12 | — | user side only | no-op |
| #5 ReceiveCancel | cancel **3** | | complete the Receive `KErrCancel` |
| #13 DataReceiveCancel | cancel **5** | | complete `KErrCancel` |
| #20 NotifyConnectionStatusCancel | cancel **6** | | complete `KErrCancel` |
| #18 NotifyFlowControlStatusCancel | cancel **7** | | complete `KErrCancel` |
| #16 custom cancel (fn 38/39/40) | cancel **8/9/10** | | complete `KErrCancel` |
| #2 / #8 | cancel **2** / **4** | | nothing pending (sends complete at once) |
| #1 | cancel **0**, then Close() (ctl 12) | | cancel everything [I] |

Panics of `IADUserApiPanic` **[V]**:

| Panic | Meaning |
|---|---|
| 1 | channel >= 64 |
| 2 | call on a channel that is not open (`iChannel == 0xFFFF`) |
| 3 | bad custom function number |
| 4 | ctl 12 or 1 (close) returned non-zero |
| **5** | `User::LoadLogicalDevice("isaaccessldd.ldd")` failed (not 0 / -11) |
| 30 | ctl 30 returned non-zero |

## 4. Message framing

The frames are the same as on ISC: a 10-byte header with media at [0], receiver device at [1], sender
device at [2], **resource at [3]**, **length = total - 6, little-endian at [4..5]**, receiver object at
[6], sender object at [7], **tid at [8]** and **msg id at [9]**. The N8 isimessage.dll `Complete` routine
(`0x805e3dd2..0x805e3e3e`) writes `(len - 6) & 0xff` to [4] and `>> 8` to [5]. **[V]** iscapi.dll passes
the descriptors through untouched. **[V]** The client's object id is `{0, channel}` from #9, the same as
our ISC answer, so `Reply()`'s receiver object = channel still holds. **[I]**

## 5. Stand-in plan

`src/standins/isaaccessldd/isaaccessldd.cpp` sets `ISC_LDD_NAME "IADDriver"` and `ISC_ISA_ACCESS`, then
includes `iscpeer.inl`. Under `ISC_ISA_ACCESS`, `iscpeer.inl` changes as follows (code in PROPOSAL.cpp):
- the factory uses `iParseMask = KDeviceAllowInfo` and 64 channel slots;
- DoCreate takes the channel from `aInfo[0]`;
- `Request(KMaxTInt, n)` dispatches to `CancelIsa(n)`;
- `ControlIsa()` maps the table above onto the existing ISC handlers (0->1 plus the iChannel write, 13->17,
  3->4, 6->7, 7->8, 18->22, 19->23, 1->14);
- `TryDeliver` writes `needed` raw.

In the profile, remove `isaaccessldd.ldd` from `missing_drivers` and add a `[[standins]] name =
"isaaccessldd"` entry. No heavy emulation is needed.
