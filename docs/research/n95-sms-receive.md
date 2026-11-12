# N95 8GB (RM-320 35.0.001) NOKIATSY.DLL: incoming SMS over PN_SMS, old "PP routing" protocol

Source: static read of `build/firmware/RM-320/35.0.001/13/ghidra/NOKIATSY.DLL.c` + `NOKIATSY.dis`
(made with `tools/xipdis.py core-flat.img core.json NOKIATSY.DLL`). Offsets are absolute ISI offsets
([3] resource, [8] tid, [9] msg id, payload from [10]). Names after oFono's isi/sms.h (naming only).
The N95 does NOT speak the N8's 02 41/42/43/3B/3C protocol (docs/research/n8-sms-receive.md).

## Dispatch (0x808d7f38, resource [3]==0x02 on [9]) [verified]
03 -> send resp (tid 3/4, `FUN_808d7dde`) | 07 -> `FUN_808d7d78` (PP_ROUTING_RESP) | **08 -> `FUN_808d7b44`
(PP_ROUTING_NTF = incoming SMS)** | 0A -> `FUN_808d75a8` (RECEIVED_PP_REPORT_RESP) | 13 -> tid 7 handler |
15, 17, 1F -> other | 19 -> `FUN_808d71fc` | resource 09: 1A/34/BD -> SIM SMS store (class-2 path).
Sub-blocks everywhere are type 1 = `id8 len8` (`isimessage` ord6(...,id,1) / ord18(...,id,1)) [verified].
Senders go through `(obj+0x14)->vfn[3](resource, tid, msgid, data)` [verified: 02 06 and 02 18 match the log].

## Requests the TSY sends and the replies it accepts
- `02 06` PP_ROUTING_REQ, built by `FUN_808d694c(cmd)`: `[10]=cmd (01 = set) [11]=01 sb count` + sb
  `0d 08 06 06 00 00 00 00`; cmd is stored at obj+0x9c [verified]. The *first* 02 06 in the log (tid c5,
  sb `0d 08 07 00 ...`) and `01 AA` (tid c1) are not built by NOKIATSY (only one 0x0d builder exists):
  another TSY on channel 7, probably SIMATKTSY (SAT data download) [inferred].
  Reply `02 07`, tid echoed, `[10]=cause` (00 = OK); only [10] is read. Completes custom IPC 1000151
  (cmd 01) / 1000152 (other) with `FUN_808e3450(2,0,cause)` -> KErrNone for 00 [verified; the error map
  itself not decompiled]. Send `02 07 { 00 00 00 00 00 00 }`. Answer the tid-c5 one the same way;
  NOKIATSY's handler runs for it too (no tid check) and completes an idle IPC, harmless [inferred].
- `02 18`, built by `FUN_808d4a10(mode)`: `[10]=00 [11]=01` + sb `10 08 01 fb 00 00 <mode==0||2> 00`
  [verified]. Reply `02 19 { 00 00 00 00 00 00 }`: reads only [10]=cause, completes IPC 0x59f2 (23026)
  with the mapped cause [verified; IPC name inferred: an SMS receive-mode/bearer setting].
- `02 09` RECEIVED_PP_REPORT_REQ, `FUN_808d5e26(tid, tpdu, cause)`: `[10]=cause present [11]=cause
  [12..14]=00 [15]=01` + sb 06 (DELIVER_REPORT: `06 LL 00 <c> 00 00 00 00`, +sb 80/0a/09 when the
  report TPDU has user data). tid 8 = ETel AckSmsStored (27002), tid 9 = NackSmsStored (27003), tid 0D =
  TSY's own report of a bad TPDU (cause 0x61) [verified]. Reply `02 0A { 00 00 00 00 00 00 }`, tid echoed:
  reads [8] and [10] only; [10]!=00 completes ReceiveMessage (32001) with -2 and clears obj+0x80 [verified].
No routing-active state gates delivery: `FUN_808d7b44` checks nothing from 06/07 [verified].

## 02 08 PP_ROUTING_NTF as parsed by `FUN_808d7a0c` (classifier) + `FUN_808d7b44` [verified]
[10],[11] are never read (convention: `00 01`) [inferred]. From offset 0x0c the TSY needs sb **0x11**
(container; its bytes [2],[3] are not read) and, searched from `sb11+4` to the message end:
- `0x82` ADDRESS, optional: `[2]=type (not read; 02 = SMSC) [3]=M [4..4+M)=address` -> `FUN_808d87b4`
  (M<=1 -> empty SMSC, no error). Format = GSM 04.11 `len TOA BCD...` as the TSY's own 02 12 sender
  builds it [inferred].
- `0x80` TPDU, required: `[2]=N [3]=filler [4..4+N)=TPDU`. N==0 or TPDU[0]&3 not in {0 (DELIVER),
  2 (STATUS-REPORT)} -> TSY sends `02 09` tid 0D cause 0x61 and completes nothing (ReceiveMessage gets -2
  only if that send fails). Missing sb 0x11 -> dropped silently.
Class 2 (DCS&3==2 with coding group 1/3/F, PID 0x41..0x47, or TPDU[1..4] = `04 D0 ..` voicemail shape)
goes to the SIM store (resource 09 BC...) instead; a WAP push/8-bit OMA CP TPDU (DCS 0x04/0xF5) takes the
plain path. Delivery: completes custom IPC 1000080 (0) then `32001 EMobileSmsMessagingReceiveMessage`
with {TPDU copy, attrs{SMSC}} and KErrNone (`DAT_808d7fec`). Buffer is a TBuf8 of 0x12c+ bytes [verified].

## Example: one SMS-DELIVER TPDU of N bytes, SMSC +358500000000, tid 00
```
02 08 { 00 01                                        [10] filler, [11] sb count (neither read)
        11 LL 00 00                                  container, LL = 4 + 12 + L2 (or 4: not checked)
        82 0C 02 08  07 91 53 85 00 00 00 00         ADDRESS: type, M=8, 04.11 address
        80 L2 NN 00  <NN TPDU bytes> <pad to 4> }    TPDU: NN=N, L2 = 4+N rounded up to x4
```
N=37 -> L2=0x2C, LL=0x3C, payload 2+60 = 62 bytes; ISI length [4..5] covers it. Lengths are 8-bit, so
N <= 164 (TMobileSmsGsmTpdu) is fine; concatenated parts are separate 02 08 messages [inferred].
Then expect `02 09` tid 08 (Ack, after smsstack stored it) -> reply `02 0A` tid 08 `[10]=00`.

## Mock proposal (syborg_modem.c)
Declare the variant in the N95 profile (e.g. `modem.sms = "pp-routing"`), not from the request ids:
- 02 06 -> 02 07 cause 00; 02 18 -> 02 19 cause 00; 02 09 -> 02 0A tid echoed cause 00.
- `modem_set_sms`: with `pp-routing`, emit the 02 08 layout above (sb 11 / 82 / 80, 8-bit ids and lengths)
  to the channel that sent 02 06 (`sms_key`); keep the N8's 02 43 layout for the `v2` variant.
