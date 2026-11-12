# ISI (Nokia BB5 modem) protocol: external public references, mock-level

Compiled 2026-11-11 from public sources for the fake modem (`src/qemu-syborg/hw/arm/syborg_modem.c`,
`platform/isi/replies-bb5.txt`). GPL sources (oFono, Wireshark plugin) are naming/layout references only;
tables below are rewritten, no code copied. Tags: [V] = read in the cited source (or seen in our own 701
trace), [I] = inferred/guessed.

Byte notation: whole-message offsets ([3] resource, [8] tid, [9] msg id, data from [10]); `data[n]` in the
oFono sources = our `[10+n]`. Sub-blocks of PN_UICC, PN_SMS (new ids) are `id16 len16`; PN_SIM, PN_NETWORK,
PN_GPDS, PN_CALL, PN_INFO are `id8 len8` (length includes the header) [V oFono request builders].
Reply-table notation: `reply RR QQ PP data..` = resource, request id, response id, data from [10].

Sources (all [V] unless stated):
- oFono 1.7 isimodem headers/drivers (elixir mirror): `https://elixir.bootlin.com/ofono/1.7/source/drivers/isimodem/`
  `uicc.h`, `uicc.c`, `sim.h`, `sim.c`, `network.h`, `network-registration.c`, `gpds.h`, `gprs.c`, `sms.h`,
  `call.h`, `mtc.h`, `info.h`; plugins `n900.c` (MTC), `u8500.c` (MCE). Newer netreg parser:
  `https://raw.githubusercontent.com/rilmodem/ofono/master/drivers/isimodem/network-registration.c`.
- Wireshark in-tree dissector `epan/dissectors/packet-isi.c` (resource/SIM/network tables):
  `https://raw.githubusercontent.com/wireshark/wireshark/master/epan/dissectors/packet-isi.c`.
  Older plugin with more resources (mtc, ss, radiosettings): `https://git.ring0.de/sre/isi-wireshark-plugin`
  (TLS cert broken at `sre.ring0.de`, not read).
- Background: Replicant wiki "OtherModemProtocols" (Nokia published ISI headers in an N900 SDK, later
  withdrawn), maemo thread cataloguing N900 binaries vs ISI messages (`talk.maemo.org/showthread.php?p=1545825`),
  Debian `libgisi` (freesmartphone.org low-level ISI lib). Symbian's own modem adaptation (nokiatsy, LTSY) is
  **not** on the SymbianSource GitHub mirror (only stubs/Qemu/graphics adaptations) [V listing].

## 0. Common

- ISI header as Wireshark sees it (after the media byte): recv dev, send dev, resource, len16 BE, recv obj,
  send obj, tid; devices 0x00 modem, 0x6C host, 0xFF any [V]. Our serial trace prints the length little-endian
  after the resource (`8c 2a 00` = 42) [V trace].
- Message id 0xF0 = COMMON_MESSAGE: [10] = sub id; 0x12 COMM_ISI_VERSION_GET_REQ -> 0x13 RESP {major, minor}
  (what oFono's `g_isi_client_verify` sends first to every resource; the UICC driver keeps major/minor, and
  sends a 2-byte UICC_REQ when major < 4) [V 0xF0 and the version use; I the 0x12/0x13 sub ids, from memory].
  0x14 = service not identified / entity not reachable [I].

## 1. PN_UICC (0x8C) — Belle FP2 SIM access (oFono uicc.h/uicc.c) [V]

Message ids (REQ/RESP/IND triplets):

| id | name | id | name |
|---|---|---|---|
| 00/01/02 | UICC_REQ/RESP/IND (server) | 0F/10 | UICC_CONNECTOR_REQ/RESP |
| 03/04/05 | UICC_CARD_REQ/RESP/IND | 12/13/14 | UICC_CAT_REQ/RESP/IND |
| 06/07/08 | UICC_APPLICATION_REQ/RESP/IND | 15/16/17 | UICC_APDU_REQ/RESP, APDU_RESET_IND |
| 09/0A/0B | UICC_PIN_REQ/RESP/IND | 18/19/1A | UICC_REFRESH_REQ/RESP/IND |
| 0C/0D/0E | UICC_APPL_CMD_REQ/RESP/IND | 1B/1C | UICC_SIMLOCK_REQ/RESP |
| 1E/1F/20 | UICC_APDU_SAP_REQ/RESP/IND | 21/22/23 | UICC_PWR_CTRL_REQ/RESP/IND; 26 CARD_READER_IND |

Service types ([10] of every message): server: B0 START_UP_COMPLETE, **B1 STATUS_GET**, B2 READY,
B3 READY_FOR_ACTIVATION, B4 INITIALIZED, B5 SHUTTING_DOWN, B6 SHUT_DOWN_CONFIG, B7 ERROR. Card: C0
DISCONNECTED, C1 REMOVED, C2 NOT_PRESENT, C4 CARD_READY, **C5 CARD_STATUS_GET**, C8 REJECTED, C9 INFO_GET.
Application: **01 APPL_LIST, 03 APPL_HOST_ACTIVATE**, 05 START_UP_COMPLETE, 06 SHUT_DOWN_INITIATED,
07 APPL_STATUS_GET, 09 HOST_DEACTIVATE; ind: 71 TERMINATED, 72 RECOVERED, 75 ACTIVATED. PIN: 11 VERIFY,
12 UNBLOCK, 13 DISABLE, 14 ENABLE, 15 CHANGE, 16 SUBSTITUTE, **17 PIN_INFO**, 18 PROMPT_VERIFY; ind: 81
VERIFY_NEEDED, 82 UNBLOCK_NEEDED, 83 PERMANENTLY_BLOCKED, 84 VERIFIED. APPL_CMD: **21 READ_TRANSPARENT**,
22 UPDATE_TRANSPARENT, **23 READ_LINEAR_FIXED**, 24 UPDATE_LINEAR_FIXED, **25 FILE_INFO**, 26 APDU_SEND,
27 CLEAR_CACHE, 28/29 SESSION_START/END, 2A/2B READ/UPDATE_CYCLIC. Connector 31 CONNECT, 32 DISCONNECT,
33 RECONNECT. CAT 41 ENABLE .. 48 POLL; ind 91 FETCHED_CMD, 92 NOT_SUPPORTED, 93 REG_FAILED, 94 REG_OK.
Refresh 61 STATUS; A1..A4. APDU 51..53. SIMLOCK ind D0 ACTIVE. SAP E1..E8. PWR F1..F4, FA.

Status ([11]): 00 OK, 01 FAIL, 02 UNKNOWN, 10 NOT_READY, **11 START_UP_COMPLETED**, 12 SHUTTING_DOWN,
20 CARD_NOT_READY, **21 CARD_READY**, 22 CARD_DISCONNECTED, 23 CARD_NOT_PRESENT, 24 CARD_REJECTED,
**30 APPL_ACTIVE**, 31 APPL_NOT_ACTIVE, 40 PIN_ENABLED, **41 PIN_DISABLED**.
Details ([12]): 00 none, 01 INVALID_PARAMETERS, 02 FILE_NOT_FOUND, 03 SECURITY_CONDITIONS_NOT_SATISFIED,
04 APPL_CONFLICT, 05 CARD_ERROR, 06 SERVICE_NOT_SUPPORTED, 07 SESSION_EXPIRED.
Card type: 01 ICC, 02 UICC. Application type: 00 unknown, 01 ICC_SIM, 02 UICC_USIM. Start-up type
(SB_APPL_INFO): 00 NO_INIT_PROC, 01 INIT_PROC. Simlock status 01 active, 02 inactive.

Sub-block ids (id16 len16, lengths seen: 8 or 16): 0001 CARD_STATUS, 0002 APPL_DATA_OBJECT,
0003 APPLICATION, 0004 APPL_INFO, 0005 APPL_STATUS, 0006 AID, 0007 FCP, 0008 PIN, 0009 PIN_REF, 000A PUK,
000B PIN_SUBST, 000C PIN_INFO, 000D APPL_PATH, 000E SESSION, 000F FILE_DATA, 0010 TRANSPARENT_READ,
0011 TRANSPARENT_UPDATE, 0012 TRANSPARENT, 0013 LINEAR_FIXED, 0014 APDU, 0015 TERMINAL_PROFILE,
0016 POLLING_SET, 0017 REFRESH, 0018 REFRESH_RESULT, 0019 APDU_ACTIONS, 001A OBJECT_ID, 001B CHV,
001C FCI, 001D TERMINAL_RESPONSE, 001F CLIENT, 0020 STATUS_WORD, 0021 ENVELOPE, 0022 APDU_SAP_INFO,
0023 SHUT_DOWN_CONFIG, 0024 CARD_INFO, 0025 CARD_REJECT_CAUSE, 0026 CYCLIC, 0027 ACCESS_MODE,
0028 RESP_INFO, 0029 APDU_SAP_CONFIG.

Layouts (oFono builds/parses; offsets whole-message):

| message | layout |
|---|---|
| REQ 00 {B1, nsb 00} -> RESP 01 | [10] svc B1, [11] status, [12] details, **[13] server status** (11 = up), [15] nsb [V reads of data[1], data[3]] |
| CARD_REQ 03 {C5, 00} -> CARD_RESP 04 | [10] C5, [11] status, [12] details, [14] card type (sastartupservice reads it), [15] nsb; sb CARD_STATUS 0001 len 0008: **[sb+7] card status** (21 = ready -> client continues; anything else = no card) [V] ; [sb+6] card type [I] |
| APPLICATION_REQ 06 {01, 00} -> RESP 07 | [11] status, [15] nsb, sbs from [16]; sb APPL_DATA_OBJECT 0002: **[sb+6] appl type, [sb+7] appl id, [sb+8] appl status, [sb+9] AID length**, AID bytes from [sb+10] [V offsets; I that the AID follows]. Client keeps only types 01/02 and activates the first one |
| APPLICATION_REQ 06 HOST_ACTIVATE | `03, nsb 02, sb 0003 len 0008 {00 00, type, appl id}, sb 0004 len 0008 {00 00 00, start-up type 01}` (19 bytes) -> RESP 07: [11] 00 OK **or 30 APPL_ACTIVE** both accepted; sbs from [16]: 001F CLIENT ([sb+7] client id), 001C FCI, 001B CHV ([sb+4] chv id 1/2, [sb+5] pin id) [V] |
| PIN_REQ 09 {17 PIN_INFO, appl id, 00, nsb 01, sb 0009 PIN_REF len 0008 {.. pin id}} (13 bytes) -> PIN_RESP 0A | [11] **41 PIN_DISABLED = no PIN needed**, 40 = PIN enabled; sb 000C PIN_INFO: [sb+4] pin status, [sb+5] PIN attempts, [sb+6] PUK attempts [V] |
| APPL_CMD_REQ 0C read | `[10] svc 21/23, [11] appl id, [12] session id (00), [13..14] filler, [15] nsb 03; sb 001F CLIENT len 8 {client id at sb+7}; sb 0012 TRANSPARENT len 8 {[sb+4..5] offset, [sb+6..7] amount} or 0013 LINEAR_FIXED len 8 {[sb+4] record, [sb+5] offset, [sb+6] amount}; sb 000D APPL_PATH len 16 {[sb+4..5] file id, [sb+6] SFI, [sb+8] path len, [sb+10..11] MF 3F00, [sb+12..13] DF1, [sb+14..15] DF2}` [V, and byte-identical to the 701 trace below] |
| APPL_CMD_RESP 0D read | [10] svc echoed, [11] status, [12] details, [15] nsb; sb 000F FILE_DATA: **[sb+4..7] data length (32-bit BE), data from [sb+8]** [V] |
| APPL_CMD_REQ 0C {25 FILE_INFO, appl id, 00, 00 00, nsb 01, sb 000D APPL_PATH} -> 0D | sb 001C FCI; USIM: [sb+8] = 62 (FCP template), [sb+9] FCP len, then TLVs: 80 02 size, 82 (02 or 05) {descriptor byte: 01 transparent, 02 linear fixed, 06 cyclic; record length 16-bit; record count}, 83 02 file id. ICC SIM: [sb+10..11] length, [sb+12..13] file id, [sb+16..18] access, [sb+20] item len, [sb+21] structure, [sb+22] record length [V] |
| UICC_IND 02 / CARD_IND 05 / PIN_IND 0B / APPLICATION_IND 08 | oFono only re-polls on them (IND -> STATUS_GET; CARD_IND -> CARD_STATUS_GET). Layout assumed as the responses: [10] svc (B2 READY, C4 CARD_READY, 75 APPL_ACTIVATED, 84 PIN_VERIFIED), [11] status, [12] details, [15] nsb 00 [I] |

Client start-up sequence (oFono = what a well-behaved host does) [V]: version 0xF0/12 -> `00 B1` -> if
[13] == 11: `03 C5` -> if card status 21: `06 01` (list) -> `06 03` (activate first SIM/USIM app) -> on OK or
APPL_ACTIVE the SIM is "inserted + ready" -> `09 17` PIN_INFO (41 = no PIN) -> EF reads via `0C 25/21/23`
with the activated appl id and the CLIENT sub-block's client id.

## 2. UICC: what the 701/808 need to see a ready SIM

Observed on the 701 (`build/instances/701/serial.log`, our modem answering "no card") [V]:
- ch 35 (SAServer) subscribes to `8C 05` CARD_IND, `8C 0B` PIN_IND, `8C 26` CARD_READER_IND; sends `03 C5 00`.
- ch 7 (nokiatsy) sends `00 B1 00`, `03 C5 00`, `06 01 00` (**APPL_LIST, currently unanswered**: "no reply
  for resource 8c msg 06"), then, without any activation, APPL_CMD reads with appl id 00 / session 00 /
  client id 00: READ_LINEAR_FIXED record 1 of EF 6F61 path 3F00/7F43 (retried 5x), READ_TRANSPARENT EF 6F05
  (EF_LI) path 3F00/7FFF amount 0x68, EF 2F05 (EF_PL) under 3F00 amount 0x68, plus two amount-0 and
  offset-2/amount-1 reads. Each gets our `0D {svc 01 FAIL}`.

Proposed replies (`reply 8C ..` lines, data from [10]); all sub-block ids/lengths 16-bit BE, sub-blocks
padded to 4 bytes [I padding]:

```
# server up (unchanged): [13] 11 = start-up completed
reply 8C 00 01 B1 00 00 11 00 00
# card present: [14] card type 02 UICC, nsb 1, CARD_STATUS {filler, filler, card type 02, status 21 READY}
reply 8C 03 04 C5 00 00 00 02 01  00 01 00 08  00 00 02 21
# application list: one USIM (type 02, appl id 01, status 30 ACTIVE), AID len 16, AID A0000000871002FFFFFFFF8906030000 (sb len 0x1C = 28)
reply 8C 06 07 01 00 00 00 00 01  00 02 00 1C  00 00 02 01 30 10 A0 00 00 00 87 10 02 FF FF FF FF 89 06 03 00 00 00 00
#   (for service 03 HOST_ACTIVATE answer: 03 00 00 00 00 01  00 1F 00 08  00 00 00 01   = OK, CLIENT id 01;
#    for 07 APPL_STATUS_GET: 07 00 00 00 00 01  00 05 00 08  00 00 02 30 [I sb layout]) -> needs a per-service
#    switch in the modem (the table keys on msg id only), like the existing 8C 0C code.
# PIN info: PIN disabled (41), PIN_INFO {status 41?, 3 PIN tries, 10 PUK tries}
reply 8C 09 0A 17 41 00 00 00 01  00 0C 00 08  41 03 0A 00
# EF reads (code, not table): 0D {svc, 00 OK, 00, 00 00, nsb 01, 00 0F 00 (8+n padded), 00 00 00 n, data..}
#   EF_LI 6F05 / EF_PL 2F05: "en" = 65 6E, rest FF; FILE_INFO 25 -> FCI as in section 1; unknown files ->
#   {svc, 01 FAIL, 02 FILE_NOT_FOUND}. Accept appl id 00 and client id 00 (the TSY reads before activating).
# indications after the card reply, in that order [I layouts]: to ch 35 `8C 05 {C4 21 00 00 00 00}` (card
#   ready), `8C 02 {B2 00 00 00 00 00}` (server ready), `8C 08 {75 30 00 00 00 00}` (app activated),
#   `8C 0B {84 41 00 00 00 00}`? only if PIN_IND is what SAServer waits for; start with 05 alone.
```
Expected effect [I]: SAServer's SIM-present decision gets card status 21; the TSY activates (or just reads)
and the language read succeeds (no 40 s wait). Verify with `sydb strace 8C` on a 701 session and
sastartupservice's `8C 04` parser (it reads [14] and sb+7 [V earlier decompile]); if it waits for a PIN
indication, add `0B` with svc 84. The 808 log had no PN_UICC traffic to compare.

## 3. PN_SIM (0x09) and PN_SECURITY (0x08) (oFono sim.h/sim.c, Wireshark) [V]

Messages: 19/1A NETWORK_INFO (svc 2F READ_HPLMN -> [12] cause, PLMN BCD at [13]), 1D/1E IMSI_READ (svc 2D:
RESP [12] len, [13..20] IMSI BCD: first digit = high nibble of [13], then low/high nibbles, F = filler),
21/22 SERV_PROV_NAME (svc 2C: [12..] 17 UCS-2 chars, then disp_home, disp_roam), 29/2A DYNAMIC_FLAGS (svc
35), BA/BB READ_FIELD (svc 66 ICC: 10 raw bytes at [12]), BC/BD SMS, C0/C1 STATUS (svc 00 CARD_STATUS,
01 PIN, 05 ALL_SERVICES, 0D INFO), DC/DD PB_READ (svc 0F), ED SERVER_READY_IND (no payload), EF SIM_IND
([10] service, [11] status), F0 COMMON. Response layout everywhere: [10] service, [11] status, data from [12].
Services also: 15/16 CAT_SUPPORT_ENABLE/DISABLE, 52/53 READ/UPDATE_PARAMETER.
Status values: 00 NOT_AVAIL, **01 OK**, 02 PIN_VERIFY_REQUIRED, 03 PIN_REQUIRED, 04 SIM_BLOCKED, 05 PERM_BLOCKED,
**06 SIM_DISCONNECTED (= no SIM)**, 07 REJECTED, 08 LOCK_ACTIVE, 09/0A AUTOLOCK, **0B INIT_OK**, 0C INIT_NOT_OK,
0E PIN_DISABLED, 12/13 PIN_ENABLE/DISABLE_OK, 19/1A FDN_ENABLED/DISABLED, 1C NOT_OK, 20 NO_PIN, 21
PIN_VERIFY_OK, 22/23 PIN_BLOCKED/PERM, **24 DATA_NOT_AVAIL**, 27 STATE_CHANGED, 2E ILLEGAL_NUMBER, 35
SIM_REMOVED, 37 CONNECTED_IND, 3B/3C INIT_OK_CS/NOT_OK_CS, 45 INVALID_FILE, **49 ICC_EQUAL, 4A ICC_NOT_EQUAL,
4B SIM_NOT_INITIALISED, 4F DATA_AVAIL**, 50 SERVICE_NOT_AVAIL, 57..5B FDN_*, 5C..5F ISIM/USIM/SIM_AVAILABLE,
60 ISIM_NOT_INITIALIZED, 61 IMS_READY, 96 APP_DATA_READ_OK, 97 APP_ACTIVATE_OK, 98 NOT_OK, F9 NOT_DEFINED,
FA NOSERVICE, FB NOTREADY, FC ERROR. (So SAServer's "present" set {01,24,4B,FC} and "usable" {49,4A} in
n8-sim-present-mock.md = OK / data not avail / not initialised / error, and ICC equal / not equal: the
SIM_IND svc 05 status 4A we send means "ICC id differs from last time" — harmless, but 49 is the cleaner
"same card" value [I].)
oFono flow: C0 {00} -> C1 status != 06 => present; then SEC_STATE_REQ 08 11 -> 12 {cause}: **05 STARTUP_OK**
(no PIN), 02 PIN_REQUIRED, 03 PUK_REQUIRED, 07 STARTUP_ONGOING, 08 CODE_BLOCKED, 16 NO_SIM, 1A SIM_REJECTED,
1E INVALID_SIM; then 29 {35, 00} -> 2A status != FB => ready. Ready also on ED SERVER_READY_IND.
PN_SECURITY ids: 01/02/03 CODE_STATE_REQ/OK/FAIL ([11] code id 02 PIN, 03 PUK, 04 PIN2, 05 PUK2; [12] 00
disable, 01 enable, 04 query), 04/05/06 CODE_CHANGE, 07/08/09 CODE_VERIFY, 11/12 STATE.

## 4. PN_NETWORK (0x0A) / PN_MODEM_NETWORK (0xC8) (oFono network.h, netreg; Wireshark) [V]

Ids: **00/01/02 MODEM_REG_STATUS_GET_REQ/RESP/IND**, 03/04 MODEM_AVAILABLE_GET, 07/08 SET, 0B/0C RSSI_GET,
19 CS_STATE_IND, 1E RSSI_IND, 20 CIPHERING_IND, 27 TIME_IND, 28/29 OLD_OPER_NAME_READ, 2C CHANNEL_INFO_IND,
**35/36/37 RAT_IND/REQ/RESP**, 3A/3B CS_STATE_REQ/RESP, 40/41/42 CELL_INFO_GET/RESP/IND, 43 NITZ_NAME_IND,
48/49 NW_ACCESS_CONF, E0/E1/E2 REG_STATUS_GET/RESP/IND (PN_NETWORK style), E3/E4 AVAILABLE_GET, E5/E6
OPER_NAME_READ. oFono uses the 00..04 "modem" ids on PN_MODEM_NETWORK or when the server version < 14.
Sub-blocks (id8 len8): 00 REG_INFO_COMMON, 01 MODEM_AVAIL_NETWORK_INFO_COMMON, 02 OPERATOR_INFO_COMMON,
04 RSSI_CURRENT, **09 GSM_REG_INFO**, 0B (MODEM_)DETAILED_NETWORK_INFO, 0C GSM_OPERATOR_INFO, 10 TIME_INFO,
11 GSM_BAND_INFO, 2C RAT_INFO, 46 GSM_CELL_INFO, 47 WCDMA_CELL_INFO, 48/49 FULL/SHORT_NITZ_NAME,
55/56/59/5A REGISTRATION/ROAMING_CONF(1)_INFO, E1 AVAIL_NETWORK_INFO_COMMON, E7 OPER_NAME_INFO.
Reg status: **00 HOME**, 01 ROAM, 02 ROAM_BLINK, 03 NOSERV, 04 NOSERV_SEARCHING, 05 NOSERV_NOTSEARCHING,
**06 NOSERV_NOSIM**, 08 POWER_OFF, 09 NSPS, 0A NSPS_NO_COVERAGE, 0B NOSERV_SIM_REJECTED_BY_NW.
Select mode: 00 UNKNOWN, 01 MANUAL, **02 AUTOMATIC**, 03 USER_RESELECTION, 04 NO_SELECTION. Cause: 00 OK, 01
COMM_ERROR, 02 INVALID_PARAM, **03 NO_SIM, 04 SIM_NOT_YET_READY**, 05 NET_NOT_FOUND, 06 NOT_ALLOWED, 07
CALL_ACTIVE, 08 BUSY, 09 SEC_CODE_REQUIRED, 0C FORBIDDEN, 0D REJECTED, 0E CS_NOT_SUPPORTED, 0F
PAR_INFO_NOT_AVAILABLE, 11 NO_SELECTED_NETWORK, 16 SERVICE_NOT_ALLOWED. RAT: 01 GSM, 02 UMTS; RAT type 00
CURRENT, 01 SUPPORTED. CS state 00 INACTIVE, 01 ACTIVE; CS type 00 GSM. Band: 00 900/1800, 01 850/1900,
02 NOT_AVAIL, 03 ALL. Oper status 00 unknown, 01 available, 02 current, 03 forbidden. UMTS avail 00/01.
Layouts:
- REG_STATUS RESP: [10] cause, [11] nsb, sbs from [12]; IND: [10] nsb, sbs from [11] (oFono iterates "from
  offset 2" counting the msg id as 0; Wireshark: `[2] count, sbs at [3]` for the E2 IND) [V].
- REG_INFO_COMMON (len 8): [sb+2] reg status, [sb+3] selection mode; rest unread [V]. Our reply uses this.
- GSM_REG_INFO: [sb+2..3] LAC BE, [sb+4..7] cell id BE, [sb+8..10] operator code BCD (oper-code reads at
  offset 8 in GSM_CELL_INFO-style blocks; for GSM_REG_INFO oFono reads none) [I offset 8], **[sb+17] EGPRS
  support, [sb+20] HSDPA, [sb+21] HSUPA** (bytes 11..16, 18, 19 = band, GPRS, DTM, RAT: not read by any public
  parser) [V 17/20/21]. Length is therefore >= 24.
- RSSI_IND: [10] rssi (0 = unknown); RSSI_GET_REQ {00 CS_GSM, 02 CURRENT_CELL_RSSI, +4 filler on the modem
  resource}; RESP: [10] cause, sb 04 RSSI_CURRENT [sb+2] rssi [V].
- RAT_REQ {00 CURRENT_RAT}; RAT_RESP/IND: sb 2C RAT_INFO: [sb+2] rat (01 GSM, 02 UMTS), [sb+3] len, [sb+4]
  compact flag [V].
- SET_REQ: [10] "registered in another protocol" flag, [11] nsb, sb 0C/02 {mode, index} len 4; manual adds
  GSM_OPERATOR_INFO len 8 {3 BCD bytes, band 02 NOT_AVAIL, 2 filler} [V]. BCD: MCC d1 low nibble of b0, d2
  high of b0, d3 low of b1; MNC d3 high of b1 (F if 2-digit), MNC d1/d2 in b2 [V].
- OPER_NAME_READ_REQ E5 {00 hardcoded latin, max len, index16, filler, nsb 00}; RESP sbs from [16] (oFono
  iterates from data offset 6): 0C GSM_OPERATOR_INFO [sb+2] oper code; E7 OPER_NAME_INFO [sb+3] length in
  UCS-2 chars, name from [sb+4] [V].
- TIME_INFO sb 10 (len 10): [sb+2] year-2000, month, day, hour, min, sec, [sb+8] UTC offset (bit7 sign, 15-min
  units), [sb+9] DST; 0x64 = invalid [V]. CELL_INFO: 46 GSM_CELL_INFO [sb+2..3] LAC, [sb+4..7] CID, [sb+8..11]
  band mask (01 900, 02 1800, 04 1900, 08 850), [sb+12..14] oper code [V Wireshark offsets from data].
- Host side after reachability on PN_MODEM_NETWORK: CS_STATE_REQ 3A -> 3B; if inactive, NW_ACCESS_CONF_REQ 48
  with sbs 59 and 5A (len 4, value 1) -> 49 [V].

## 5. PN_GPDS (0x31) (oFono gpds.h/gprs.c) [V]

Ids: 00/01 LL_CONFIGURE, 02/03/04 CONTEXT_ID_CREATE_REQ/RESP/IND, 05 CONTEXT_ID_DELETE_IND, 06/07
CONTEXT_CONFIGURE, 08/09/0A CONTEXT_ACTIVATE_REQ/RESP/IND, 0B/0C/0D CONTEXT_DEACTIVATE, 0E/0F/10 NWI,
11/12 CONFIGURE, 13/14/15 ATTACH_REQ/RESP/IND, 16/17/18 DETACH, 19/1A STATUS, 1B/1C/1D SMS_PDU, 1E
TRANSFER_STATUS_IND, 1F CONTEXT_ACTIVATE_FAIL_IND, 20/21 LL_BIND, 22/23/24 CONTEXT_STATUS, 25
CONTEXT_ACTIVATING_IND, 2A/2B/2C CONTEXT_MODIFY, 2D ATTACH_FAIL_IND, 2F CONTEXT_DEACTIVATING_IND, 30/31/32
CONFIGURATION_INFO, 33/34 CONTEXT_AUTH, 37 RADIO_ACTIVITY_IND, 38/39 FORCED_READY_STATE, 3A/3B
CONTEXTS_CLEAR, 3C..47 MBMS, 48/49 TEST. Sub-blocks (id8 len8): 00 COMP_INFO, 01/02/03 QOS_REQ/MIN/NEG,
04 PDP_ADDRESS, 05 APN, 06/07/08 QOS99_*, 09/0A TFT, 0B USER_NAME, 0C PASSWORD, 0D PDNS, 0E SDNS,
0F CHALLENGE, 90 DNS_ADDRESS_REQ. Status: 00 ERROR, **01 OK**, 02 FAIL. PDP type 01 PPP, 21 IPv4, 57 IPv6.
Attach status 00 detached, 01 attached; attach mode 00 manual, 01 automatic, FF default; transfer status
00/01 with cause 02 attached, 03 detached, 04 resumed, 05 suspended no coverage, 07 call/SMS, 08 call, 09
RAU, 0A LU, 0B DSAC. Causes: 07 GPRS_NOT_ALLOWED, 0B PLMN_NOT_ALLOWED, 1B APN, 1C PDP_UNKNOWN, 1D AUTH, 1F
ACT_REJECT, 24 DEACT_REGULAR, F5 DETACHED, F9 SIM_REMOVED, FA POWER_OFF, ...
Layouts: CONFIGURE_REQ 11 {attach mode, mt act mode 00 reject, classC FF, AOL FF, 00, 00} -> 12 {status};
ATTACH_REQ 13 {00 follow off} -> 14 {status 01 OK, ..}; DETACH_REQ 16 {00, nsb 00} -> 17 {status};
DETACH_IND 18 {cause, ..}; TRANSFER_STATUS_IND 1E {status, cause}; STATUS_REQ 19 -> 1A {[10] attach status,
[11..19] unread, [20] transfer status, [21] transfer cause} (>= 12 data bytes) [V]. PEP type for the pipe:
PN_PEP_TYPE_GPRS = 04 [V]. Max PDP contexts via PN_PHONE_INFO PP read (feature CA) [V].

## 6. PN_SMS (0x02) (oFono sms.h) [V]

Ids: 02/03 MESSAGE_SEND, 06/07/08 PP_ROUTING_REQ/RESP/NTF, 09/0A GSM_RECEIVED_PP_REPORT, 0B/0C/0D
GSM_CB_ROUTING, 22 MESSAGE_SEND_STATUS_IND, **30/31 SETTINGS_UPDATE, 32/33 SETTINGS_READ**, 3B/3C
RECEIVED_MSG_REPORT, **41/42 RECEIVE_MESSAGE, 43 RECEIVED_MSG_IND**. Causes: **00 OK**, 01 ROUTING_RELEASED,
02 INVALID_PARAMETER, 03 DEVICE_FAILURE, 05 ROUTE_NOT_AVAILABLE, 09 NO_SIM, 0A SIM_NOT_READY, 0B
NO_NETW_RESPONSE, 0F SMSC_ADDR_NOT_AVAILABLE, 10 ROUTING_FAILED, 11 CS_INACTIVE; cause type 00 common, 01 GSM.
Sub-blocks: old id8: 00 GSM_DELIVER, 01 STATUS_REPORT, 02 SUBMIT, 03 COMMAND, 06 DELIVER_REPORT, 0C REPORT,
0D ROUTING, 0E CB_MESSAGE, 11 GSM_TPDU, 80 COMMON_DATA, 82 ADDRESS; id16: 001C TPDU, 0023 ROUTE_INFO, 0031
SMS_PARAMETERS, 0082 ADDRESS. Routing cmd 00 release, 01 set, 02 suspend, 03 resume, 04 update; routing mode
0B all, 0C CB_DDL; routing type 06 TPDU; message type 06 MT_ALL; address types 00 unicode, 01 GSM 03.40, 02
GSM 04.11/SMSC; reception command 01 activate, 02 deactivate; status 01 active, 02 inactive; setting type 02
route; route preference 00 any/GPRS pref, 01 CS, 02 GPRS, 03 CS pref, 04 default.

## 7. PN_CALL (0x01) / PN_MODEM_CALL (0xC9) (oFono call.h) [V]

Ids: 01/02 CREATE, 03 COMING_IND, 04/05 MO/MT_ALERT_IND, 06 WAITING_IND, 07/08 ANSWER, 09/0A/0B RELEASE,
0C TERMINATED_IND, 0D/0E/0F STATUS_REQ/RESP/IND, 10 SERVER_STATUS_IND, 11/12/13 CONTROL, 14..16 MODE_SWITCH,
17/18 DTMF_SEND, 19/1A DTMF_STOP, 1B/1C DTMF_STATUS/TONE_IND, 1E RECONNECT_IND, 1F/20 PROPERTY_GET, 21/22/23
PROPERTY_SET, **24/25 EMERGENCY_NBR_MODIFY_REQ/RESP**, 26/27 EMERGENCY_NBR_GET, 28/29 EMERGENCY_NBR_CHECK,
2A CREATE_NO_SIMATK, 2B..30 TIMER, 31 EMERGENCY_NBR_IND, 32 SERVICE_DENIED_IND, 33 USER_CONNECT_IND,
34/35 RELEASE_END, 38/39/3A SERVICE_ACTIVATE, 40 AUDIO_CONNECT_IND, 41 SECURITY_IND, 42/43 MEDIA_HANDLE,
A0.. GSM_*. So our `C9 24 -> 25` is CALL_EMERGENCY_NBR_MODIFY (emergencyservice.exe pushing its list):
sub-block 08 CALL_CAUSE {[sb+2] cause type, [sb+3] cause} = what nokiatsy's parser reads [V names, matches
the decompile]. Status values: 00 IDLE, 01 CREATE, 02 COMING, 03 PROCEEDING, 04 MO_ALERTING, 05 MT_ALERTING,
06 WAITING, 07 ANSWERED, 08 ACTIVE, 09 MO_RELEASE, 0A MT_RELEASE, 0B..10 HOLD/RETRIEVE/RECONNECT/TERMINATED/
SWAP. Cause types 00 default, 01 client, 02 server, 03 network; ISI causes 00 none, 01 NO_CALL, 0F
NO_COVERAGE, 1F NO_SIM, 25 CS_INACTIVE, 2C NOT_REGISTERED. Sub-blocks: 01 ORIGIN_ADDRESS, 03
DESTINATION_ADDRESS, 07 MODE, 08 CAUSE, 0A STATUS, 0B STATUS_INFO, 0E ORIGIN_INFO, 14 EMERGENCY_NUMBER, 1B
EMERGENCY_NUMBER_INFO, 1C STATUS_MODE, 1D ADDR_AND_STATUS_INFO, 47 LINE_ID, A0.. GSM_*. Call ids 01..07,
10 conference, 20 waiting, 40 hold, 80 active, F0 all. Modes 00 emergency, 01 speech, A5/A2 ALS line 1/2.

## 8. PN_MTC (0x15) / PN_MODEM_MCE (0xC2): modem state, leaving offline (oFono mtc.h, n900.c, u8500.c) [V]

MTC ids: 01 STATE_REQ, 02 STATE_QUERY_REQ, 03 POWER_OFF_REQ, 04 POWER_ON_REQ, 0B STARTUP_SYNQ_REQ, 12
SHUTDOWN_SYNC_REQ, 64 STATE_RESP, 65 STATE_QUERY_RESP, 66 POWER_OFF_RESP, 67 POWER_ON_RESP, 6E
STARTUP_SYNQ_RESP, 75 SHUTDOWN_SYNC_RESP, **C0 STATE_INFO_IND**. States: 00 POWER_OFF, **01 NORMAL**, 02
CHARGING, 03 ALARM, 04 TEST, 05 LOCAL, 06 WARRANTY, 07 RELIABILITY, 08 SELFTEST_FAIL, 09 SWDL, **0A
RF_INACTIVE**, 0B ID_WRITE, 0C DISCHARGING, 0D DISK_WIPE, 0E SW_RESET, FF CMT_ONLY. Actions: 03 START, 04
READY, 0C NOS_READY, 11 SOS_START, 12 SOS_READY. Causes: 00 OK, 01 FAIL, 02 NOT_ALLOWED, 05
STATE_TRANSITION_GOING_ON, 06 ALREADY_ACTIVE, 10 SERVICE_DISABLED, 13 NOT_READY_YET, 14 NOT_SUPPORTED, 16
TRANSITION_ONGOING, 17 RESET_REQUIRED.
Layouts: STATE_QUERY_REQ {00 00} -> 65 {[10] current state, [11] target state}; STATE_REQ 01 {state, 00}
(online = 01 NORMAL, offline = 0A RF_INACTIVE) -> 64 {[10] cause}: 00 OK completes at once, else the host
waits for C0; STATE_INFO_IND C0 {[10] state, [11] action}: action 03 START = transition begins (ignore),
**04 READY = state reached**; POWER_OFF_REQ / STARTUP_SYNQ / SHUTDOWN_SYNC: {00 00}. Powered = any state but
POWER_OFF/CHARGING/SELFTEST_FAIL. (Our profile already answers 15 65/71/79; the host leaves offline by `15 01
{01 00}` then expects `64 {00}` and `C0 {01 04}` [V oFono; I that Symbian's SSM uses the same pair].)
MCE (0xC2, newer modems): 00 MODEM_STATE_IND {state, action}, 01/02 MODEM_STATE_QUERY {00 00} -> {current,
target}, **03/04 RF_STATE_REQ {rf 00 off / 01 on, 00} / RESP {cause}**, 05 RF_STATE_IND {rf state, action},
06/07 RF_STATE_QUERY, 08/09 POWER_OFF. Modem states 00 NORMAL, 01 LOCAL, 80 SW_RESET, 81 POWER_OFF; actions
03 START, 04 READY; status 00 OK, 01 FAIL, 06 ALREADY_ACTIVE, 16 TRANSITION_ONGOING [V].

## 9. PN_PHONE_INFO (0x1B) / PN_MODEM_INFO (0xC5) / PN_EPOC_INFO (0x62) (oFono info.h) [V]

Ids: 00/01 SERIAL_NUMBER_READ (type 41 IMEI plain -> sb 41 string), 02/03 PP_READ (sb 47 PP: count, then
16-bit {feature, value} words; feature CA = max PDP contexts), 07/08 VERSION_READ (type 01 MCUSW -> sb 48
MCUSW_VERSION, 00 MODEMSW_VERSION), 15/16 PRODUCT_INFO_READ (01 name, 07 manufacturer -> sb 01/07). Status
00 OK, 01 FAIL, 02 NO_NUMBER, 03 NOT_SUPPORTED. The 701 sends `1B 12` with sb 0x29 ({00 01 74 ..}) which is
not in oFono's list [V trace; I meaning].

## 10. Other resources named publicly (Wireshark resource table) [V]

0x06 Subscriber Services (SS), 0x08 SIM authentication (= PN_SECURITY), 0x10 Indication, 0x32 General Stack
Server (GSS), 0x54 GPS, 0xB4 Radio Settings (WRAN), 0xD9 pipe (oFono gisi), 0xF0 common message id.
