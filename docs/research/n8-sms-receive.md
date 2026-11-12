# N8 (Delight 6.7) nokiatsy.dll: incoming SMS over PN_SMS (02 43) and the report exchange

Source: static read of `build/roms/nokia-n8-rm596-delight/*/ghidra/nokiatsy.dll.c` + `nokiatsy.dis`, ETel IPC
numbers from the Symbian^3 SDK `platform/etelmmcs.h`, CTSY behaviour from the public commontsy source.
Offsets are absolute ISI offsets ([3] resource, [8] tid, [9] msg id, payload from [10]).

## Dispatch [verified]
`FUN_80e91042` (not decompiled, see .dis 80e91042..80e91144): resource [3]==0x02 -> on [9]:
0x43 -> `FUN_80e90e0e` (RECEIVED_MSG_IND), 0x42 -> `FUN_80e90e6e`, 0x3C -> `FUN_80e907c2`, 0x31 -> `FUN_80e9079e`,
0x03 -> `FUN_80e90efe` (send resp, by tid 3/5). Resource 0x09 msg 0xBD = SIM SMS store (class-2 path). No
"reception activated" state is checked anywhere on the 0x43 path: our `02 42` reply is only needed to complete
the activate request (tid 0x11 -> IPC 1000363) [verified]; it is not a gate for delivery.

## 02 43 layout as parsed (`FUN_80e90d00` classifier, `FUN_80e9081a` deliver) [verified]
Sub-blocks are searched with isimessage `FindSubBlockOffsetById(start=0x0c, id, type=4)`; type 4 = id16/len16
(the TSY builds its own `02 30` with the same type and it comes out `00 23 00 08 ...`) [verified]. Bytes
[10] and [11] are never read; by convention [10]=00 filler, [11]=number of sub-blocks [inferred].
- `0x0082` ADDRESS (optional): [sb+4] address type (not read, use 02 = GSM 04.11 SMSC), [sb+5] = M bytes,
  [sb+6..] = M bytes in GSM 04.11 form `len TOA BCD...` (digits=(len-1)*2, TOA 0x91 -> "+", F nibble ends;
  `FUN_80e91b72`/`FUN_80e91a2a`). Missing -> empty SMSC address, no error.
- `0x001C` TPDU (required): [sb+4] = N (TPDU length, 8-bit), [sb+5] filler, [sb+6..sb+6+N) = TPDU, pad to 4.
  Missing -> indication silently dropped (nothing completed). N must be > 0 and TPDU[0] & 3 must be 0
  (SMS-DELIVER) or 2 (STATUS-REPORT); otherwise the TSY itself sends `02 3B` tid 0E with cause for
  KErrGsmSMSUnspecifiedInvalidMessage (-4607, DAT_80e90a7c) and completes nothing.
- No route id / message reference / storage info is read or echoed later [verified: `FUN_80e8f2ca` uses
  no state from the indication]. Class-2 detection (`FUN_80e90d00`: DELIVER with DCS&3==2 and coding
  group 1/3/F, or PID 0x41..0x47) diverts to the SIM store (PN_SIM 0xBC); a WAP push (DCS 0xF5/0x04) takes the
  plain path. TPDU buffer is TBuf8 of ~164 bytes (RMobileSmsMessaging::TMobileSmsGsmTpdu) [inferred].
Delivery: `FUN_80e7a83a(..., 32001 = EMobileSmsMessagingReceiveMessage, {TPDU, attrs{SMSC addr}}, KErrNone)`.

## Example: one 8-bit SMS-DELIVER of N bytes, SMSC +358500000000 (tid free, e.g. 00)
```
02 43 { 00 02                                             [10] filler, [11] sb count
        00 82 00 10  02 08  07 91 53 85 00 00 00 00  00 00  ADDRESS: type 02, M=8, 04.11 addr, 2 pad
        00 1C LL LL  NN 00  <NN TPDU bytes> <pad to 4> }   TPDU: NN=N, LL = 6+N rounded up to x4
```
N=37 -> LL=0x002C, 1 pad byte; payload = 2+16+44 = 62 bytes; ISI length [4..5] must cover it all
(the sub-block search runs to the message length, not to [11]) [inferred].

## Report exchange (CTSY + `FUN_80e8f2ca`) [verified unless marked]
ETel: commontsy `CMmSmsTsy::CompleteReceiveMessage` completes a PENDING `RMobileSmsMessaging::ReceiveMessage`
(smsstack keeps one outstanding). If none is pending the message is NOT buffered: the CTSY Nacks with
KErrGsmSMSUnspecifiedProtocolError and the message is lost [verified in public source; Nokia build inferred].
smsstack then calls AckSmsStored(deliver-report TPDU, usually `00 00`) -> `FUN_80e8f2ca(tid=9, tpdu, 0)`:
```
02 3B { 00 00 00 00 00 00 }   [8]=09  [10]=cause present (0), [11]=cause (0), [12]=0, [15]=sb count (0)
```
NackSmsStored -> tid 0A: [10]=01, [11]=cause mapped from the ETel error (`FUN_80e8f280`), [15]=01 and one
sub-block `00 19 00 08 <TPDU[0]> <TPDU[1] or AF> 00 00` [verified]; a report TPDU with TP-UD adds sub-blocks
0x001D (PID, DCS, UDL, UDH) and 0x0020 (user data), count +2 [names inferred]. The TSY's own tid 0E report
(bad TPDU) is `[10]=01 [11]=cause [15]=01` + the 0x0019 block with `<MTI> AF`.
Expected reply, handler `FUN_80e907c2` (reads [8] and [10] only):
```
02 3C { 00 00 00 00 00 00 }   [8] = tid echoed (09 -> completes 27002 AckSmsStored, 0A -> 27003 NackSmsStored)
```
[10] = cause: 00 -> KErrNone via `FUN_80e9e5c4(2,0,cause)`; non-zero -> error and the SIM-route flag (+0x7c)
is cleared. A reply to tid 0E completes nothing. Payload length 6 is a guess (only [10] is read) [inferred].
So: ReceiveMessage must be pending (it is, once smsstack runs); 42 is not required for 43; nothing from 43
is echoed; answer every 3B with 3C tid-echoed and [10]=00.
