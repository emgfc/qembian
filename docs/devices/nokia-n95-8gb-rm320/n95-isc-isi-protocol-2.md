# N95 8GB APE↔modem messaging, part 2: the requests the fake peer does not answer yet

Continuation of [n95-isc-isi-protocol.md](n95-isc-isi-protocol.md) (same conventions: wire offsets
are those of the buffer handed to `RIscApi::Send`/returned by `Receive`, `[8]` = transaction id,
`[9]` = message id, `[10..]` = payload; length `[4..5]` little-endian = total − 6). Input: the
ISC traffic of the 2026-11-04 hybrid boot (evidence/e1/hybrid-isc-traffic.txt),
the Ghidra decompiles in `build/n95/ghidra/` (NOKIATSY, SAFeatureInfoService, SAServer, SAClient,
FeatureInfoAdaptation, FeatMgrServer, startuplanguageplugin, EInfoServer, NokiaDSY) and
`build/n95/ghidra/DATAPORT.CSY.c`, produced with the new `tools/e32load.py` from the ROFS image
(carved with `tools/research/rofs_extract.py`, added here). Public references: oFono
`drivers/isimodem/{network,info,mtc,gpds,sim,gss}.h` (constants only) and the user's own BB5 notes
(notes on the BB5 service-tool wire format, resource `0x23`).

Legend: **[verified]** read from the N95 binaries (address given); **[ref]** public reference
(names/values); **[notes]** the user's BB5 notes; **[inferred]** reasoned, not measured.

## 1. Summary

- **Boot-relevant and currently unanswered:**
  1. `PN_PHONE_INFO (0x1B)` msg `0x29` and `0x2E` from `SAServer` (channel 35): these are the
     *feature-info* reads of `SAFeatureInfoService.dll`. The client request inside SAServer is
     completed only when **both** replies have arrived (state machine `0x809230xx`), and the
     client is `FeatMgrServer.exe` (it started `SAServer.EXE`: serial log line 3056826) through
     the ECom plugin `FeatureInfoAdaptation.dll` → `SAClient.dll`. Without the replies the feature
     manager never gets its variant features; with a malformed reply it gets `KErrGeneral`.
     Answer both (§3.2). **[verified]**
  2. `NOKIATSY` (channel 7) sends, at construction, `INFO 0x29` (one product-profile flag),
     `NET_RSSI_GET_REQ`, `MTC_STATE_QUERY_REQ`, two requests to resource `0x17`, a name-service
     query, `NET_RAT_REQ`, `NET_MODEM_REG_STATUS_GET_REQ`, `SIM_STATUS_REQ`, `MTC 0x16`. None of
     them blocks the TSY (all are active-object driven, no timers), but every one that is answered
     turns into an ETel completion; `MTC_STATE_QUERY_RESP` is the only one that matters for
     "modem ready". The replies already sent for `MTC 0x02` and `SIM 0xC0` are compatible with
     NOKIATSY's parsers (§3.1.3, §3.1.9). **[verified]**
  3. `LanguageSelection.exe` (channel 32) does **synchronous** receives with a **5 s timer** per
     request and retries: SIM `0x1F` ×3 service types × 10 rounds, SIM `0x19` ×2, `0x23 0x04` ×2 →
     **≈ 170 s of boot delay** when nothing answers. Any syntactically valid reply ends the loops
     (§3.3). **[verified]**
  4. `DATAPORT.CSY` (channel 46): control 22 is `RIscApi` export #10, a synchronous call whose
     **return value is used as a buffer size** (`FUN_7001412a` `0x7001412a`); our stub returns 0.
     Control 8 is `NotifyFlowControlStatus` (park it). The name-service registration reply is not
     parsed by DATAPORT at all (§3.4). The `C32-fault 0` is not explained by the ISI traffic; see §5.
- **Not a modem reply but will bite:** `Kern::MutexWait`/`MutexSignal` assertions
  ("Calling thread must be in critical section") on every ISC control of `DATAPORT` (serial log
  around line 603054): `iscpeer.inl` takes `iF->iLock` without `NKern::ThreadEnterCS()`; on a udeb
  kernel this is an assertion per call and a real risk when the thread is killed while holding
  the mutex.

## 2. Resource ids used by the boot clients

| Id | Name | Evidence |
|---|---|---|
| `0x0A` | `PN_NETWORK` | NOKIATSY sends `0x0B` with `00 02` = `NET_RSSI_GET_REQ(NET_CS_GSM, NET_CURRENT_CELL_RSSI)`, `0x36` = `NET_RAT_REQ`, `0x00` = `NET_MODEM_REG_STATUS_GET_REQ`; its handlers expect `0x01/0x02/0x0C/0x1E/0x20/0x27/0x35/0x37/0x3F` **[verified `0x808cc752`, ref network.h]** |
| `0x15` | `PN_MTC` | as in part 1 |
| `0x16` | `PN_CSD` (circuit-switched data) | NOKIATSY builds `0x16` msg `0x03` with subblock `0x0A` for a data call (`0x808cf4b2`); handles `0x04`, `0x06` **[verified, name inferred]** |
| `0x17` | **GSS** (GSM stack server, older numbering) | NOKIATSY sends msg `0x00` with an *operation* byte (3) and receives `0x01` with a subblock holding a RAT value 0/1/2 mapped to GSM/UMTS/Dual network-mode caps; same `REQ 0x00 / RESP 0x01 + operation` shape as oFono `GSS_CS_SERVICE_REQ/RESP` which lives at `0x32` on later firmware **[verified `0x808c5e7c`; name inferred]** |
| `0x1B` | `PN_PHONE_INFO` | msg `0x29/0x2A` carry product-profile subblock `0x74`; `0x2E/0x2F`; `0x00/0x01`, `0x0B/0x0C` (EInfoServer) **[verified; ref info.h for `0x00/0x01`]** |
| `0x23` | `PN_PERMANENT_DATA` (PERM server) | **[notes: bb5-adl-wire-format.md line 393]**; the language plugin reads msg `0x04` → `0x05` |
| `0x31` | `PN_GPDS` | NOKIATSY uses `0x19 STATUS_REQ`, `0x30 CONFIGURATION_INFO_REQ`, `0x3A CONTEXTS_CLEAR_REQ`, `0x13 ATTACH`, `0x16 DETACH`, … all matching gpds.h **[verified, ref]** |
| `0x32` | unknown Nokia resource (NOKIATSY handles msgs `1, 2, 0x10, 0x11, 0x13`); *not* GSS on this firmware | **[verified registration list]** |
| `0x48`, `0x53`, `0xD9` | `0x48` and `0x53` Nokia-internal; `0xD9` = `PN_PIPE` (DATAPORT builds pipe messages `0x60`, handles responses with `[11] ∈ {1, 0x0D}`) **[verified DATAPORT `0x70016318`, name ref phonet]** |
| `0xDB` | `PN_NAMESERVICE` | `PNS_NAME_QUERY_REQ 0x01 / RESP 0x02`, `PNS_NAME_ADD_REQ 0x05 / RESP 0x06`, `PNS_NAME_REMOVE_REQ 0x07` (mc_isiserver `0x80a026d6`, NOKIATSY `0x808e2128`, DATAPORT `0x70016126`) **[verified ids; names ref]** |

Subblock encodings used by `ISIMESSAGE.DLL` `FindSubBlock` (ord18) `aType` **[verified from the
bytes the same code emits]**: type 1 = `id8 len8 …`; type 2 = `id8 filler8 len16(BE) …`
(the `0x74` subblock: `74 00 00 14`); type 3 = `id16(BE) len16(BE) …` (the `0xF000` subblock);
type 4 = 16-bit id, used for SIM subblocks (`0xFB`, `0xDF`, `0x190`).

## 3. Per-client analysis

### 3.1 NOKIATSY.DLL (ETel TSY in `c32exe.exe::CTelServer`, channel 7)

**Dispatch.** `FUN_808c5818` (`0x808c5818`) looks a received `([3], [9])` up in a sorted table of
`(resource, msgid | -1, handler*)` built with `FUN_808c56cc`; each handler object gets the
message through a thunk at `object+8`. The full registration list (grep `FUN_808c56cc(` in the
decompile) is the set of messages NOKIATSY ever expects:
`01:{0A,12,13,18,1A,1B,1C,*}` `02:{0C,0D,*}` `06:{01,02,03,05,06,10}` `08:{02,03,09,0B,0C,0E,0F,*}`
`09:{02,0A,14,1A,24,2E,34,3E,61,6D,BB,BD,C1,D5,DD,DF,E8,EF,*}` `0A:{01,02,35,37,3F,*}`
`15:{65,71,79,C0}` `16:{04,06}` `17:{01,0B,21}` `1B:{01,03,2A}` `31:{0D,37,3B,*}`
`32:{01,02,10,11,13}` `48:{*}` `53:{03,05}` `D9:{*}` `DB:{02}`. **[verified]**
Nothing in NOKIATSY waits synchronously on the modem and no request has a timer: unanswered
requests only leave ETel requests pending. The per-request facts:

#### 3.1.1 `1B 29` — product-profile flag read (tid 5)
Sent by `FUN_808ccee0` (`0x808ccee0`) from the "phone" object's init (`FUN_808ccf7a`):
`[10]=00 [11]=01` then subblock `74 00 00 0C | 00 00 00 01 | 01 26 00 00` = one entry,
PP id `0x0126`, value 0. Handler `FUN_808cd14c` (`0x808cd14c`): requires `[10]==0` (INFO_OK),
finds subblock `0x74` (type 2) from offset 12, reads the 32-bit entry at `sb+8` and sets a flag
(`this+0x93C`, consulted at `0x808cd8xx` only) when its low 16 bits == 2. **Reply** with the same
subblock and value 0 (flag off). Not blocking. **[verified]**

#### 3.1.2 `0A 0B` — `NET_RSSI_GET_REQ` (tid 4, data `00 02 00 00 00 00`)
Sent by `FUN_808cafc8` at init. Handler at `0x808cbf22` (not split by Ghidra; disassembly):
`[10]` must be 0 (`NET_CAUSE_OK`), subblock id `0x04` (`NET_RSSI_CURRENT`, type 1) from offset 12,
`sb+2` = signal percent, `sb+3` = dBm-ish value; `FUN_808cbec4` turns them into bars
`(pct*8-1)/100` and completes the ETel signal-strength notification. **Reply**
`00 01 04 04 00 00` → 0 bars. If `[10]≠0` nothing happens, which is also fine. **[verified]**

#### 3.1.3 `15 02` — `MTC_STATE_QUERY_REQ` (already answered)
Handler `FUN_808c84bc` (`0x808c84bc`): `cur=[10]`, `target=[11]`; **if `cur≠target` it re-sends
the query** (`FUN_808c784c`) — a loop if a peer ever answered `01 00`; if equal and changed it
stores the state, and for state 1 (NORMAL) or 10 (RF_INACTIVE) starts the network sub-object
(`FUN_808cea1c`). Our `01 01` is right. `MTC_STATE_INFO_IND` handler `FUN_808c8522`
(`0x808c8522`) only accepts **action `[11] ∈ {0x04 READY, 0x0C NOS_READY}`**, so the
`01 03` (START) indication we send for the DSY is ignored by NOKIATSY — harmless, the query
response already sets the state. **[verified]**

#### 3.1.4 `17 0A` (tid 6, data `02 00`) and `17 00` (tid 6, data `03 00`) — GSS reads
Both sent from `FUN_808c5c4a` at init (`FUN_808c5c18` → msg `0x0A`, `FUN_808c5be6` → msg `0x00`,
payload = `operation, filler`). Handlers (`0x808c71f8` dispatch):
- `17 01` ← `FUN_808c5e7c` (`0x808c5e7c`): requires `[10]==1`; if `[11]` (nsb) ≠ 0 finds subblock
  id `0x00` (type 1) from offset 12 and reads `sb+2`: 0→1 (GSM), 1→2 (UMTS), 2→4 (Dual) =
  `RMmCustomAPI` network-mode caps, stored at `+0x50`, then completes IPC 20067. **Reply**
  `01 01 00 04 02 00` (Dual).
- `17 0B` ← `FUN_808c5df4` (`0x808c5df4`): requires `[10]==1`; subblock id `0x16` (type 1),
  `sb+2` = max, `sb+3` = current → percent `cur*100/max` stored at `+0x54`, same IPC. **Reply**
  `01 01 16 04 07 00` (0 %).
- `17 21` indication ← `FUN_808c5f02`: 16-bit flags at `[10]`: bit 0 → re-read msg 0, bit 3 →
  re-read msg 0x0A, bit 4 → zero the percentage. Never needed. **[verified]**

#### 3.1.5 `DB 01` — `PNS_NAME_QUERY_REQ` (tid 0x0F, data `00 00 | 00 00 00 31 | 00 00 00 31`)
Sent by `FUN_808e2128` (`0x808e2128`, GPDS object init): filler(2), name(4)=`0x31`, mask/end(4)=`0x31`.
Handler `FUN_808e2226` (`0x808e2226`) for `DB 02`: `count = Get16(msg, 12)`; entries of 8 bytes
from offset 16: `name[4] dev obj flags filler`; for the entry whose `name[3]==0x31` it stores
`dev`→`+0x22`, `obj`→`+0x23` (defaults `0x00`, `0x33`, `FUN_808e21ba`). Only the GPDS pipe code
uses them. **Reply** (optional, keeps the defaults):
`00 01 | 00 01 | 00 00 | 00 00 00 31 | 00 33 00 00`. **[verified layout]**

#### 3.1.6 `0A 36` — `NET_RAT_REQ` (tid 0xDC, data `00` = `NET_CURRENT_RAT`)
Handler `FUN_808dd21e` (`0x808dd21e`), shared with `NET_RAT_IND 0x35`: for a `0x37` response it
requires `[10]≠2` and `tid==0xDC`; subblock `0x2C` (`NET_RAT_INFO`, type 1), `sb+2`: 1→2
(`ENetworkModeGsm`), 2→6 (`ENetworkModeWcdma`), stored at `+0x140`, ETel mode notification.
**Reply** `00 01 2C 04 01 00` (GSM). **[verified]**

#### 3.1.7 `0A 00` — `NET_MODEM_REG_STATUS_GET_REQ` (tid 4, no data)
Two objects handle `0A 01`:
- `FUN_808dd3c0` (`0x808dd3c0`): subblock `0x00` (`NET_REG_INFO_COMMON`, type 1) → `sb+2` = reg
  status mapped by the 12-entry table at `0x808dd384` into an ETel registration status; subblock
  `0x09` (`NET_GSM_REG_INFO`) → bytes `+0xD..+0x12` (MCC/MNC/LAC) if present.
- `0x808cc28c` (disassembly): `[10]` cause → `FUN_808e3450(10, 0, cause)` error code for the
  pending ETel request (0 for cause 0); if cause 0 it also runs the indication parser
  `0x808cbfea`, which for each subblock looks for ids `0x39`, `0x09`, `0x0A` and ignores others.
**Reply** `00 01 00 04 06 00` = cause OK, one subblock, `NET_REG_STATUS_NOSERV_NOSIM` (6)
[ref]; with no SIM a real modem reports exactly that. Absent subblocks are skipped safely.
**[verified]**

#### 3.1.8 `09 C0` — `SIM_STATUS_REQ` (service 0, answered with `C1 00 06` since run 42)
**NokiaDSY** (`0x808bfb54`) maps the status: `0x01`/`0x1C`/`0x4B` → 3 = card present (then waits
for SIM init), `0x06`/`0x24` → 4 = no card (DOS SIM state → sae 0x100052E9 = 5). `SEC_STATE_RESP`
(`0x808bfd20`): cause `0x16` (NO_SIM) is ignored; `0x05` (STARTUP_OK) → security-code status → sae
0x100052F0 = 1. Both are needed for Startup's no-SIM path (state 8). **[verified by run 42]**

Handler `FUN_808c82dc` (`0x808c82dc`): **only `[11]==1` (`SIM_SERV_OK`)** does anything (marks
"SIM ready", custom IPC 1000659). Any other status is ignored, so `0x1C` (`SIM_SERV_NOT_OK`) is
acceptable; NOKIATSY learns "no SIM" from `SIM_IND 0xEF` / SEC messages, not from this reply.
`SIM_IND` handler `FUN_808c83a6`: `[10]` service, `[11]` status; `(0x0D, 0x0B INIT_OK, [12]==8)`
→ SIM ready; `(7, 0x4F)` → data available; `(5, 6)` → reset flags; `(1, 2)` → PIN info at `[14]`.
Not needed for boot. **[verified]**

#### 3.1.9 `15 16` (tid 7, data `00 00`)
Sent by `FUN_808c799c` (`0x808c799c`) right after the state query; response `0x79` handled by
`FUN_808c844c` (`0x808c844c`): `[10]==0` → value 4 else 3, completed on custom IPC 1000661;
additionally, when `[10]==0` **and the MTC state is `0x0A` (RF_INACTIVE)** it starts the SIM/SEC
queries (`FUN_808c7a84`, `FUN_808c841a` = `SEC 0x11`). Given the request/response pair
`0x16/0x79` and the RF-state test, this is `MTC_RF_STATUS_QUERY_REQ/RESP` **[inferred, ids not in
oFono mtc.h]**. **Reply** `00 00` (`[10]=0` = OK / RF off): takes the normal path, no extra queries
because our state is NORMAL. **[verified behaviour]**

#### 3.1.10 `31 19` / `31 30` — GPDS (tid 0xDD / 0xDF)
`GPDS_STATUS_REQ` and `GPDS_CONFIGURATION_INFO_REQ` from the packet-data object, after the
network object is up. Handlers exist (`0x808dd4c8` dispatch) but nothing at boot depends on them.
**Not needed for boot**; no reply, or `T 1A 00 00` / `T 31 00 00` if logging noise is unwanted
(unverified parsers — leave unanswered).

#### 3.1.11 SEC (`0x08`) note
NOKIATSY's SEC dispatcher (`0x808c8cf6`, `__ARM_switch8` on `msgid−2`) handles `0x02, 0x03, 0x05,
0x06, 0x08, 0x09, 0x0D, 0x0E, 0x0F, 0x11` and **ignores `0x12`**; `FUN_808c8578` (msg `0x11`)
treats `[10]==5` (`SEC_STARTUP_OK`) as "security ready". NokiaDSY, on the other hand, sends
`0x11` and registers handlers for `0x10` and `0x12` (part 1). Keep answering `08 11` with `0x12`
(the DSY is the consumer); NOKIATSY does not send `08 11` in a NORMAL-state boot. **[verified]**

#### 3.1.12 Indications NOKIATSY subscribes to (55 pairs, part 1 §5.6)
None is required for boot. The only one with boot-time effect is `15 C0` (needs action 4/0x0C
to be accepted, see 3.1.3). `0A 1E` (`NET_RSSI_IND`, `[10]`=percent `[11]`=dBm) and `0A 02`
(`NET_REG_STATUS_IND`, same layout as the `0x01` response without the cause byte at `[10]`;
`[11]` is nsb there too — `FUN_808dd3c0` reads the cause only for msg 1) can be used later to
fake coverage. **[verified]**

### 3.2 SAServer.EXE / SAFeatureInfoService.dll (channel 35, `SAIscService.DLL` gateway)

- Channel: `SAIscService` opens `0x23` (35) with `aInfo=0` (`0x80911d30+…`, `imp_IscApi_ord21(…,0x23,…)`),
  subscribes `17 21 1C 37 1C 42` (lights/accessory indications used by the other SA plugins). **[verified]**
- `SAFeatureInfoService` (`0x80922b50`): on the first client request (`FUN_8092305e`, state 0) it
  creates one sub-request object and calls its start (`FUN_809235b2` `0x809235b2`): state=1 and
  sends **both** `1B 29` (`FUN_80923554`) and `1B 2E` (`FUN_8092344c`); the responses are routed by
  SAServer by tid (`SAServer_ord1`) to `FUN_80923348`, which **leaves with `KErrGeneral` on
  any other message id**. When the outstanding count (`+0x18`) reaches 0 the sub-request is state 2
  (or 3 on parse error) and `FUN_80922fda` → `FUN_80922eec` completes the client with the 20-byte
  feature block (`+0x2C`) or `KErrGeneral`. **No timer** anywhere in SAServer.DLL or the plugin:
  the client waits forever. **[verified]**
- Client chain: `FeatMgrServer.exe` ConstructL (`FUN_838e26fa` `0x838e26fa`, after `CServer2::StartL`
  and loading `STATICFEATURES.DLL`) → `FUN_838e251c` lists/creates ECom plugins and calls their
  "feature info" method (`FUN_838e1e98` vtable+0xC) → `FeatureInfoAdaptation.dll` (`0x80922550`)
  → `SAClient` session (`SAClient_ord5`, which starts `SAServer.EXE` and waits for its rendezvous,
  `FUN_80910c6e`) → async request `SAClient_ord2`. The serial log confirms `!FeatMgrServer`
  created `SAServer.EXE` (line 3056826). **[verified]** Whether FeatMgrServer withholds its
  own rendezvous until the plugin answers was not established (its request path is async);
  either way every `FeatureManager` client gets wrong/late answers until the reply arrives.
- Request `1B 29` (tid 2): `[10]=00 [11]=01`, subblock `74 00 00 14 | 00 00 00 03 | 01 08 00 00 |
  00 23 00 00 | 01 35 00 00` (`FUN_809234ac`): three PP ids `0x0108`, `0x0023`, `0x0135`.
  Parser `FUN_809232b4` + `FUN_80923226` (`0x80923226`): `[10]==0`, `[11]≠0`, subblock `0x74`
  (type 2) from offset 12; `len=Get16(sb+2)`, `count=Get16(sb+6)`, entries `Get32(sb+8+4i)` =
  `id<<16 | value`; `0x0108`→feature 0 (`value≠0`), `0x0023`→feature 1 (`value≠0`), `0x0135`→
  feature 2 (`value & 0x10`); an unknown id → `KErrGeneral`. **[verified]**
- Request `1B 2E` (tid 2): `[10]=00 [11]=00`. Parser `FUN_8092313e` (`0x8092313e`): `[10]==0` **and
  `[11]≠0`** (a reply without subblocks is treated as an error → state 3 → client gets −2);
  subblock id `0xF000` (type 3) from offset 12: `flags=Get32(sb+4)`; bit 0 && `Get32(sb+8)≠0` →
  feature 3; bit 1 && `Get32(sb+12)≠0` → feature 4. **[verified]** Message name unknown
  (`INFO_…_READ`, `0x2E/0x2F`) **[inferred]**.
- The five feature bits are forwarded to FeatMgr as variant features; which S60 feature ids they
  are was not traced (`FUN_809230a0` stores by index). Replying "all off" is the conservative
  choice; if a UI feature later turns out to be gated on one of them, flip the value here.

### 3.3 LanguageSelection.exe / startuplanguageplugin.dll (channel 32)

Opens extension channel `0x20` (32) with `aInfo=0` (`FUN_816e2xxx`, `imp_IscApi_ord21(…,0x20,…)`),
no subscription. Every request is `Send` + a **synchronous** `Receive` with an `RTimer` of
`0x4C4B40` µs = **5 s** (`FUN_816e22d6` `0x816e22d6`; timeout → `ReceiveCancel`, `KErrTimedOut`
(−33); `KErrOverflow` → grows the buffer from `aNeeded` and retries). Order
(`FUN_816e27fc` `0x816e27fc`): **[verified]**

1. `FUN_816e279a`: loop up to 10 rounds of SIM `0x1F` with service types `02`, `01`, `0x2E`
   (payload `st 00`). Reply `0x20` parsed by `FUN_816e25c6` (`0x816e25c6`): `[10]` = service
   type, `[11]` = status: `1` ok, `0x24` (`SIM_SERV_DATA_NOT_AVAIL`) → −1, `0xFB`
   (`SIM_SERV_NOTREADY`) → −18, else −5; on ok `[12]` = count, then for service 1/2 pairs of
   ISO-639 letters at `[13+2i]`, for `0x2E` one-byte language codes at `[13+i]`, looked up in
   tables → preferred UI languages. This is the SIM language-preference read (EF_LP/EF_ELP)
   **[inferred from the parser]**. The loop ends as soon as all three receives *succeed*
   regardless of status, so with no SIM reply `20 st 24`.
2. `FUN_816e24dc`: SIM `0x19` (`SIM_NETWORK_INFO_REQ` [ref]) service `0x2F` (`READ_HPLMN` [ref]),
   payload `2F 00`, up to 2 rounds. Reply `0x1A` parsed by `FUN_816e2214` (`0x816e2214`):
   `[11]`: 1 ok (then BCD MCC from `[12]`/`[13]` → country → language), `0x24` → −5 (stops),
   `0xFB` → −18 (retry once), else −5. Reply `1A 2F 24`.
3. `FUN_816e2a0a`: resource `0x23` msg `0x04`, payload `00 D4 00 00 00 00 00 00 00 00 00 00 00 02`
   (built by `FUN_816e2920` through an ISIMSG class, hence the 4-byte padding), up to 2 rounds.
   Reply `0x05` parsed by `FUN_816e2828` (`0x816e2828`): `[10]==0`, then a subblock whose first
   byte at the iterator position == 1, and `Get16(msg, 0x18) == 0x69` sets a boolean that
   `FUN_816e2a90` uses to choose a language-selection variant; any other well-formed reply →
   boolean false, which is the normal (non-variant) case. **Reply** with `[10]=0`, nsb 1 and a
   16-byte subblock of zeros: `00 01 01 10 00 00 00 00 00 00 00 00 00 00 00 00 00 00` (so
   `[12]=1` is the subblock id, `[0x18..0x19]=0`). `PN_PERMANENT_DATA` msg names unknown **[inferred]**.
4. `FUN_816e27fc` completes with 0 if at least one language was found, else −2; the plugin then
   falls back to the ROM language list. The earlier `StartupLangPlugI -5` panic (part 1) was the
   `ctl 1` failure; with replies it does not recur.

So nothing here blocks forever, but without replies LanguageSelection holds the boot for about
three minutes (the start list waits for it: it is a *rendezvous* item in `Starter`).

### 3.4 DATAPORT.CSY (channel 46, `c32exe.exe::CCommServer`)

Decompile: `build/n95/ghidra/DATAPORT.CSY.c` (loaded at `0x70014000` as in the trace).
- `FUN_700141fc` (`0x700141fc`): `RIscApi` ctor (ord29), `Open(channel, status, EOwnerThread, 0)`
  with channel `0x2E` for port `DATAPORT::0`, `0x2F` for `::1`, `0x1F` for `::2/::3`,
  `WaitForRequest`, `LeaveIfError`. **[verified]**
- `FUN_7001412a` (`0x7001412a`): `n = RIscApi #10 (DoControl 22)`; allocates two buffer objects with
  `(n, n*4)` (`FUN_70014e66`: `HBufC8::NewL(size)` + `Fill('A')`), i.e. **ctl 22 returns a size
  (bytes) and must be positive**. The real value is unknown; `0x400` is a safe guess **[inferred]**.
  Then: receive object (`FUN_70016000`: `Receive` into the first buffer), flow-control object
  (`FUN_7001546a`: `RIscApi #15` = **ctl 8 `NotifyFlowControlStatus {status*, TPtr8*(4)}`**,
  `SetActive`; leave it pending), name registration (`FUN_70015dca` → `FUN_70016126`
  `0x70016126`), then the pipe/port objects. **[verified]**
- `PNS_NAME_ADD_REQ` (`FUN_70016126`): `00 00 | 00 0A 01 D9 | 00 | obj | 80 | 00` = filler(2),
  name(4) = `0x000A01D9` (port 0; `0x00010DD9` for ports 2/3), **dev = 0x00**, obj = low byte
  of `GetChannelInfo` (ctl 21 → our `6C 2E` → `0x2E`), flags `0x80`, filler. mc_isiserver uses the
  same layout with dev `0x6C` (`0x80a026d6`). The request is sent with `[8]` = a per-channel tid
  (`FUN_70018074`) and the registration object only waits for its own `iStatus` (set to
  `KRequestPending` and completed by the send path), state 2 → 1 (`FUN_70015e96`).
  **No handler for resource `0xDB` exists in DATAPORT** (its two response parsers
  `FUN_70016318`/`FUN_70016382` are for `PN_PIPE 0xD9`, status `[11] ∈ {1, 0x0D}`), so the content
  of `PNS_NAME_ADD_RESP` is irrelevant to it; `db 06 00 00` is as good as anything. The response
  layout from the name-service convention is `reason(1) filler(1)` **[inferred]**; keep it.
- `ctl 22` is `RIscApi` export #10 (`IscApi_ord10` `0x8084c13c`, no arguments, returns the
  `DoControl` value); `ctl 34` is export #13 (`0x8084c05e`), not used at boot. **[verified]**
- The leave: the only `LeaveIfError` sites after the ISC init are the pipe/port constructors
  (`FUN_70015030`, `FUN_7001777c`, `FUN_700170b6`, `FUN_7001663a`, `FUN_70016994`, `FUN_70016d66`) and
  `User::Leave(KErrNotSupported)` for port numbers > 3 (`0x70016126`). The `C32-fault 0` is not
  in `build/boot-test/hybrid/serial0.log` (only the summary header mentions it), so the exact
  leave code could not be read from the log; the ctl 22 = 0 result is the first thing to fix.

### 3.5 EInfoServer.exe (channel 11): `1B 0B` with `[2]=0x62 [7]=0x0B`
Not in the task list but it appeared in the traffic. `FUN_808a5f46` (`0x808a5f46`) forwards a
client request (`einfo_interface.dll`) as an `INFO` read whose message id/data come from a jump
table (`0x0B`, data `00 02 …` here; the `0x00/0x41` IMEI read is the other branch). The reply
parser `FUN_808a5998` (`0x808a5998`) accepts `[9] ∈ {0x01, 0x08, 0x0C}`, needs `[10]==0`, reads
the subblock id at `[12]` (`'A'` IMEI, `'I'`, `'N'`, `'M'`), the string length at `[15]` and the
string from `[16]`, and leaves with −1 on a wrong id. For `0x0C` it expects `'N'` (data `00 02`)
or `'M'` (`00 01`): product name / manufacturer **[inferred]**. Optional reply:
`00 01 4E 0C 00 06 'R' 'M' '-' '3' '2' '0' 00 00`. Which client asked was not traced (likely
`MC_info.exe`/crash reporting); nothing in the boot chain is known to wait for it.

## 4. Reply table additions (bytes as delivered; `T` = request `[8]`, `C` = channel; header
`00 6C 00 res len_lo len_hi C 00` as in part 1; `len = total − 6`)

| Request `[3] [9]` (client) | Deliver to `C` | Notes |
|---|---|---|
| `1B 29` (NOKIATSY, 1 entry `01 26`) | `00 6C 00 1B 12 00 C 00 T 2A 00 01 74 00 00 0C 00 00 00 01 01 26 00 00` | echo the request's entries with value 0; **generic rule:** copy the request payload from `[10]`, set `[10]=00`, keep `[11]=01`, zero the low 16 bits of every entry at `sb+8+4i` |
| `1B 29` (SAServer, 3 entries) | `00 6C 00 1B 1A 00 C 00 T 2A 00 01 74 00 00 14 00 00 00 03 01 08 00 00 00 23 00 00 01 35 00 00` | same generic rule (22-byte payload → total 32, `len=0x1A`) |
| `1B 2E` (SAServer) | `00 6C 00 1B 16 00 C 00 T 2F 00 01 F0 00 00 10 00 00 00 00 00 00 00 00 00 00 00 00` | `[11]` **must** be ≥ 1; subblock `F000`, len 16, flags 0 |
| `0A 0B` `NET_RSSI_GET_REQ` | `00 6C 00 0A 0A 00 C 00 T 0C 00 01 04 04 00 00` | cause OK, `NET_RSSI_CURRENT` 0 % |
| `0A 36` `NET_RAT_REQ` | `00 6C 00 0A 0A 00 C 00 T 37 00 01 2C 04 01 00` | `NET_RAT_INFO` = GSM |
| `0A 00` `NET_MODEM_REG_STATUS_GET_REQ` | `00 6C 00 0A 0A 00 C 00 T 01 00 01 00 04 06 00` | `NET_REG_INFO_COMMON`, status `NOSERV_NOSIM` |
| `17 00` (op 3) | `00 6C 00 17 0A 00 C 00 T 01 01 01 00 04 02 00` | `[10]=1` required; subblock 0 value 2 = dual mode |
| `17 0A` (op 2) | `00 6C 00 17 0A 00 C 00 T 0B 01 01 16 04 07 00` | `[10]=1` required; subblock `0x16` max 7, current 0 |
| `15 16` | `00 6C 00 15 06 00 C 00 T 79 00 00` | `[10]=0` |
| `DB 01` `PNS_NAME_QUERY_REQ` | `00 6C 00 DB 12 00 C 00 T 02 00 01 00 01 00 00 00 00 00 31 00 33 00 00` | optional; one match, name `0x31`, dev 0, obj `0x33` (= NOKIATSY default) |
| `DB 05` `PNS_NAME_ADD_REQ` | `00 6C 00 DB 06 00 C 00 T 06 00 00` | unchanged; DATAPORT does not parse it |
| `09 1F` (data `st 00`) | `00 6C 00 09 06 00 C 00 T 20 st 24` | echo service type; `SIM_SERV_DATA_NOT_AVAIL` |
| `09 19` (data `2F 00`) | `00 6C 00 09 06 00 C 00 T 1A 2F 24` | same |
| `23 04` | `00 6C 00 23 16 00 C 00 T 05 00 01 01 10 00 00 00 00 00 00 00 00 00 00 00 00 00 00` | `[10]=0`, subblock id 1 len 16 zeros; `[0x18]`=0 → "not variant" |
| `1B 0B` (EInfoServer) | `00 6C 00 1B 12 00 C 00 T 0C 00 01 4E 0C 00 06 52 4D 2D 33 32 30 00 00` | optional ("RM-320"); for data `00 01` use id `4D` |
| `31 19`, `31 30` | no reply | not needed for boot |
| `15 02` / `09 C0` / `08 11` / `1C 2D` / `09 E4` | unchanged from part 1 | NOKIATSY's `0x65` parser needs `[10]==[11]`; its `0xC1` parser ignores `0x1C` |

Length bytes: `1B 2A` to NOKIATSY = 10 + 14 = 24 bytes → `len = 18 = 0x12`, i.e. the header is
`00 6C 00 1B 12 00 C 00`; to SAServer = 10 + 22 = 32 bytes → `len = 0x1A`; `1B 2F` = 10 + 18 = 28 →
`0x16`; `0A 0C`/`0A 37`/`0A 01`/`17 01`/`17 0B` = 16 → `0x0A`; `15 79` = 12 → `0x06`; `DB 02` = 24
→ `0x12`; `09 20`/`09 1A` = 12 → `0x06`; `23 05` = 28 → `0x16`; `1B 0C` = 24 → `0x12`. (The
`len_lo` values in the table are computed this way; the `Reply()` helper in `iscpeer.inl` does
it automatically from the payload length.)

Indications: none required beyond part 1. Optional for later (UI coverage display):
`00 6C 00 0A 06 00 C 00 00 1E pct dbm` (`NET_RSSI_IND`) and
`00 6C 00 0A 0A 00 C 00 00 02 01 00 04 06 00` (`NET_REG_STATUS_IND`, nsb at `[11]`) to channels
subscribed to `0A 1E` / `0A 02` (NOKIATSY). If a second `MTC_STATE_INFO_IND` is ever wanted for
NOKIATSY, use action `04` (`01 04`); the DSY needs action `03`.

Control additions: **ctl 22 → return a positive buffer size (e.g. `0x400`)**; **ctl 8 → park
`{status*, TPtr8*}` like ctl 7 (its cancel is one of the no-argument controls
19/20/28/30/31/32/34/36 of part 1 §2.2, not identified; complete the parked request with
`KErrCancel` on channel close)**; everything else as in part 1.

## 5. Things that will block boot and are not a modem reply

- **`Kern::MutexWait` outside a critical section** in `iscpeer.inl` (`Request()` takes
  `iF->iLock` around `Control()`): udeb assertion on every call; must wrap with
  `NKern::ThreadEnterCS()/LeaveCS()` or use an `NFastMutex`/`DMutex` from a thread already in CS.
- **`DATAPORT.CSY` leave → `C32-fault 0`**: cause not in the available log; first suspects are
  ctl 22 returning 0 (buffer sizes) and the pipe/port constructors (`0x70015030`, `0x7001777c`,
  `0x700170b6`, `0x7001663a`, `0x70016994`, `0x70016d66`), which also use the pipe resource `0xD9`
  (`PNS_PIPE_*` requests to the modem will follow once the port is created). Rerun with the kernel
  `REQC`/`IPCRD` traces and grep `ISCPEER: ch 46` to see which of them sends/receives next.
- **FeatMgrServer ↔ SAServer** (§3.2): until `1B 29`/`1B 2E` are answered every
  `FeatureManager` client (SysAp, Phone, …) risks waiting on the feature-info plugin.
- **LanguageSelection** (§3.3): ≈170 s of timeouts without the SIM/PERM replies.

## 6. Open questions

- Exact meaning of `0x17` messages `0x00 (op 3)` / `0x0A (op 2)` and resource `0x32` on this
  firmware; the values chosen only satisfy the parsers.
- The real return value of ctl 22 (`RIscApi` #10) and the names of IscApi exports #10/#13.
- Which S60 feature ids the five `SAFeatureInfoService` bits map to (PP ids `0x0023`, `0x0108`,
  `0x0135`, and the two `0xF000` flags); all are currently reported "off".
- `NET_REG_STATUS` → ETel mapping table at `0x808dd384` was not decoded entry by entry; status 6
  lands in a "not registered" ETel state either way.
- The ETel/custom IPC numbers NOKIATSY completes (20067, 20104, 1000659, 1000661, …) were not
  resolved: no SDK on disk carries the `etelmm` IPC enum.

Helper added: `tools/research/rofs_extract.py` (carve a file out of a ROFS image by name; the
output is the compressed E32 image for `tools/e32load.py`). Decompile of DATAPORT added at
`build/n95/ghidra/DATAPORT.CSY.c` (from `e32load.py --base 0x70014000`).
