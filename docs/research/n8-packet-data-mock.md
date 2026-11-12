# N8 Delight 6.7: packet data through the fake BB5 modem (mock plan)

Scope: from "GPRS attach" to IP packets flowing, for `nokia-n8-rm596-delight` (Symbian Belle). SIM and
network registration are assumed to work (other agent). Just enough to mock; tags: **[V]** seen in code,
logs or the ROM, **[R]** naming reference (oFono `isimodem/gpds.h`, Linux phonet `pep.h` ids, no code
copied), **[I]** inferred/best guess. Gaps end with "ask the real modem: …" (`tools/phonet_query.py --raw`).

Evidence used: `build/instances/n8-delight-test/serial.log` (no-SIM boot), core ROM modules
`nokiatsy.dll`, `dataport.csy`, `bcatoisc.dll`, `rawip.dll`, `pdp.dll` (`tools/xipdis.py`), the ROM
CommsDat `rofs1 Private/10202BE9/cccccc00.cre`, SF `telephonyprotocols/rawipnif`,
`docs/devices/nokia-n8-rm596/isa-access.md`, `docs/devices/nokia-n95-8gb-rm320/n95-isc-isi-protocol-2.md` 3.4.

## 0. What the firmware already does at boot (no SIM) [V, serial log]

| t | ch | bytes after the 10-byte ISI header ([3] res, [8] tid, [9] id) | meaning | today |
|---|---|---|---|---|
| 2.31 | 7 (nokiatsy) | `DB 01` `00 00 00 00 00 31 00 00 00 31` | PNS_NAME_QUERY_REQ for GPDS (0x31) | answered: dev 00 obj 33 |
| 2.63 | 46 (dataport.csy, port `DATAPORT::0`) | `DB 05` `00 00 00 0A 01 D9 6C 2E 80 00` | PNS_NAME_ADD_REQ: name `0x000A01D9` = (dev 6C, obj 2E) = DATAPORT's PEP | answered `06 00 00` |
| 2.63 | 7 | `D9 00` `01 02 6C 2E 01 00 00 31 04 00` | PNS_PIPE_CREATE_REQ (see 1.5) | **unanswered** |
| 2.63 | 7 | `31 19` / `31 30` | GPDS_STATUS_REQ / GPDS_CONFIGURATION_INFO_REQ | answered (replies table) |
| 17.5 | 7 | `31 11` `00 FF FF 00 00 00` | GPDS_CONFIGURE_REQ {attach mode 00 = manual, MT act FF, class C FF, AOL 00, fill} [R] | answered `12 01` (OK) |
| 18.3 | 7 | `31 06` `FE 21 00 00 00 03` `05 04 00 00` `00 04 00 00` `F9 04 00 00` | GPDS_CONTEXT_CONFIGURE_REQ: cid FE (default/all [I]), PDP IPv4, type normal, primary 00, fill, 3 empty sub-blocks (APN 05, COMP 00, Nokia F9) | **unanswered** |

Channel 46 is opened by `dataport.csy` for port 0 (ISC channel 0x2E, obj 0x2E) [V N95 code, N8 log].
The TSY's GPDS object keeps (dev, obj) of the GPDS server from the name query (defaults 00/33) [V N95].

## 1. One context, control plane (resource 0x31 PN_GPDS, channel 7)

Message ids [R gpds.h]: LL_CONFIGURE 00/01, CONTEXT_ID_CREATE 02/03 (+IND 04, ID_DELETE_IND 05),
CONTEXT_CONFIGURE 06/07, CONTEXT_ACTIVATE 08/09 (+IND 0A), CONTEXT_DEACTIVATE 0B/0C (+IND 0D),
CONFIGURE 11/12, ATTACH 13/14 (+IND 15), DETACH 16/17 (+IND 18), STATUS 19/1A, TRANSFER_STATUS_IND 1E,
ACTIVATE_FAIL_IND 1F, LL_BIND 20/21, CONTEXT_STATUS 22/23 (+IND 24), ACTIVATING_IND 25, MODIFY 2A/2B/2C,
ATTACH_FAIL_IND 2D, DEACTIVATING_IND 2F, CONFIGURATION_INFO 30/31/32, CONTEXT_AUTH 33/34,
RADIO_ACTIVITY_IND 37, CONTEXTS_CLEAR 3A/3B. Status byte: GPDS_ERROR 00, **GPDS_OK 01**, GPDS_FAIL 02 [R;
consistent with our `12 01` reply being accepted]. Sub-block ids (type 1: `id8 len8 data… pad to 4`):
COMP_INFO 00, QOS_REQ 01, QOS_MIN 02, QOS_NEG 03, PDP_ADDRESS 04, APN 05, QOS99_REQ/MIN/NEG 06/07/08,
TFT 09/0A, USER_NAME 0B, PASSWORD 0C, PDNS_ADDRESS 0D, SDNS_ADDRESS 0E, CHALLENGE 0F, DNS_ADDRESS_REQ 90 [R].
PDP types: IPv4 0x21, IPv6 0x57, PPP 0x01; link types: LL_PLAIN 02; attach status 00/01 [R].

The TSY (nokiatsy, N95 registration list `31:{0D,37,3B,*}` [V N95]) handles every GPDS message through
a wildcard handler; the requests it sends on a connect are not in any log yet (no SIM). The layouts
below are oFono's for the same resource [R/I]; **capture the real ones once a SIM is present**
(`sydb strace`/`ISCPEER: ch 7 send`) and fix the model from them.

1.1 Attach (may be implicit: attach mode is manual, so the pdp layer calls `RPacketService::Attach`
or the modem attaches on the first activation [I]):
- req `31 13` `{attach_type 00, fill}` -> resp `31 14` `{01 OK, 01 ATTACHED, 00 cause}` [I] and
  ind `31 15` `{01 ATTACHED, cid FF?}` [I]. GPDS_STATUS_RESP `31 1A` [10] = attach status (today 00; make it
  01 once attached), rest as today [I]. ask the real modem: `31 19` with SIM in and data on, `31 13 00 00`.

1.2 Context id: req `31 02` (no data) -> resp `31 03` `{cid, 01 OK}` (cid 01..) + ind `31 04` `{cid}` [R/I].

1.3 Link layer (binds the context to DATAPORT's pipe): req `31 00` `{cid, pipe_handle, 02 PLAIN}` ->
`31 01` `{cid, 01}` [R]; some firmwares use `31 20` LL_BIND `{cid, pipe_handle}` -> `31 21` instead [I].
Accept both; remember cid -> pipe handle -> data channel.

1.4 Configure: req `31 06` `{cid, 21, 00 normal, primary_cid, fill, nsb, sub-blocks…}` with APN
`05 len apn_len 'internet' pad` (expect APN from the IAP; accept anything, log it), optional
USER_NAME 0B / PASSWORD 0C (same shape; CHAP/PAP are `GPDS_CONTEXT_AUTH 33/34` on some firmwares [I]),
DNS_ADDRESS_REQ `90 04 00 00` (asks for DNS in the activate ind), COMP_INFO `00 04 hdr data`, QoS99 06/07
(ignore) -> resp `31 07` `{cid, 01}` [R]. The boot-time `FE …` request: answer `31 07` `{FE, 01}` [I].

1.5 Pipe (resource 0xD9 PN_PIPE, ids [R pep.h]: PIPE_CREATE 00/01, REMOVE 02/03, RESET 04/05, ENABLE 06/07,
REDIRECT 08/09, DISABLE 0A/0B, PIPE_DATA 20, ALIGNED_DATA 21, PEP_CONNECT 40/41, PEP_DISCONNECT 42/43,
PEP_RESET 44/45, PEP_ENABLE 46/47, PEP_CTRL 48/49, PEP_DISABLE 4A/4B, PEP_STATUS_IND 60, PIPE_CREATED_IND 61,
PIPE_RESET_IND 63, PIPE_ENABLED_IND 64, PIPE_REDIRECTED_IND 65, PIPE_DISABLED_IND 66).
- Observed create (ch 7, tid 2E): `D9 00` `01 02 | 6C 2E 01 00 | 00 31 04 00` = {state_after 01 ENABLE,
  priority 02, pep1 dev 6C obj 2E type 01, fill, pep2 dev 00 obj 31 type 04 PN_PEP_TYPE_GPRS, fill} [I layout,
  V bytes]. pep1 is DATAPORT's registered name (1.0), pep2 the GPDS PEP on the modem. Note obj 31 here
  while the name query answered 33: use whatever the request says.
- Reply `D9 01` `{pipe_handle, 00 NO_ERROR, 00 pep1_error, 00 pep2_error}` [I]; handle 01.. (not FF: dataport
  treats FF as "handle not yet known" [V 0x80e4f308]).
- Real pipe controllers also talk to each PEP: `D9 40` PNS_PEP_CONNECT_REQ to (6C,2E) on **channel 46**
  `{pipe_handle, state_after, other_pep_type 04, fill, nsb, sub-blocks}` -> dataport answers `D9 41`
  `{pipe_handle, 00, …}`; `D9 46` PEP_ENABLE_REQ -> `47`; later `4A`/`42`. dataport.csy has two PN_PIPE parsers
  [V 0x80e4f29a, 0x80e4f2f6]: one stores `[10]` as the pipe handle when its own is FF, requires `[11] in
  {01, 0D}`, keeps `[12]`, scans type-1 sub-blocks from offset 14 for id 0D; the other reads `[12]`, requires
  `[11] in {00, 01, 0D}`, scans from 15 for id 01. Which message ids reach them is not read (no literal
  0x40..0x66 in the code: ids come from a table/isimessage.dll). Whether DATAPORT refuses `RComm::Open`
  or `Write` until the PEP handshake happened is **unknown** [I: likely it only needs the handle for its
  own PEP_STATUS_IND flow control]. ask the real modem: register the PC as a PEP (`--raw db 05 00 00 00 0a
  01 d9 10 2e 80 00`), then `--raw d9 00 01 02 10 2e 01 00 00 31 04 00 --wait 3`: the reply gives the
  CREATE_RESP layout and the PEP_CONNECT_REQ (what arrives at obj 2E), then `--raw d9 06 <handle> 00`.
- Enable/remove: `D9 06` `{handle, fill}` -> `D9 07` `{handle, 00}`; `D9 02` `{handle, fill}` -> `D9 03`
  `{handle, 00}` [R/I]. oFono enables the pipe after the context is active; the N8 TSY creates it at boot with
  state_after = ENABLE, so data may flow right after activation without an explicit enable [I].

1.6 Activate: req `31 08` `{cid, nsb, [sub-blocks]}` -> resp `31 09` `{cid, 01, nsb 00}` [R/I]; then the
indications the TSY completes the ETel activation on [I, oFono does the same]: `31 25` ACTIVATING_IND
`{cid}`, then `31 0A` CONTEXT_ACTIVATE_IND `{cid, nsb, sub-blocks}` with
- PDP address `04 08 00 04 0A 00 02 0F` (id, len, fill, addr_len 4, 10.0.2.15),
- primary DNS `0D 08 00 04 0A 00 02 03`, secondary DNS `0E 08 00 04 0A 00 02 03`,
- optionally `08` QOS99_NEG (copy from the request or omit), `00` COMP_INFO `00 04 00 00`.
  Address sub-block shape (addr_len at +3, bytes from +4) is from memory of oFono [I]; ask the real modem
  (activation on the owner's phone: `31 08 <cid> 00` after 1.2/1.4, or just read an `0A` from a trace).
- Also send `31 1E` TRANSFER_STATUS_IND `{01 AVAIL, 00}` and `31 24` CONTEXT_STATUS_IND `{cid, 01?}` [I] (the
  "G"/"3G" data indicator); `31 37` RADIO_ACTIVITY_IND `{01}` is optional (TSY handles it [V N95]).
- Failure path: `31 1F` ACTIVATE_FAIL_IND `{cid, cause}`.

1.7 Deactivate: `31 0B` `{cid, fill}` -> `31 0C` `{cid, 01}` + `31 0D` DEACTIVATE_IND `{cid, cause 00}` [R/I];
pipe `D9 02` remove. Detach `31 16` `{detach_type, fill}` -> `31 17` `{01, 00}` + `31 18` `{cause}`.
`31 3A` CONTEXTS_CLEAR -> `31 3B` `{01}`.

## 2. Data plane: how IP packets reach the modem on this ROM

CommsDat defaults in the ROM [V `rofs1 Private/10202BE9/cccccc00.cre` strings]: ModemBearer records
"Default Modem" {NIF PPP, port `COMM::0`, TSY MM, CSY ECUART}, **"GPRS Modem" {NIF `spud-rawip`, port
`DATAPORT::0`, TSY PHONETSY, CSY DATAPORT}**, "CSD Modem" {PPP, `DATAPORT::0`, PHONETSY, DATAPORT}; the BcaStack
column holds one value `BcaToIsc`. `bcatoisc.dll` (CBcaToIsc, ncp `modemadaptation/bcaiscadapter`) parses
its port name as a decimal ISA channel number (optionally after a 4-char prefix, literal `Int_`) and
opens that channel with iscapi #34 [V 0x807ea03a..0c8], so it cannot be the BCA of a `DATAPORT::0` record;
the packet-data BCA on this ROM is C32Bca -> RComm `DATAPORT::0` -> dataport.csy [I; check the instance's
persisted `C:\private\10202be9\persists\cccccc00.cre` if the connection fails before any ISA traffic].

Path, top down:
1. tcpip -> `pdp.dll` (PDP MCPR/SCPR: drives `RPacketContext` SetConfig/Activate on ETel = nokiatsy's GPDS
   code in 1.) -> `rawip.dll` = SF **rawipnif v1** (class names CRawIPFlow, CBcaIoController, CIPv4Binder; no
   CIPTagHeader, so `RAWIP_HEADER_APPENDED_TO_PACKETS` is off: **no 2-byte protocol tag**) [V strings].
   One IPv4 datagram per BCA Write, <= 1500 bytes (`KMaxIPPacket`, MTU 1500) [V SF].
2. BCA Write -> dataport.csy / bcatoisc -> iscapi **DataSend(const TDesC8&)** = ISA ctl 14 (sync, also the
   async form ctl 4), receive with **DataReceive** = ctl 5 into a buffer of `MaximumDataSize()` (ctl 18,
   our stand-in says 4096), flow control via `NotifyFlowControlStatus` (ctl 7) / `FlowControlStatus` (ctl 16)
   [V bcatoisc imports #28/#3/#13/#14/#15/#18; dataport.csy imports #28/#3/#13/#15/#18/#10]. Both clients do
   this on the port's own channel: **channel 46 (0x2E) for DATAPORT::0** [V N95 code; N8 log]. The pipe handle
   is never passed to the driver by either client (no custom functions used) [V imports], so the ISA kernel
   driver (not ours) binds data frames to the pipe; for the mock, **channel = context**.
3. Payload of DataSend: expected a bare IPv4 datagram (rawip adds nothing; whether dataport.csy prepends a
   `PNS_PIPE_DATA` ISI header `00 00 6C D9 len.. obj obj tid 20 handle` before DataSend was not read) [I].
   Detect at run time: first byte 0x45 = bare IP, 0x00 = ISI-framed (then the pipe handle is at [10] and the
   datagram at [11], per Linux pep: PIPE_DATA = `utid, 0x20, pipe_handle, data…`) [R].
4. Back: one datagram per DataReceive completion, same framing as seen on send; MTU 1500; flow control
   never off (park NotifyFlowControlStatus, ctl 16 -> 0) [V stand-in today].

Sizes today: guest stand-in `KMaxMsg = 256` (`iscpeer.inl`), modem `MODEM_MAX_MSG 256`, `MODEM_BUF 0x800`,
`ModemFrame.data[256]`: a data path needs >= 1500 (+11) bytes per frame on both sides.

## 3. QEMU side: proposed shape

New file `src/qemu-syborg/hw/arm/syborg_modem_gpds.c` (called from `modem_request()` for res 0x31/0xD9 and
from a new TX kind), keeping `syborg_modem.c` as the transport.

```c
/* mailbox: TX kind 5 = DATA (channel, payload); RX kind 3 = DATA (completes the guest's DataReceive) */
enum { TX_DATA = 5 };  enum { RX_DATA = 3 };
#define MODEM_MAX_MSG 2048           /* ISI + data frames; MODEM_BUF 0x1000, window stays 0x2000 */

typedef struct GpdsPipe { uint8_t handle, state /*0 free 1 created 2 enabled*/, pep1_dev, pep1_obj, pep2_obj; } GpdsPipe;
typedef struct GpdsCtx  { uint8_t cid, state /*0 free 1 created 2 configured 3 active*/, pipe, pdp_type;
                          char apn[64]; uint32_t ch_ctl; /* channel of the TSY (replies/inds) */ } GpdsCtx;
typedef struct GpdsState {
    uint8_t attached;  GpdsPipe pipe[4];  GpdsCtx ctx[4];
    uint32_t data_ch;                  /* guest channel key of DATAPORT::0 (the one that NAME_ADDs 0x000A01D9) */
    uint8_t framed;                    /* 1 once a DataSend started with 0x00 (ISI-framed PIPE_DATA) */
    NICState *nic; NICConf conf;       /* -nic user,model=syborg-modem: its own slirp */
    uint8_t ip[4], dns[2][4], gw_mac[6], my_mac[6];
} GpdsState;

bool gpds_request(SyborgModemState *s, uint32_t ch, const uint8_t *m, uint32_t len);   /* res 0x31 */
bool pipe_request(SyborgModemState *s, uint32_t ch, const uint8_t *m, uint32_t len);   /* res 0xD9 */
void gpds_data_tx(SyborgModemState *s, uint32_t ch, const uint8_t *p, uint32_t len);   /* TX_DATA */
static ssize_t gpds_net_receive(NetClientState *nc, const uint8_t *buf, size_t size);  /* slirp -> guest */
```
- `gpds_request`: state machine of section 1 (ids 02/00/20/06/08/0B/13/16/19/3A -> resp + inds); every
  unknown GPDS request gets `id+1 {cid?, 01}` and a log line rather than silence.
- `pipe_request`: `00` -> allocate handle, reply `01 {h,0,0,0}`, then (config `modem.gpds_pep=1`) send
  `D9 40 {h, 01, 04, 00, 00}` on `data_ch` and expect `41` on it; `06/02/0A` -> `+1 {h, 0}`; `60` from DATAPORT
  (flow control) -> ignore; `41/47/4B/43` -> ignore.
- Name service: `modem_request` already answers `DB 05`; also record (name -> channel key) so `data_ch` is the
  channel that added `0x000A01D9` (and name queries for 0x31 keep the table reply).
- Bridge: `-nic user,model=syborg-modem` (same pattern as `syborg_net.c`: `qemu_new_nic`, `.receive`,
  `.can_receive`; added by `tools/device.py qemu-args` when `[board.modem] gpds = true`). Slirp speaks Ethernet:
  on TX wrap the datagram in `dst 52:55:0a:00:02:02 (slirp gateway MAC = 52:55 + 10.0.2.2), src my_mac, 0x0800`;
  on RX accept ethertype 0x0800 to our IP (strip 14 bytes, push RX_DATA on `data_ch`, framed as the guest
  sends), answer ARP who-has 10.0.2.15 with our MAC, drop the rest. Two slirp instances (Ethernet NIC and
  modem) are independent 10.0.2.0/24 networks: fine, the phone sees one IAP at a time.
- Board config (`[board.modem]` -> `modem.*` in the config string, read with `modem_config()`):
  `gpds_apn = "internet"` (log a mismatch, still accept), `gpds_ip = "10.0.2.15"`, `gpds_dns = "10.0.2.3"`,
  `gpds_dns2 = ""`, `gpds_pep = 1` (PEP handshake with DATAPORT), `gpds_frame = "auto|ip|isi"` (data framing).
- Guest stand-in (`iscpeer.inl`): ctl 14/4 DataSend -> TX_DATA frame (no reply); ctl 5 DataReceive -> park and
  complete from RX_DATA frames (queue them like ISI frames, size up to 4096); log `ISCPEER: ch N data N bytes`
  for the first frames; `KMaxMsg` 2048 for data queue entries.

Order of implementation, with the check at each step:
1. Frame sizes (guest + modem) and TX_DATA/RX_DATA plumbing with a log line; smoke:
   `tests/device-smoke.sh nokia-n8-rm596-delight` (nothing else changes).
2. Answer today's unanswered messages: `D9 00` -> `01 {01 00 00 00}`, `31 06` -> `07 {cid, 01}`; check the
   serial log has no `no reply for resource d9/31` and `c32exe`/dataport do not die (`sydb catch`).
3. With SIM + registration from the other agent: Settings > Connectivity > Internet AP (packet data,
   APN internet), browser "Go": capture the real ch 7 sequence (`sydb log --grep 'ch 7'`), fix layouts of
   1.1-1.6 from it, implement the GPDS state machine; check: context reaches active (ETel) = `ISCPEER: ch 46
   data` lines appear (rawip's first DNS query).
4. Bridge to slirp; check with `-object filter-dump` on the modem NIC (pcap of what the phone sends),
   `python3 -m http.server` on the host and `http://10.0.2.2:8000/` in the browser, then a public URL.
5. Deactivate/detach/pipe remove when the connection closes; flow control stays "on"; secondary contexts
   (spud, `cont_type 02`) and IPv6 are out of scope.

## 4. Open questions (ask the real modem)

- Pipe create reply and what the controller sends to the APE PEP (1.5): the two `--raw` lines above.
- GPDS_CONTEXT_ACTIVATE_IND sub-block layout (PDP/DNS address bytes) and GPDS_STATUS_RESP when attached:
  `--raw 31 19`, and a trace of one real activation (`31 02`, `31 06 …`, `31 08 …`) on the owner's phone.
- Whether dataport.csy frames DataSend as PNS_PIPE_DATA or sends bare IP: visible in our own log at step 3
  (no real-modem query needed).
- The persisted CommsDat of the instance (BcaStack of "GPRS Modem"): bcatoisc would mean a numeric port
  name and a different channel; the mock keys on the channel that NAME_ADDs `0x000A01D9` either way.
