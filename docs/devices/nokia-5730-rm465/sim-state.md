# 5730 (RM-465): ETel 40018 and SIM state with no SIM (mock level)

Static reading of the 5730 core image plus the Symbian OSS etelpacketdata sources (names only); no
runs. [verified] = read in code or tables, [inferred] = best guess.

## ETel 40018 is a packet-data notification, not the blocker

- 40018 = **`RPacketService::NotifyChangeOfNtwkRegStatus`** [verified]. etelpckt.dll export 116
  (0x80463e3b) loads 40020 and subtracts 2. The name comes from OSS `etelpacketdata/eabi/EtelpcktU.def`
  ordinal 116. Neighbours: GetNtwkRegStatus = 40013 (ord 97), GetMSClass = 40020 (ord 85).
  ConnMon also holds the literal 40018 [verified].
- It is a change notification. It stays pending until the GPRS registration state changes, and on
  the N95 it stayed pending all the time without blocking start-up (n95-startup-animation.md) [inferred].
  The TSY would complete it on a GPDS/NET registration indication. Nothing needs to answer it for
  start-up, and no ISI request follows it.

## Where the SIM state comes from on the 5730: SAServer, `SAStartupService.DLL`, ISI channel 35

The 9.3 build has no NokiaDSY SIM path. SAServer's start-up plugin (`SAStartupService.DLL`, code
0x80953c58) talks to PN_SIM itself:

- **Unsolicited `SIM_IND` (09 0xEF)**. This is the subscription SAServer registers on channel 35.
  Handler 0x80955516 / 0x8095555e [verified]:
  - it accepts only resource 0x09, msg 0xEF;
  - it reads byte [10] = a and byte [11] = b (0x80954a0e);
  - **only a == 5 is used**. Then `state+32 = b` and `state+24 = 1` (0x809543b4).
- The stored status is evaluated in 0x80955faa [verified compares, inferred meaning]:
  - b = **0x01, 0x24, 0x4B, 0x1A** -> result 100 (0x64), the "SIM present/ready" branch [inferred];
  - b = **0x06** (and the error group 0x16, 0x1F, 0x2C, 0x2D) -> branch 0x809560a0, the
    "no SIM / SIM not usable" branch [inferred]. 0x06 is the same SIM_SERV_SIM_DISCONNECTED value that
    made the N95 publish 100052e9 = 5 (build-log run 42).
- The plugin can also send `SIM_STATUS_REQ` 0xC0 `<svc> 00` itself (0x80954964 / 0x80954950) and
  parse `0xC1` byte [11] (0x809549ea). The existing reply C0 -> C1 `00 06` already covers it
  [verified that the request id is 0xC0 and that byte 11 is read].

That this SA state feeds DosServer's SAE, which then publishes KPSSimStatus (101f75b6/100052e9) and
lets startup.exe enter its UI phase, is [inferred]. I found no aligned literal 0x100052E9 in the core,
so the publisher is in ROFS or builds the key differently.

## What the fake modem should send

When a channel subscribes to `(09 EF)` (SAServer, channel 35), the peer should send this once,
unsolicited, as a real modem does after its SIM server starts:

```
00 <KDevApe> 00 09 06 00 <obj> 00 00 EF 05 06      SIM_IND, length 12
```
- [10] = 0x05 is required: any other value is ignored [verified].
- [11] = 0x06 means SIM disconnected / no SIM [inferred from the branch grouping and the N95].
  Use 0x01 only when faking a present SIM.
- tid 00. Same framing as `DIscPeerChannel::Reply`, with receiver object = the subscribing channel,
  the way `MtcStateInd` sends its unsolicited indication.

Timing: send it right after the subscription (it may be queued until the client's Receive is
pending), or after the first C0/C1 exchange. It is idempotent, so sending again on a later subscription
is harmless [inferred]. No request needs an answer for this: the unanswered `0A 00`, `09 DE` and
`09 09` were also unanswered on the N95, which reached the idle screen [inferred, not on this path].

If startup.exe still waits after the indication, check (1) that SAServer's channel actually received
the EF (ISCPEER deliver log) and (2) for an SA command that makes SAStartupService send its own 0xC0
on channel 35. It must get C1 with byte [11] = 06; the current `00 06` reply already does that.

## Result of the first try (2026-11-07)

`[modem].indications = [{ res = 0x09, msg = 0xEF, data = "05 06" }]` sent the indication on both
subscribing channels, 35 (SAServer) and 7 (the TSY) [verified, `ISCPEER: ch N profile indication`].
- The TSY on channel 7 parses it with a longer layout: `c32exe.exe::CTelServer Panic IsiMessage:
  OFFS -9` right after it, then `C32Start Panic C32:CTelServer 4` [verified]. Without the
  indication neither panic happens.
- The reset after system state 117 did not change [verified].
So the indication is off in the profile again. Next: (1) an indication per channel (or a
TSY-compatible layout: find the subblock the TSY reads, as for NET_MODEM_REG_STATUS on the N95),
(2) check with the IPC-traced ROM what still blocks at 117 before the modem reset.

## Start-up passes (2026-11-07, later the same day)

What start-up waits on, found with the IPC-traced ROM (`syb_report.py` "likely blocked", then the
requests completed with -33 or -3 to sysstart right before system state 117):
1. **State 103**: sysstart calls SAServer function 101 (argument = the adaptation command); SAServer
   asks `09 C0` service 00 and waits for `SIM_IND`. Fix: `[modem].indications` entry with
   `channel = 35, after = "09 C0"` (the TSY on channel 7 must not get it) [V].
2. **State 103, command 107**: SAServer asks `09 C0` service 01 (SIM lock / autolock). SAStartupService
   0x80955ea6 maps status 8/9/10/1 to its results 108..111 and leaves anything else open. Fix:
   `[modem] sim_service_status = { "0x01" = 0x01 }` (OK, no lock) [V branch table, I meaning].
3. **State 104**: sysstart's ETel request 22006 (`RMobilePhone::GetLockInfo`) makes NOKIATSY send
   PN_SECURITY `08 67`; unanswered, ETel times out after 60 s (-33) and start-up shuts down. NOKIATSY
   0x3c64 (code offset of the RAM-loaded DLL) reads `08 68`: [10] = 1 else -2, [13] lock status,
   [14] lock setting. Fix: `reply 08 67 68 01 00 00 00 00` (unlocked, disabled) [V].
Then the first-boot queries (region, date, time) and the home screen and menu (L1). Seeded boot: idle
(`Startup sets 101f8766/46 = 104`) at 20 s.
