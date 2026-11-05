# N95 8GB APE↔modem messaging at boot: IscApi channel interface and ISI protocol

Specification for a *fake modem peer* inside the kernel-side `NokiaISCDriver` stand-in
(`src/n95compat/stubs/nokiaiscdriver.cpp`). Everything below was derived from the RM-320 core
ROM (`build/n95/core-flat.img`, base `0x80000000`) with `tools/xipdis.py` and
`tools/ghidra/decompile.sh` (outputs in `build/n95/ghidra/*.c`), from ROFS1, and from public
ISI references (oFono `drivers/isimodem/*.h`, `plugins/n900.c`; Linux `phonet.h`).

Legend: **[verified]** read from the N95 binaries (address given); **[ref]** from a public
reference (oFono/Linux, names and values only); **[inferred]** reasoned, not measured.

## 1. Summary

- **What blocks today.** `Starter.exe` calls `SysStartup::GetStartupReasons()`
  (`sysutil.dll` `0x838e4adc`) in its ConstructL (`FUN_83900b24` at `0x83900b24`, from
  `FUN_83900cd4`). That goes through `DSClient.dll::RDosServer::Connect` (`DSClient_ord3`
  `0x838ed76c`), which starts `DosServer.exe` and **waits on the global semaphore
  `DosServerStartupSem`** (`DSClient_ord48` `0x838ed6a8`). `DosSrv.dll` signals the semaphore only
  after loading the DSY and `CServer2::StartL` (`FUN_838ea920` → `FUN_838ea976`, `0x838ea976`).
  Loading `NokiaDSY.dll` opens ISC channel 24 and blocks in `User::WaitForRequest` on
  **IscApi control 1 (open channel)** (`FUN_808baa8c` `0x808baa8c`), which our stand-in leaves
  pending. So the whole chain Starter → sysutil → DSClient → DosServer → NokiaDSY → ISC ctl 1 is
  stuck on one missing completion. **[verified]**
- **Minimal fix:** complete control 1 (and 14) with `KErrNone`, answer control 21 with two bytes,
  accept controls 17 and 23, and keep a per-channel receive queue for control 4. With that,
  DosServer starts, Starter proceeds to its start list (next non-modem blockers: AknCapServer /
  EikSrv, then `SysAp.exe`).
- **ISI framing** (§3): 8-byte Phonet header `media, rdev, sdev, resource, len_lo, len_hi, robj,
  sobj`, then `transaction id, message id, data…`. The length is **little-endian** on this
  platform and equals total length − 6. User code fills only `[3]`, `[8]`, `[9]`, `[10..]`
  (receiver/sender fields are zero or garbage); the kernel side is expected to address the
  message. Received messages must carry the full 8-byte header. **[verified]**
- **Boot conversation** (§5): at DSY construction the only ISI messages are a subscription list
  (control 23: `09EF 09ED 0810 15C0`), one `SIM` request `0xE4` and one request to resource
  `0x1C` (`0x2D`); nothing waits for a reply. Later, `NOKIATSY.DLL` (loaded by ETel for
  `SysAp.exe`/`Phone.exe`) sends `MTC_STATE_QUERY_REQ` and expects `MTC_STATE_QUERY_RESP`
  and `MTC_STATE_INFO_IND`; the DSY turns `MTC_STATE_INFO_IND(NORMAL, START)` into the
  global system state "normal RF on". §6 gives the byte strings.

## 2. IscApi.dll channel interface

`Sys/Bin/IscApi.dll` (code `0x8084bae8`, 32 exports, 0xbb8 bytes; disassembly
`build/n95/iscapi.dis`, decompile `build/n95/ghidra/IscApi.dll.c`). `RIscApi` is an
`RBusLogicalChannel` (handle at +0) with four `TPtr8` members (+0x0c, +0x18, +0x24, +0x30), a
`TInt iOpen` at +0x3c and a `TUint16 iChannel` at +0x40 (0x500 = none). Object size 0x44.
**[verified, ctor at `0x8084bb58`]**

### 2.1 Devices and units

| Export | Address | What it does |
|---|---|---|
| #19 `Initialize(TRequestStatus&)` | `0x8084bc7e` | `User::LoadLogicalDevice` of `xbus.ldd`, `iscisimultiplexer.ldd`, `nokiaiscdriver.ldd`, `nokiaiscdriverextension.ldd` (panics `NokiaISCApi 1..4` on error other than `KErrAlreadyExists`); `DoCreate(_L("NokiaISCDriver"), TVersion(0,0,0), unit 0, NULL, NULL, EOwnerThread)` (`0x8084bd22`); sets `iChannel=0, iOpen=1`; status = `KRequestPending`; **`DoControl(0, &{TRequestStatus*})`** (`0x8084bd4e`). The kernel completes the status when the modem link is up. |
| #21 `Open(TUint16 aChannel, TRequestStatus&, TOwnerType, const TDesC8* aInfo)` | `0x8084bd78` | channel 1..31 → `DoCreate("NokiaISCDriver", unit = channel, info = TBuf8<1>{channel})`; channel 32..63 → `DoCreate("NokiaISCDriverExtension", unit = channel − 32, same info)`; else panic `NokiaISCApi 1`. On `DoCreate` error the status is completed with that error (no kernel call). Then `iChannel=channel, iOpen=1`, status pending, **`DoControl(1, &{TRequestStatus*, TUint32 channel, const TDesC8* aInfo})`** (`0x8084be5e`). `aInfo` is the 5th argument of the export; callers pass `0` or a pointer to a `TBuf8` listing ISI resource ids served by this channel (EInfoServer: `{0x62}`, NOKIATSY: `{0x5A}`). |
| #25 `Close()` | `0x8084bb94` | if `iChannel < 0x40 && iOpen`: `DoControl(15)`; then `RHandleBase::Close()`. |
| #24 `Close(TRequestStatus&)` | `0x8084be90` | if `iChannel < 0x40 && iOpen`: `DoControl(14, &{TRequestStatus*})` else completes `KErrNotReady`; `iOpen=0`. The kernel must complete the status. |
| #31/#32 dtor | `0x8084bbc2` | `Close()` if open. |
| #12 | `0x8084bd60` | `DoControl(29)` only when `iChannel == 0` (init channel). Not used by boot clients. |

Observed in our boot: `EStart` opens unit 0 and does ctl 0 (completed) and later ctl 15 (destructor);
`EInfoServer` opens unit 11 with `aInfo→{0x62}`; `DosServer` opens unit 24 with `aInfo=0`
(serial log lines 45320, 47900, 122205). **[verified]**

### 2.2 Data path (all take `this` first; `a1` of `DoControl` points at a stack struct)

| Export | DoControl | `a1 →` struct | Meaning / kernel obligation |
|---|---|---|---|
| #23 `Send(const TDesC8&)` → `TInt` | **17** | `{const TDesC8* aMsg}` | Synchronous send of one ISI message. Return `KErrNone` (DSY `User::LeaveIfError`s it at construction, `0x808ba6fc`; EStart panics on non-zero). `0x8084bede` |
| #26 `Receive(TRequestStatus&, TDes8& aBuf, TUint16& aNeeded)` | **4** | `{TRequestStatus*, TDes8* aBuf, TPtr8* aNeededPtr}` where `aNeededPtr` is `iPtr0` (+0x0c) set to `TPtr8((TUint8*)&aNeeded, 2, 2)` | Deliver the next queued message for this channel: write the whole message (8-byte header included) into `*aBuf` (`Kern::ThreadDesWrite`, length = message size) and complete `KErrNone`. If the message is longer than `aBuf.MaxLength()`: write the total message length as a 16-bit value through the `TPtr8` (2 bytes, native little-endian) and complete **`KErrOverflow` (−4)**, keeping the message queued; the client reallocates and calls `Receive` again (`FUN_808ba8fe` `0x808ba8fe`). Only one `Receive` is outstanding per channel. `0x8084bfda` |
| #5 `ReceiveCancel()` | 33 | none | Complete the pending Receive with `KErrCancel` (used in destructors/`DoCancel`: EInfoServer `0x808a3ebe`, mc_isiserver `0x80a02fba`). |
| #17 `NotifyConnectionStatus(TRequestStatus&, TInt& aStatus)` | **7** | `{TRequestStatus*, TPtr8* (4 bytes over aStatus)}` | Completes when the modem link status changes. NokiaDSY posts it at construction (`FUN_808babc2` `0x808babc2`) and on completion raises a DOS event (`0x808bac22`). Leave pending. |
| #20 `NotifyConnectionStatusCancel()` | 35 | none | Cancel the above (`DoCancel`, `0x808bac4a`). |
| #9 `GetChannelInfo(TUint16 aChannel, TDes8& aOut)` → `TInt` | **21** | `{TUint32 channel, TDes8* aOut}` | Must write **2 bytes** into `aOut` (a `TBuf8<2>`) and return 0. Callers read them as a big-endian 16-bit value: NokiaDSY stores it (`0x808baa8c`, +0x76); mc_isiserver uses the low byte as its Phonet *object id* and `0x6C` as the device id in a name-service registration (`FUN_80a026d6` `0x80a026d6`). Return `{0x00, channel}`: a 16-bit object id. Aamm.DLL requires the value ≤ 255 (`User::Invariant` at `0x80881e8a`), so the earlier `{0x6C, channel}` guess was wrong **[verified run 43]**. |
| #7 `CustomControl(TInt aFn 23..27, const TAny*)` | 23..27 | `{arg}` | **23 = subscribe to indications**: `arg` → `TDesC8` of `(resource, message id)` byte pairs (NokiaDSY `FUN_808baa40` `0x808baa40` with the 8-byte list at `0x808bab90`; NOKIATSY `0x808c55ec` with a 110-byte list). Return 0. 24 is also used by the NokiaDSY gateway (`0x808ba876`, descriptor argument; not at boot). |
| #22 / #27 | 3 / 5 | `{TRequestStatus*, const TDesC8*}` | Async `Send` / `DataSend` (not used by boot clients). |
| #3 `DataReceive` | 6 | like Receive, uses `iPtr1` (+0x18) | not used at boot |
| #15 `NotifyFlowControlStatus` | 8 | `{TRequestStatus*, TPtr8*(4)}` uses +0x30 | not used at boot |
| #28, #6, #16 | 18 `{arg}`, 9..13 `{status*, arg}`, 37..41 | | custom sync/async/no-arg functions, not used at boot |
| #4 #2 #8 #13 #11 #14 #18 #10 #1 | 28 31 32 34 19 20 36 22 30(+Close) | none | cancels/resets (names unknown; none used at boot) |
| cancel mask (`KMaxTInt`) | | | `RBusLogicalChannel::DoCancel` is never called by IscApi itself. |

All of the above **[verified from `iscapi.dis`]** except the ctl 21 value **[inferred]**.

## 3. Message framing on the ISC link

Three libraries build messages; all agree. **[verified]**

| | ISIMESSAGE.DLL (`0x80856180`; used by EStart, EInfoServer, NOKIATSY, mc_isiserver) | PNMSG.DLL + ISIMSG.DLL (used by NokiaDSY) |
|---|---|---|
| header length | `TIsiSend(TDes8&, len)` ord15 `0x80856844`; `Complete()` ord10 `0x8085672e` writes `[4] = (len−6) & 0xff`, `[5] = (len−6) >> 8` | `PNMSG_ord1` `0x808fcfe6`: `HBufC8(n+6)`, `FillZ(6)`, `[4] = n & 0xff`, `[5] = n >> 8`; `ISIMSG` finaliser `0x808acaf2` writes the same order |
| byte access | `Set8bit` ord9 `0x808566b6`, `Get8bit` ord20, `Get16bit` ord22 (**big-endian**), `Get32bit` ord23 (big-endian), `Copy` ord11 | `ISIMSG_ord319` `0x808acbee`: `SetLength(8); [3]=resource; Append(tid); Append(msgid); Append(data); pad to 4 bytes (except resources 0x08,0x09,0x0A,0x1B,0x31)` |
| subblocks | `FindSubBlock` ord18 `0x808563c8`: id at +0, length at +1, scanning from a per-message start offset | `ISIMSG_ord303` similar |

Wire layout (offsets within the buffer handed to `Send` / returned by `Receive`):

```
 0  media            (user code writes 0 or leaves garbage)
 1  receiver device  (0x00 = PN_DEV_HOST = modem; EInfoServer copies the request's sender dev)
 2  sender device    (0x6C = PN_DEV_SOS = APE; user code writes 0)
 3  resource id      (always set by user code)
 4  length low  = total − 6     <-- LITTLE-ENDIAN on this platform (all three libraries)
 5  length high
 6  receiver object  (user code: 0; EInfoServer replies: request's sender object)
 7  sender object    (user code: 0; EInfoServer replies: its channel number 0x0B)
 8  transaction id   (EStart leaves it uninitialised; DSY uses a counter; NOKIATSY 7)
 9  message id
10.. payload, then 4-byte padding where ISIMSG applies it
```

Device ids are `[ref]` (Linux `phonet.h`: `PN_DEV_PC 0x10`; oFono/N900 usage: host 0x00,
SOS 0x6C); that the N95 kernel driver fills bytes 1/2/6/7 is **[inferred]** from the fact that no
boot client sets them. `IscApi.dll` itself passes the descriptor through unchanged (no
prepend/strip); what the multiplexer LDD adds is unknown. A fake peer only needs `[3]`, `[8]`,
`[9]`, `[10..]` of requests and must produce complete 8-byte headers on delivery
(EInfoServer reads `[1]`, `[2]`, `[7]`, `[8]`, `[10]`, `[11]` of received requests, `0x80a02830`;
NokiaDSY reads `[3]`, `[8]`, `[9]`, `0x808ba948`).

Routing **[inferred]**: object id == IscApi channel number (EInfoServer puts `0x0B` in `[7]`,
mc_isiserver registers `(0x6C, object)` with ctl 21's low byte). Responses go to the channel
that sent the request; indications go to every channel whose ctl-23 list contains
`(resource, msgid)`; requests from the modem to APE servers (e.g. `PN_EPOC_INFO 0x62`) go to the
channel whose open-time `aInfo` list contains the resource.

### 3.1 Resource and message ids seen

| Resource | Value | Where |
|---|---|---|
| PN_MTC | 0x15 [ref] | EStart reset, NokiaDSY handlers, NOKIATSY |
| PN_SIM | 0x09 [ref] | NokiaDSY, NOKIATSY |
| PN_SECURITY | 0x08 [ref] | NokiaDSY (`SEC_STATE_REQ 0x11`), subscription `0810` |
| PN_PHONE_INFO / PN_EPOC_INFO | 0x1B / 0x62 [ref] | EInfoServer serves 0x62 (`INFO_SERIAL_NUMBER_READ 0x00/0x01`, `INFO_VERSION_READ 0x07/0x08`, `INFO_PP_READ 0x02/0x03`, `INFO_PRODUCT_INFO_READ 0x15/0x16`, subblocks `0x41` IMEI, `0x48` MCUSW) |
| PN_NAMESERVICE | 0xDB [ref] | mc_isiserver `NAME_ADD_REQ 0x05`, `NAME_REMOVE_REQ 0x07` |
| 0x19, 0x1C, 0x35, 0x43, 0x48, 0x59, 0x5A | | Nokia-internal (ISIMSG classes `0x808b62d4`, `0x808b2eb8`, `0x808b3850`, `0x808b3904`, `0x808b23ec`); 0x5A is NOKIATSY's own server resource |

MTC ids [ref oFono `mtc.h`]: `STATE_REQ 0x01, STATE_QUERY_REQ 0x02, POWER_OFF_REQ 0x03,
POWER_ON_REQ 0x04, STARTUP_SYNQ_REQ 0x0B, SHUTDOWN_SYNC_REQ 0x12; STATE_RESP 0x64,
STATE_QUERY_RESP 0x65, POWER_OFF_RESP 0x66, POWER_ON_RESP 0x67, STARTUP_SYNQ_RESP 0x6E,
SHUTDOWN_SYNC_RESP 0x75, STATE_INFO_IND 0xC0`. Responses are request + 0x63, so EStart's
`0x06` → `0x69` is `MTC_RESET_GENERATE_REQ/RESP` **[inferred, consistent with NokiaDSY
registering 0x69 and EStart's panic string "EStart:ResetGenerate"]**. MTC states: `POWER_OFF 0,
NORMAL 1, CHARGING 2, ALARM 3, TEST 4, LOCAL 5, …`; actions `START 3, READY 4`.
SIM ids [ref `sim.h`]: `SIM_STATUS_REQ 0xC0 / RESP 0xC1` (data: service type, status),
`SIM_SERVER_READY_IND 0xED`, `SIM_IND 0xEF`, `SIM_ST_CARD_STATUS 0x00`, `SIM_ST_PIN 0x01`,
`SIM_SERV_OK 0x01`, `SIM_SERV_NOT_OK 0x1C`; SEC: `SEC_STATE_REQ 0x11 / RESP 0x12`,
`SEC_STARTUP_OK 0x05`, `SEC_CAUSE_NO_SIM 0x16`.

## 4. Who talks to ISC

Importers of `IscApi.dll` in the core ROM (`tools/research/xip_importers.py`): `EStart.exe`,
`Aamm.DLL`, `EInfoServer.exe`, `NokiaDSY.DLL`, `NOKIATSY.DLL`, `SAIscService.DLL`,
`wlanhwinit.dll`, `mc_isiserver.exe`, `npaprivlistener.exe`, `nlacommon.dll`,
`supllistener.exe`, `SuplPosMsgPlugin.dll`, `startuplanguageplugin.dll`. ROFS executables were
not scanned (compressed E32 images). `PHONETSY.TSY` only loads `NOKIATSY.DLL`;
`CommonDSY.DLL`/`DosServer.exe` do not touch ISC themselves. **[verified]**

## 5. Boot conversation per client

### 5.1 EStart.exe (`build/n95/ghidra/EStart.exe.c`)
- `FUN_8085c636` (main): `Initialize()` → wait → panic if not 0; `custom.ldd` controls 32/2/1;
  closes the init channel at exit (ctl 15). **No ISI message in the normal path.**
- `FUN_8085c4ea` (`0x8085c4ea`), reached only when Custom ctl 1 returns 4 or 5 (`0x8085c7a2`
  path): opens channel **0x22** (extension unit 2), sends a 12-byte MTC message
  `[3]=0x15 [9]=0x06 [10]=0x07 [11]=0x00` and waits (with a timer) for a message whose `[9]` is
  `0x69`; otherwise panics "EStart:ResetGenerate". Our Custom stand-in returns 0, so this is
  not taken.

### 5.2 EInfoServer.exe
- `FUN_808a3dec`: `Open(11, status, EOwnerThread, TBuf8{0x62})`, **`WaitForRequest`** (blocks
  its main thread until ctl 1 completes; the server is not started before that), then
  `Receive` into a 0x200-byte `HBufC8` (`FUN_808a3d64`).
- It is a *server* for `PN_EPOC_INFO` requests arriving from the modem; it never sends
  unsolicited messages. Replies copy `[2]`→`[1]`, `[7]`→`[6]`, `[8]`, set `[7]=0x0B`
  (`FUN_808a4a0a` `0x808a4a0a`). Clients: `einfo_interface.dll` (not used by the boot chain).
  A fake peer needs only to complete ctl 1; sending nothing is fine.

### 5.3 mc_isiserver.exe
- `FUN_80a0228c`: `Open(0x37, …)` (extension unit 23) → wait; failure only traces. Then ctl 21,
  and `NAME_ADD_REQ` to `PN_NAMESERVICE` (`FUN_80a026d6`). Not boot critical.

### 5.4 DosServer.exe → CommonDSY.dll → NokiaDSY.dll (the blocker)
Order in `DosSrv.dll` `FUN_838ea920` (`0x838ea920`): rename thread, `FUN_838ea858` reads
`Z:\private\101f6efa\DosServer.ini` = `CommonDSY.dll` (which reads `CommonDSY.ini` =
`NokiaDSY.dll`), `RLibrary::Load`, `Lookup(1)()` = `NokiaDSY_ord1` (`0x808ba150`) → DSY
construction; then `CServer2::StartL("DosServer")`, then `DosSrv_ord1` signals
`DosServerStartupSem` (`0x838ea976`). **[verified]**

NokiaDSY construction, ISC gateway `ISCGatewayBase` (`0x808ba828`, vtable `0x808c3830`):
1. `Open(0x18 = 24, status, EOwnerThread, 0)` + **`WaitForRequest` + `LeaveIfError`**
   (`0x808baa8c`). → ctl 1 must complete 0.
2. `GetChannelInfo(0x18, TBuf8<2>)` → ctl 21 must return 0 and write 2 bytes.
3. Subscribe (ctl 23) with the 8-byte list at `0x808bab90`:
   `09 EF 09 ED 08 10 15 C0` = SIM_IND, SIM_SERVER_READY_IND, SEC msg 0x10 (indication),
   MTC_STATE_INFO_IND.
4. First `Receive` (ctl 4) into a 0x80-byte payload buffer (0x86-byte descriptor), `aNeeded`
   at gateway+0x74.
5. `NotifyConnectionStatus` (ctl 7) posted (`FUN_808babfa`).
6. Sub-handlers register `(resource, msgid)` handlers with `CommonDSY_ord13` and send:
   - resource `0x1C`, msg `0x2D`, no data (`FUN_808bace8` `0x808bace8`): wire
     `00 00 00 1C 04 00 00 00 <tid> 2D` (10 bytes; the reply `0x2E`/`0x2F` carries a 4-byte
     value in its payload (`FUN_808baed6` stores the first word of the DOS message data), only
     consumed by a synchronous DOS query `FUN_808baf1e`, not at boot).
   - resource `0x09` (SIM), msg `0xE4`, tid 1, data `01 02 40 00 1D 00 00 00`
     (`FUN_808c06b8` `0x808c06b8`): wire
     `00 00 00 09 0C 00 00 00 01 E4 01 02 40 00 1D 00 00 00` (18 bytes). Handlers for
     `0xE5`/`0xE6` exist but nothing waits.
   Both `Send`s must return 0 (`LeaveIfError`). No construction-time MTC request exists in
   NokiaDSY (all MTC requests are built elsewhere; the DSY only *handles* MTC 0x65, 0x66, 0x67,
   0x69, 0x6E, 0x75, 0xC0 — dispatcher `FUN_808be568` `0x808be568`).
7. Later, on DOS client requests, the DSY sends `SIM_STATUS_REQ` (`0xC0`, data `00 00` or
   `01 00`, `0x808bfadc`/`0x808bfff4`), `SEC_STATE_REQ` (`0x11`, data `00 00`, `0x808bfca8`),
   SIM `0x2B`/`0x2D`, resource 0x19 msgs `0x0A`/…, 0x35 msg 2, 0x59 msg 0x42.

What the DSY does with MTC messages (`FUN_808be194` `0x808be194`, `FUN_808be4c6`):
- `MTC_STATE_INFO_IND` (`[10]`=state, `[11]`=action): `state==1 (NORMAL) && action==3 (START)` and
  `SysStartup::State() ∈ {0xCD, 0xCE}` → `SysStartup::SetState(0xCF)`; `state==3 (ALARM) &&
  action==3 && State()==0xCE` → `SetState(0xD1)`; `state==0 (POWER_OFF) && action==3` → starts
  the power-off sequence via `custom.ldd`. So **`action` must be 3 and `state` 1** for a normal
  boot; the indication only matters once SysAp has reached 0xCD/0xCE.
- `MTC_STATE_QUERY_RESP`: reads `[11]` (target state) and raises DOS event `0x330` with a mapped
  value.
- The N95 global state values (`RProperty` key 0x41 of the startup category, `SetState`
  `0x838e4be0`, default 200): 0xC9 StartingUiServices, 0xCA StartingCriticalApps,
  0xCB SelfTestOK, 0xCC SecurityCheck, 0xCD CriticalPhaseOK, 0xCE EmergencyCallsOnly,
  0xCF NormalRfOn, 0xD1/0xD2 Alarm/Charging, 0xFA/0xFC fatal **[inferred: 201-based version
  of the Symbian^3 `TGlobalState` enum, matching Starter/SysAp/DSY transitions]**.

### 5.5 Starter.exe (`build/n95/ghidra/Starter.exe.c`)
- ConstructL `FUN_83900cd4` → `FUN_83900bd4` (RFs, `RWsSession::Connect` loop,
  `RSharedDataClient::Connect` loop) → `FUN_83900b24` → **`SysStartup::GetStartupReasons`
  (blocks on DosServer)** → defines/sets P&S keys 0x41 (=0xC9), 0x42, startup reason → opens
  `Z:\resource\Starter_Arm.rsc` (ROFS1 offset `0x149e82c`, 6205 bytes) and picks a start list
  through the table in resource 2: `(mode, reason) → list id`; `(0, 100) → 3` is the normal
  list.
- RunL (`0x83901fbe`, not split by Ghidra): state 0 → `FUN_83901d44` launches resource-1's
  executable with a watcher; state 1 → `FUN_83901c44` (`0x83901c44`: CenRep, `SysLocale`,
  `akncapserverclient.dll!#39` retry loop, `REikAppUiSession::Connect` ×3, sets key 0x41 =
  0xCA); state 3 → one list item per `RunL` (`0x83901da2`).
- List 3 order (resource @0x8f): tzserver, DbRecovery, HWRMServer, accserver, MediatorServer,
  *(type 5: DosServer launch helper)*, randsvr (rendezvous), Sae, **SysAp**, Phone, idle,
  clknitzmdls, ScreenSaver, Startup, npaprivlistener, supllistener, Watcher, xnthemeserver,
  NcnList, BtServer, … Item types 3/4/12 stop the list until an external event
  (`0x83901fa6`); type 13/15 set key 0x41 (15 → 0xCB). **[inferred from the RSC bytes and the
  `__ARM_switch8` table at `0x83901df4`]**
- Starter never uses IscApi or DSClient directly; `sysutil.dll` is its only modem dependency.

### 5.6 SysAp.exe and NOKIATSY.DLL (next stage, not reached yet)
- SysAp startup controller (`FUN_8338a806` `0x8338a806`): on Starter state 0xC9/0xCA calls
  `StarterClient` and `DSClient_ord60(session, 1|0|4)` (DOS "startup state" IPC 0x193); on 0xCB
  → `SetState(0xCD)`; on 0xCC → `SetState(0xCE)`; `FUN_8338b6e4` sets 0xCF when state is 0xCE
  and no SIM/security dialogs are pending. SysAp also opens ETel (`LoadPhoneModule("phonetsy")`).
- NOKIATSY (loaded by ETel; `build/n95/ghidra/NOKIATSY.DLL.c`): `Open(7, status, EOwnerThread,
  TBuf8{0x5A})` + `WaitForRequest` + `LeaveIfError` (`FUN_808c4c98` `0x808c4c98`); subscribes
  (ctl 23) the 110-byte pair list at `0x808e7688`:
  `011B 011C 0103 0106 0113 0104 0116 0105 011E 010B 0110 010F 010C 01A0 01A7 0132 0A02 0A1E 0A35
  0A1F 0A20 0A27 0A3F 1721 0610 0609 0606 0208 020D 0810 09EF 09ED 09E6 15C0 3104 3105 310A 310D
  310E 3115 3118 311E 311F 3124 3125 312D 312F 3132 312C 3137 4818 4816 4819 3211 1606`;
  registers handlers for MTC `0xC0 0x79 0x65 0x71`, SIM `0xEF 0x0A 0xC1 0xDF 0xDD 0x14 0x02`
  (`FUN_808c78ae` `0x808c78ae`); and **at construction sends `MTC_STATE_QUERY_REQ`**
  (`FUN_808c7878` → `FUN_808c784c` `0x808c784c`): generic builder `FUN_808c5548`
  (`[3]=res, [8]=tid, [9]=msgid, [10..]=data`, padded to 4): wire
  `00 00 00 15 06 00 00 00 07 02 00 00`. Its MTC handler `FUN_808dabb8` (`0x808dabb8`) maps the
  response/indication state to an ETel modem status (1→"ready").

## 6. Minimal fake-peer specification

### 6.1 Channel controls (per `DLogicalChannel` of `NokiaISCDriver`; extension LDD should
offer the same for units 0..31 = channels 32..63)

| Control | Action |
|---|---|
| 0 `{status*}` (unit 0) | complete `KErrNone` (already done) |
| 1 `{status*, channel, info*}` | record `channel` and, if `info*` non-null, the resource-id list (read the `TDesC8`); **complete `KErrNone`** |
| 4 `{status*, TDes8*, TPtr8*}` | if queue empty: park. Else: `len = msg.size`; if `len <= aBuf.MaxLength()`: `ThreadDesWrite(aBuf, msg)`, dequeue, complete 0; else `ThreadDesWrite(aNeededPtr, (TUint16)len)` and complete −4 (keep message) |
| 7 `{status*, TPtr8*}` | park (never complete); 35 cancels it with `KErrCancel` |
| 14 `{status*}` | complete `KErrNone` (then the user closes the handle) |
| 15, 29, 28, 30, 31, 32, 34, 19, 20, 22, 36 | return 0 |
| 17 `{TDesC8*}` | read the message, parse `[3] [8] [9] [10..]`, run the reply table, return 0 |
| 21 `{channel, TDes8*}` | write 2 bytes `6C <channel>` (set length 2), return 0 |
| 23 `{TDesC8*}` | read pairs `(res, msgid)`, remember for indication fan-out, return 0; after recording, if the list contains `15 C0`, queue one `MTC_STATE_INFO_IND(NORMAL, START)` to this channel |
| 24..27, 37..41, 9..13, 3, 5, 6, 8, 18 | return 0 / complete 0 (not used at boot) |
| 33 | complete pending Receive with `KErrCancel` (−3) |
| channel close | drop queue, cancel pending requests |

Delivery: when a message is queued for a channel that has a parked Receive, complete it
immediately (from the kernel thread via `Kern::RequestComplete`); keep per-channel FIFO.

### 6.2 Request → response table (bytes as delivered to the client; `T` = request's `[8]`,
`C` = sending channel number)

| Request (`[3]`,`[9]`) | Response queued to channel `C` |
|---|---|
| `15 02` MTC_STATE_QUERY_REQ | `00 6C 00 15 06 00 C 00 T 65 01 01 00 00` = RESP, current 1 (NORMAL), target 1 |
| `15 0B` MTC_STARTUP_SYNQ_REQ | `00 6C 00 15 04 00 C 00 T 6E` (+`00 00` padding ok) |
| `15 06` (EStart reset) | `00 6C 00 15 06 00 C 00 T 69 00 00` (only `[9]==0x69` is checked) |
| `15 03`/`15 04`/`15 12` | `T 66` / `T 67` / `T 75` with 2 zero bytes; the DSY treats `[10] ∈ {0,1,5,6}` as success |
| `09 E4` (DSY init) | optional: `00 6C 00 09 04 00 C 00 T E5` (no consumer waits) |
| `09 C0` SIM_STATUS_REQ, data `st 00` | `00 6C 00 09 06 00 C 00 T C1 st 1C` = SIM_STATUS_RESP, service type echoed, `SIM_SERV_NOT_OK` (0x1C) → "no SIM" [ref]. Use `01` (SIM_SERV_OK) only when faking a SIM. |
| `08 11` SEC_STATE_REQ | `00 6C 00 08 06 00 C 00 T 12 16 00` = SEC_STATE_RESP, cause `SEC_CAUSE_NO_SIM` (0x16) [ref] |
| `1C 2D` | `00 6C 00 1C 08 00 C 00 T 2E 00 00 00 00 00 00` (payload zeros; the handler takes a 4-byte value from the payload, exact offset unverified) **[inferred]** |
| `DB 05` NAME_ADD_REQ | `00 6C 00 DB 04 00 C 00 T 06` **[inferred]** |
| `62 xx` to EInfoServer | never generated by the peer |
| anything else | no reply (log it) |

Indication to send once per subscribing channel (DSY channel 24, later NOKIATSY channel 7):
`00 6C 00 15 06 00 C 00 00 C0 01 03` = MTC_STATE_INFO_IND, state NORMAL, action START.
Re-send it to channel `C` after answering `MTC_STATE_QUERY_REQ` from `C` as well, so that the
DSY sees it after SysAp has moved the global state to 0xCD/0xCE (the DSY ignores it earlier).
"Offline / RF off, no SIM" is expressed by `SIM_SERV_NOT_OK` and `SEC_CAUSE_NO_SIM` above; MTC
state must still be NORMAL (1) (`MTC_RF_INACTIVE 0x0A` would be mapped to a non-ready status by
NOKIATSY `0x808dabb8` and is not handled by the DSY).

Header fields for delivered messages: `[0]=0x00` media, `[1]=0x6C`, `[2]=0x00`, `[4..5]` LE
length = size − 6, `[6]=C`, `[7]=0x00`, `[8]=T` (0 for indications). User code checks none of
`[0..2]`, `[6]`, `[7]`.

## 7. Other blockers in the Starter / DOS / SysAp chain

- **Non-modem, after DosServer:** `FUN_83901c44` loops on `akncapserverclient.dll!#39` and
  `REikAppUiSession::Connect` (AknCapServer/EikSrv must come up; they are started on demand by
  those client DLLs); `RSharedDataClient::Connect` loop in `FUN_83900bd4`; list items need the
  ROFS executables (`tzserver`, `HWRMServer`, `accserver`, …). `HWRMServer`/`accserver` have
  their own adaptation plugins (`SALightsService`, `SAAccessoryService`, `SAVibraService`,
  `SAIscService.DLL`) that use ISC/ISIMESSAGE — resources `0x3A`-class lights/accessory
  messages will appear next; not analysed here.
- **DOS side:** `NokiaDSY` registers `RProperty` watchers (`FUN_808be70e`) and opens
  `custom.ldd` (`FUN_808ba35e`, `DoCreate("custom", TVersion(2,0,…))`) on power-off paths only.
  A DSY sub-object connects to ETel (`FUN_808bd4d8`) but no caller was found; treat as lazy.
- `EInfoServer` and `mc_isiserver` main threads also sit in `WaitForRequest` on ctl 1; nothing
  in the boot chain depends on them, but completing ctl 1 unblocks them for free.
- `LanguageSelection` panic `StartupLangPlugI -5`: `startuplanguageplugin.dll` imports IscApi
  and ISIMSG (it queries the modem for the language/product variant); it will start working
  once ctl 1 completes and will then need a reply — unknown message, log it (open question).

## 8. Open questions / confidence

- Exact values the real `NokiaISCDriver` returns for ctl 21 and the media/device bytes it fills
  in: inferred (§2.2, §3). Verify by logging the first `Send` from NokiaDSY and NOKIATSY once
  ctl 1 completes.
- Whether `MTC_STATE_INFO_IND` is delivered to a channel immediately on subscription on real
  hardware is unknown; sending it both on subscription and after each state query is harmless.
- The meaning of resource `0x1C` msg `0x2D`/`0x2E`, SIM `0xE4`/`0xE5`/`0xE6`, SEC `0x10`, and the
  Nokia-internal resources 0x19/0x35/0x43/0x48/0x59 is not public; replies above are shaped to
  satisfy the parsers only.
- The `TSWState` numbering (§5.4) is inferred from behaviour; if the "normal" transition does not
  happen, check `RProperty` key 0x41 of category `*0x839010a4` at `SysAp` time.
- `Starter_Arm.rsc` item decoding (type/flags/timeout fields) is approximate; the executable
  order is reliable.
- Not analysed: `Aamm.DLL`, `SAIscService.DLL`, `wlanhwinit.dll`, `nlacommon.dll`,
  `npaprivlistener.exe`, `supllistener.exe` (all later than the current blocker).

Helper added: `tools/research/xip_importers.py` (lists XIP importers of a DLL by ordinal).
