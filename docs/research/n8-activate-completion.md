# N8 Delight nokiatsy: what completes RPacketContext::Activate (IPC 41001)

Static analysis of nokiatsy.dll (ROM 0x80e7xxxx..0x80eaxxxx, build/roms/nokia-n8-rm596-delight/*/ghidra/nokiatsy.dis).
IPC numbers from the Symbian^3 pcktcs.h (~/work/symbian/.../etelpacketdata/inc/secure/pcktcs.h) [verified].

## Answer

The Activate ETel request is completed only by **GPDS_CONTEXT_ACTIVATING_IND (31 25)**, never by 31 09 or 31 0A.
Our modem never sent a well-formed 25, so 41001 sat until the 10 s timeout. The earlier 31 25 {cid} crashed the TSY because
the handler reads byte 12 and scans sub-blocks from offset 16 unconditionally (TIsiReceiveC panics past the message end).

Send, in this order, after `-> 31 08 {cid 00}` (bytes in braces start at ISI offset 10, as in the brief's notation):

    <- 31 09 {04 01 00}                                   CONTEXT_ACTIVATE_RESP (ignored by the TSY, keep as is)
    <- 31 25 {04 00 21 00 00 01 | 05 0C 08 'internet' 00}  CONTEXT_ACTIVATING_IND -> completes 41001 (EPacketContextActivate)
    <- 31 0A {04 03 | 04 08 .. | 0D 08 .. | 0E 08 ..}      CONTEXT_ACTIVATE_IND  -> 42003 NotifyConfigChanged + 40004 NotifyConnectionSpeedChange
    <- 31 1E {01 00}                                      TRANSFER_STATUS_IND (unchanged)

31 25 layout: [10] cid, [11] filler, [12] pdp_type (0x21 IPv4 / 0x57 IPv6 / 0x01 PPP; echo byte 11 of the CONTEXT_CONFIGURE_REQ),
[13..14] filler, [15] number of sub-blocks, [16..] sub-blocks; sub-block 0x05 GPDS_APN_INFO = {05, len, apn_len, apn.., pad} exactly as the
TSY sent it in 31 06. Minimal form without APN: `31 25 {04 00 21 00 00 00}` (the TSY then zeroes the APN; keep the APN block so CTSY keeps it).

## Evidence

- Dispatcher 0x80e9a418 [verified]: byte 3 == 0x31 (PN_GPDS), switch on byte 9 (msg id). ids < 0x1f go through RVCT `__ARM_switch8`
  (table at 0x80e9a452, count 0x0e, target = 0x80e9a452 + 2*entry): 0x01 -> 0x80e9a14a, 0x03 -> 0x80e9a3cc, 0x04 -> FUN_80e9a36e,
  0x07 -> 0x80e99ffc, 0x0A -> FUN_80e99586, 0x0C -> 0x80e99254, 0x0D -> 0x80e991d6, 0x05 -> 0x80e99030; **0x09 and every other id < 14
  -> entry 0x08 = 0x80e9a462 = `b 0x80e9a444` = return (no handler)**. Explicit compares: 0x1f -> 0x80e9949e, 0x23 -> 0x80e99158,
  0x24 -> 0x80e990ec (both read u32 at 12 and 16, complete literal 0x80e994d4: data volume), **0x25 -> 0x80e99b7a**, 0x2b -> 0x80e9a518,
  0x2c -> 0x80e99414, 0x2f -> 0x80e9935e, 0x34 -> 0x80e99dc0. (Ghidra's .c has neither 0x80e99b7a nor the dispatcher.)
- Activate handler 0x80e99b7a..0x80e99dae [verified] (not in the .c; FUN_80e99586 really ends at 0x80e99b78):
  cid = byte 10; FUN_80e9df38(list,cid) stores 0xff into record+0x81c (clears a pending-state byte);
  r = FUN_80e9dc88(list,cid,&name): if r == 0: type = FUN_80e9e230(list,cid) = record[+0x6c] (11 records of 0x86c bytes, record[+4]=cid);
  obj = FUN_80e9e340(list,cid,type) (type 1 -> record+0x70, type 4 -> record+0x448). **Only type 1 (KConfigGPRS) or 4 (KConfigRel99Rel4)
  continue** (0x80e99bec/0x80e99c6e); else no completion. Then FUN_80e9f394(obj+4, byte 12): 0x21->0 (IPv4), 0x57->1 (IPv6), 0x01->3 (PPP),
  other -> unchanged [verified 0x80e9f394]; FindSubBlock(msg, 16, id 5, type 1, &off) (0x80e99c1c): apn_len = byte off+2, data at off+3 ->
  Copy into obj+8, else Zero; package {name, obj} and FUN_80e7a83a(router, 0xa029 = 41001, pkg, 0) at 0x80e99d9c.
  If r == -6 (KErrNotFound): FUN_80e9e41c (second list) must return 0 and FUN_80e9e412(list) (byte at list+0x5cac) must equal cid, then the
  same parse into a stack TContextConfigGPRS (etelpckt ord 40) and the same completion (dial-up/external context path) [inferred naming].
- FUN_80e99586 (0x0A) [verified]: literal pool 0x80e99db4 = 0x0000a413 = 42003 EPacketContextNotifyConfigChanged, 0x80e99db8 = 0x9c44 =
  40004 EPacketContextNotifyConnectionSpeedChange (objdump prints halfwords byte-swapped; read the word, not the printed `a413`).
  Sub-blocks parsed from 12: 0x04 PDP addr (len at +3, data +4), 0x05 APN (+2/+3), 0x03 QoS neg (byte +5 -> FUN_80e993c4 speed), 0x0D/0x0E DNS,
  0x00 comp info (+2 header, +3 data; 0xff = unchanged), 0xF4 IPv6/other addr (+3/+4), 0xF7 (+2). None of them is required for 41001.
- Status Active: nokiatsy loads 40005 (0x9c45) only in the 0x05, 0x0D, 0x1F, 0x2C, 0x2F handlers (0x80e9908a, 0x80e99240, 0x80e99560,
  0x80e99482, 0x80e993ae) [verified]; no GPDS message sets Active in the LTSY. The CTSY does it: CMmPacketContextGsmWcdmaExt::CompleteActivate
  (open-source commontsy, ~/work/symbian/oss.FCL.sf.os.cellularsrv/.../cmmpacketcontextgsmwcdmaext.cpp) completes the client request and sets
  EStatusActivating; CompleteNotifyConfigChanged (from 31 0A) then sets EStatusActive [verified in source; firmware CTSY assumed same, inferred].
- 31 09 content is irrelevant to the completion (handler table entry = return) [verified]; a failure path for 09 was not looked for [inferred: none].
- Pipe messages (D9 60/61/64), 31 24, QoS99 sub-block 0x08 in 0A: not read by the Activate path [verified by absence in 0x80e99b7a].

## Modem change (syborg_modem.c gpds_request, CONTEXT_ACTIVATE 0x08)

After replying 31 09, emit 31 25 with the stored cid, the pdp_type and the APN sub-block captured from the context's CONTEXT_CONFIGURE_REQ
(byte 11 and sub-block 0x05), then the existing 31 0A and 31 1E. Keep the order 09, 25, 0A, 1E (25 before 0A so Activate completes before
ConfigChanged moves the status to Active).
