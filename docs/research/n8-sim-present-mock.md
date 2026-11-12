# N8 Delight 6.7 (RM-596, Belle): mocking an inserted, ready SIM and a home network

Mock-level research, static only (no runs). Sources: headless Ghidra decompiles of the Delight core
(`build/roms/nokia-n8-rm596-delight/<fw>/ghidra/nokiatsy.dll.c`, `sastartupservice.dll.c`, made with
`tools/ghidra/decompile.sh`), the no-SIM boot log `build/instances/n8-delight-test/serial.log`, the real
N8 Delight's answers without SIM (`build/evidence/phonet/batch-20261111-121736.txt`), and the earlier notes
`docs/devices/nokia-n8-rm596/belle-emergency-only.md`, `docs/devices/nokia-5730-rm465/sim-state.md`,
`docs/devices/nokia-n95-8gb-rm320/n95-isc-isi-protocol-2.md`.
[V] = read in code or in a real-modem capture, [I] = best guess. Offsets are whole-message offsets
([3] resource, [8] tid, [9] msg id, data from [10]); `isimessage` ord20/22/23 = 8/16/32-bit **big-endian**
reads, ord18(msg, from, id, type, &pos) = sub-block search (type 1 = `id8 len8`, type 4 = `id16 len16`).

Message senders in nokiatsy.dll all go through one virtual `Send(obj, resource, tid, msgid, data)` (vtable
slot +0xc of `this+0xc`), e.g. `FUN_80e7bb5e` = `09 1D {svc, 00}` [V]. The TSY registers one handler object per
(resource, msg) with `FUN_80e7b068(table, obj, res, msg)`; the full PN_SIM set it ever expects:
`09:{02,0A,14,24,2C,2E,34,3E,61,6D,74,BB,BD,C1,D5,DD,DF,E8,EF,*}`, PN_MODEM_NETWORK `C8:{01,02,35,37,3F,*}`,
PN_SECURITY `08:{02,03,09,0B,0C,0E,0F,*}`, MTC `15:{65,71,79,C0}` [V].

## 1. The state switch: what says "card present, ready, PIN disabled"

### 1.1 SAStartupService (SAServer, channel 35) - decides KPSSimStatus

- C1 reader `FUN_80e70580`-ish (`sastartupservice.dll.c` lines 1600-1610): `[10]` = service, `[11]` = status
  [V]. (The "byte[12]" in belle-emergency-only.md is this `[11]`: the real no-SIM C1 is
  `00 06 00 ff 00 00` and the 06 is `[11]`.)
- SIM_IND reader (lines 1620-1630): `[10]`, `[11]`; only `[10] == 5` stores `[11]` as the SIM status [V].
- ESIMPresent decision (0x80e71c34): stored status **1, 0x24, 0x4B, 0xFC -> present**; **6 -> no SIM**;
  anything else -> C0 polled again [V].
- SIM-lock check: `C0 01 00` -> C1 `01 01` = ESimLockOk (8/9/10 = lock states) [V, already answered].
- SIM-usable raiser (0x80e71056, runs after every PN_SIM message on channel 35): stored status
  **0x49 or 0x4A -> ESimEvent/ESimUsable -> KPSSimStatus = 101 (ESimUsable)**, which is what the
  SSM policy needs for the normal (non-offline, non-emergency) state; 6 -> SimRemoved/NotReady [V code,
  I that 0x49/0x4A are the "SIM init complete" statuses of SIM_IND service 5].

So for channel 35 the mock must deliver, in this order:
1. `09 C1 {00 01 00 FF 00 00}` to `09 C0 {00 00}` (6-byte layout as the real modem; `[11]` = 01 = SIM_SERV_OK) [V parser, I the 4 tail bytes].
2. `09 C1 {01 01}` to `09 C0 {01 00}` (unchanged).
3. unsolicited `09 EF {05 4A}` on channel 35 after the first C1 (the profile already has
   `[modem].indications` with `channel = 35, after = "09 C0"`): sets the stored status to 0x4A and raises
   ESimUsable [V the raiser's compare; I that a real modem sends exactly 05 4A].
   Ask the real modem: listen on PN_SIM for `09 EF` after boot with a SIM (the two bytes after the msg id).
4. The three `09 1F {svc 00}` language queries (svc 02, 01, 2E) may be answered as today (`svc 24`, data
   not available); a real value would only pick the UI language [I].
5. PN_SECURITY: `08 11` -> `12 {05 00 00}` (startup OK) and `08 67` -> `68 {01 00 00 00 00 00}` (lock info:
   unlocked, disabled) stay as they are [V, from the 5730/N8 notes]. With PIN disabled nothing else is asked.

### 1.2 NOKIATSY (c32exe, channel 7) - "SIM ready" for ETel

- `09 C1` handler object `FUN_80e7d4f8` (registers `08:*, 15:C0/79/65, 09:EF/0A/C1/DF`) [V]. On the N95 the
  C1 parser acts only on `[11] == 1` (marks SIM ready, completes custom IPC 1000659); the Delight parser was
  not isolated (budget) [I same]. `C1 {00 01 ...}` is therefore also right for channel 7.
- SIM_IND parser `FUN_80e7e95e`: reads `[10]` (service, switch), `[11]`, `[12]`, `[14]` and dispatches on the
  service [V]. N95 mapping of the same reads: `(0x0D, 0x0B SIM_SERV_INIT_OK, [12] == 8)` -> SIM ready;
  `(7, 0x4F)` -> data available; `(5, 6)` -> reset flags; `(1, 2)` -> PIN info at `[14]` [V on N95, I here].
  The TSY reads up to `[14]`, so a 2-byte indication makes it panic (`IsiMessage OFFS -9`, seen on the 5730):
  the channel-7 indication must carry >= 5 data bytes.
  Proposed for channel 7: `09 EF {0D 0B 08 00 00}` once, after the C1 exchange [I].
  Ask the real modem: the same `09 EF` listen as above (the modem sends several, one per service).
- `09 0A` (reply to `09 09 {97 00}`): keep `97 20`; `[10] == 0x9B` re-sends the request (do not use) [V].
- `09 DF` (reply to `09 DE` svc 1, sb 0x0190): keep `01 01 00 00 00 01 01 90 00 08 01 00 00 00` [V].

## 2. PN_SIM requests of the TSY and SAServer in a SIM-present start-up

Builders found in nokiatsy.dll (resource 9, `msg {svc, 00 ...}` unless noted) and what the parsers read.
Today every one of them is answered with `svc 24` (data not available) or `svc FB` (no card); none blocks
start-up (NOKIATSY has no request timers [V N95, I N8]; the SAServer waits only on C0/EF). With a SIM each
turns into an ETel completion, so a wrong value shows as a wrong IMSI/operator/name, not as a hang.

| Request | Built by | Response and parse | Value for the test SIM | Blocks? |
|---|---|---|---|---|
| `09 1D {2D 00}` READ_IMSI (tid 6) | `FUN_80e7bb5e` | `1E`: `[10]` svc (ignored), `[11]` status, **1 = OK**; then 9 bytes at `[12..20]` in 3GPP EF_IMSI coding: `[12]` = length (8), `[13]` high nibble = digit 1 (low nibble = parity, skipped), `[14..]` BCD low nibble first, stops at 0xF or after `[12]` bytes (`FUN_80e7ce4a`) [V]. Status != 1 -> ETel error from `FUN_80e9ee86(1, status)`. | `1E {2D 01 08 09 10 10 10 32 54 76 98}` = IMSI 001010123456789 [V coding] | no (GetSubscriberId; asked 9x at boot) |
| `09 13 {svc 00}`, svc 1A, 74, 38, 28 (boot) and 17 / 15 (`FUN_80e7bacc`: 23 / 21 entries) | `FUN_80e7ba94` | `14`: dispatched on **tid `[8]`** 0/1/2 -> `FUN_80e7c8e8` / `FUN_80e7c6aa` / `FUN_80e7c560`; the 0x17/0x15 variants fill per-service capability bit masks (bytes `this+0x25..0x29`, codes 7,0xB,0x13,0x14,0x1A,0x1B,0x1D,0x25,0x28,0x73,0x74) = a **SIM service table** read [V reads, I meaning]. Layout of the data not read (budget). | ask the real modem: `09 13 17 00`, `09 13 15 00`, `09 13 1A 00`, `09 13 74 00`, `09 13 38 00`, `09 13 28 00` | no |
| `09 21 {2C 00}` | `FUN_80e7ba5c` | `22`: `[11]` status, `[10]` 0x2C -> `FUN_80e7c1d4`, 1 -> `FUN_80e7c084`; completes IPC 0x5600 with two 16-bit strings (`this+0x5c`, `+0x84`) [V] = operator/service-provider **name strings** [I: SPN "qembian"] | ask: `09 21 2C 00` | no |
| `09 29 {35 00}` and `09 29 {36 00 <0/1> 01 00 00}` | `FUN_80e7bc0a` | `2A`: svc 0x35, `[11] == 1` -> `[12]` 0 -> 1, 1 -> 0, else 2 (a mode flag, stored via `FUN_80e8214e`); svc 0x36: `[11] == 0xFB` -> -2 else OK [V] | `2A {35 01 00 00 00 00}`; `2A {36 01}` [I] | no |
| `09 31 {3B 00 00}` | `FUN_80e7bbc6` | `32`: `[11] == 1` -> u32 at `[12]` and 60 bytes at `[16..]` into an etelmm (ord60) record [V reads; I: own numbers / MSISDN record] | ask: `09 31 3B 00 00` | no |
| `09 2D {04 00}` (x2 at boot) | line 7447/8880 | `2E` -> `FUN_80e80f7a` (not read) | ask: `09 2D 04 00` | no |
| `09 23 {25}` (ICCID-like, 1-byte data) | 19353/20055 | `24` [I: ICCID 10 BCD bytes] | ask: `09 23 25` | no |
| `09 60 {FA 00}`, `09 62 {4D 01 01}`, `09 6C`, `09 D4`, `09 D8`, `09 3D`, `09 3F`, `09 33`, `09 2B`, `09 BA` (14-byte), `09 BC`, `09 DC` (phonebook, sb 0xE4), `09 E4`, `09 C8 {svc 00}` (sent once, `FUN_80e7b5fc`) | various | not read (budget); today `FB`/`24` answers are accepted (error completions) | keep the no-card answers first; ask the real modem for each request as logged | no |
| ch 35 `09 1F {02 00}`, `{01 00}`, `{2E 00}` | SAStartupService | `20 {svc, status}`; status 0x24 accepted today | ask: `09 1F 02 00` | no |

PIN/PUK: the TSY sends PN_SECURITY `08 0D` and `08 10` (lines 31647/31665) only on ETel requests
(VerifySecurityCode/GetLockInfo); with PIN disabled the start-up path asks `08 67` (lock info) only, answered
[V]. "PIN disabled" = no PIN query from the modem and SIM_IND `(1, 2)` never sent [I].

Request bytes above are ready for `tools/phonet_query.py` (`RES MSG DATA...`); record the answers in
`build/evidence/phonet/` and copy only the formats here.

## 3. Network: registered on the home network

Handlers in nokiatsy.dll for PN_MODEM_NETWORK 0xC8 (same message numbering as PN_NETWORK 0x0A on the N95):

- **NET_MODEM_REG_STATUS_GET_RESP (C8 02) and _IND (C8 01)**. `FUN_80e92f56` (msg 1 or 2): looks for sub-block
  **0x39**, else **0x09** (type 1, from `[12]`); reads `u16 @sb+2` (LAC), `u32 @sb+4` (cell id), 3 bytes `@sb+8`
  (MCC/MNC BCD) and raises a cell-info change when any differs [V reads, I names]. The registration status
  itself comes from sub-block **0x00 NET_REG_INFO_COMMON** `@sb+2` through a 12-entry map (N95 `FUN_808dd3c0`;
  Delight's copy not isolated, budget) [V N95, I N8]; `[10]` = cause (0 = OK), `[11]` = number of sub-blocks.
  Status values (names only, oFono `network.h`): 0 HOME, 1 ROAM, 3 NOSERV, 4 SEARCHING, 5 NOT_SEARCHING,
  6 NOSERV_NOSIM, 8 POWER_OFF, 9 NSPS, 0x0A NSPS_NO_COVERAGE, 0x0B SIM_REJECTED [ref].
  Proposed reply (and the same bytes as an indication `C8 01` once after the first GET):
  ```
  00 02  00 04 00 00            cause OK, 2 sub-blocks; NET_REG_INFO_COMMON: status 0 = HOME, selection 0
         09 0C 00 01 00 00 00 01 00 F1 10 00   NET_GSM_REG_INFO: LAC 0x0001, CI 0x00000001, PLMN 001/01 (BCD 00 F1 10), pad
  ```
  [I layout of both sub-blocks beyond the fields read: ask the real modem `C8 00` with a SIM registered, the
  reply shows the real sub-block set and lengths].
- **NET_RAT_RESP (C8 37)** `FUN_80e83564`: sub-block **0x2C** `@sb+2`: 1 = GSM, 2 = UMTS [V]. Today
  `00 01 2C 04 01 00` (GSM); for WCDMA send `00 01 2C 04 02 00`.
- **C8 3F** `FUN_80e83238`: sub-block **0x3C** `@sb+2 == 1` -> a boolean notification (not the RSSI) [V reads, I meaning].
- **C8 35** (handler registered, parser not read) and **RSSI**: the TSY's `C8 0B {00 02}` (NET_RSSI_GET) is answered
  by the real modem with `F0 {01 0B ...}` = not supported [V phonet], and no `0C` handler is registered, so
  signal bars do not come from PN_MODEM_NETWORK 0x0B. Candidates: `C8 35`/`C8 3F` indications, or the
  0xC9 resource (`FUN_80e7f548` handles `C9 18/1A/1B/1C`; `FUN_80e891c6` `C9 0A/12/13`) [I]. Ask the real modem:
  listen on 0xC8/0xC9 after registration (indications), and `C9 ...` is probably the cell/RSSI resource.
- Operator name: `C8 1D`, `C8 1E`, `C8 20`, `C8 27`, `C8 43` have parsers (`FUN_80e83540`, `FUN_80e834c8`,
  `FUN_80e832d4`, `FUN_80e8333e`) in the SIM-services object; `FUN_80e857a2` (`[10] == 0`, sub-block 0 type 1,
  u32 length `@sb+8`, string `@sb+12`) is a name read [V reads, I which request]. Not needed for "registered";
  the home-screen operator text may use the SPN from PN_SIM (`09 21`) first [I].
- NITZ/time zone: not found in the budget (no `C8` time handler isolated); skip.
- MTC: `15 16` -> `79 {00 00}` (RF normal) and `15 02` -> `65 {0A 0A}` as today: no change needed for
  registration [V].

## 4. Proposed QEMU shape

New `src/qemu-syborg/hw/arm/syborg_sim.c` (or a section of `syborg_modem.c`), called from the PN_SIM branch
before the canned table:

```c
typedef struct { uint8_t present, pin_enabled; char imsi[16], iccid[21], spn[17], msisdn[21];
                 uint8_t plmn[3]; uint8_t lang; } VirtSim;          /* from board config sim.* */
typedef struct { uint8_t req; void (*fn)(SyborgModem *, int ch, const uint8_t *m, int len, uint8_t tid); } SimHandler;
static const SimHandler sim_handlers[] = {
    { 0xC0, sim_status },      /* C1 {svc, 01, 00 FF 00 00} (svc 0) / {01, 01} (svc 1) when present, else today's 06/01 */
    { 0x1D, sim_read_imsi },   /* 1E {2D 01 <9 EF_IMSI bytes>} */
    { 0x13, sim_read_table },  /* 14: service table - after the real-modem capture */
    { 0x21, sim_read_names },  /* 22: SPN strings - after the capture */
    { 0x29, sim_mode_flags },  /* 2A {35 01 00..} / {36 01} */
    { 0x23, sim_read_iccid },  /* 24: after the capture */
    { 0x1F, sim_language },    /* 20 {svc 24} today */
    /* 09 09, DE, E4 stay as they are */
};
```
- `sim_present == 0` keeps every current no-SIM answer (`id + 1 {svc, FB}` etc.) so other profiles do not change.
- IMSI -> EF_IMSI bytes: `len = 8; b[0] = (d[0] << 4) | 9 (odd parity, 15 digits); then pairs low-first`.
- PLMN BCD helper for `sim.mcc`/`sim.mnc` -> 3 bytes (`00 F1 10` for 001/01).
- Indications, per channel (the existing `modem.ind` syntax `RRMMDATA@CH>RRMM`): ch 35 `09EF054A@35>09C0`,
  ch 7 `09EF0D0B080000@7>09C0`; plus `C801...` (the GET reply bytes) `@7>C800`.
- Network answers from `net.*` keys through the same table on resource 0xC8: `00 -> 02` (above), `36 -> 37`
  with `net.rat` (1/2), `C8 01` indication.

Board-config keys (`[board.sim]` / `[board.net]` in the instance or profile; `sim.*` per instance so a phone keeps its IMSI):
`sim.present = true`, `sim.imsi = "001010123456789"`, `sim.iccid = "8900101234567890123"`, `sim.spn = "qembian"`,
`sim.mcc = "001"`, `sim.mnc = "01"`, `sim.msisdn = ""`, `sim.pin = ""` (empty = disabled), `sim.lang = 0x01`;
`net.reg = "home"`, `net.lac = 1`, `net.ci = 1`, `net.rat = 2`, `net.rssi = 5`, `net.oper = "qembian"`.
`modem.simst` keeps working as an override.

## 5. Gaps (ask the real modem with a SIM inserted, `tools/phonet_query.py`)

1. `09 EF` indications after boot (listen): the service/status pairs, especially service 5 (expected 0x4A) and 0x0D.
2. `09 C0 00 00` with a SIM: the full 6-byte C1.
3. `09 1D 2D 00`: confirms the EF_IMSI layout (the parser is [V], the surrounding bytes are not).
4. `09 13 17 00`, `09 13 15 00`, `09 13 1A 00`, `09 13 74 00`, `09 13 38 00`, `09 13 28 00`, `09 21 2C 00`,
   `09 29 35 00`, `09 29 36 00 01 01 00 00`, `09 31 3B 00 00`, `09 2D 04 00`, `09 23 25`, `09 60 FA 00`,
   `09 62 4D 01 01`, `09 1F 02 00`.
5. `C8 00` (registered), `C8 36 00`, and a listen on `C8`/`C9` for the status, RSSI and name indications.
