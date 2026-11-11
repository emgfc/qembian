# N8 (RM-596): unanswered ISI requests, mock level

Static reading of the N8 core image (Ghidra decompiles in `build/roms/nokia-n8-rm596/<fw>/ghidra/`),
log `build/sydb-n8/serial.log`. No runs. [V] = read in code, [I] = best guess.
Offsets are whole-message offsets ([3] resource, [8] tid, [9] msg id, data from [10]).
isimessage.dll: ord20/22/23 = 8/16/32-bit get, **big-endian** [V]; ord18 FindSubBlock type 4 =
`id16(BE) len16(BE)` (switch case at 0x805e3974: ord22 at +0, ord22 at +2), scanning from the given
offset while `pos + 4 <= length`, stepping by the subblock length (must be non-zero) [V].

## 09 DE -> 09 DF (NOKIATSY, channel 7, ~41 s and ~61 s)

- Client: nokiatsy.dll `FUN_80d9ae3e` builds `[10]=svc [11..14]=0 [15]=1 [16..17]=sb id [18..19]=0`.
  One TSY request (0x80d9b65a area) sends three messages together: `09 DE` svc 1 sb 0x0190 (tid 7),
  `15 16` (answered) and `09 09` svc 0x97 — exactly the 60.90 s burst in the log [V].
  Svc 3 / sb 0x0191 is a second use (a string of up to 16 bytes) not seen in the log.
- Parser: `FUN_80d9bb1e` (dispatch table 0x80d9c8xx, resource 09 msg DF) [V]:
  - `[10]` = svc; `!= 3` takes the svc-1 path. `[11]` = status, **1 = OK**.
  - finds sb **0x0190** (type 4) from offset 16; needs `[sb+4] == 1`; then `[sb+5]`:
    `0` -> result value 0, error 0; `3` -> value 1 (and sets object+0x34), error 0.
  - anything else (status != 1, no sb, other values) -> completes with **-2**, value 2.
  - The result goes to the waiting TSY request through `FUN_80d9712c` [V]; what 0/1/2 mean is [I]
    (0 = the plain case, 1 = a special card mode, 2 = failed/unknown).
- Proposed: OK, value 0 (no special mode). No-SIM phones: no evidence they answer with an error,
  and an error completion (-2) is the riskier choice for start-up [I].

## 09 09 -> 09 0A (NOKIATSY, same burst)

- Client: `FUN_80d9af9e` sends `[10]=svc [11]=0`, svc 0x97 (or 1) [V].
- Parser: `FUN_80d9bcac` [V]: reads `[10]` (svc) and `[11]` (status), `[8]` (tid).
  - `[10] == 0x9B`: a trigger path that re-sends `09 09` (svc 1 or 0x97); do not use it.
  - otherwise every status ends in `FUN_80d9b1c0(this, 7, 0, 2, 0)` (the follow-up of the same
    TSY request), with a flag on the way: status 2/0x22/0x23 -> object+0x50 = 1; status 0x20 with
    svc 0x97 -> object+0x38 = 0; any other status -> object+0x54 = 1 [V].
- Proposed: `[10]=97 [11]=20` (the branch written for this very request) [I meaning].
  If start-up then shows a SIM error note, try 0x22 (sets +0x50 instead).

## 1C 9E -> response (hapticsadaptation.dll, hwrmhapticsserver channel 12, every ~3 s)

- Request (`FUN_80d66fac`) [V]: `[10]=0 [11]=1 [12]=0x64 [14..15]=len-12 (BE) [16..19]=0
  [20..21]=data length (BE) [22..]=Immersion/VIBE packet` (first `12 00 01 00 00 00`, then
  `13 00 ff ff ff ff`).
- The adaptation runs a two-state machine (`FUN_80d670ea`): state 1 hands **whatever message
  arrives next on the channel** to `FUN_80d66e26`; the response id is not checked there [V]
  (0x9F is the usual REQ+1, [I]).
- Parser `FUN_80d66e26` [V]: success needs `[10] == 0`, `[11] != 0`, `[17] == 0`; then
  `[20..21]` = length of the packet at `[22..]`, which is passed to
  `hwrmhapticspluginservice` ord3/ord12 (the packetizer decodes it; not read here [I]).
  Otherwise error -2.
- Unanswered, the server times out after 3 s, `EwSrv.exe::FeedbackPlayerThread` dies -33 and the
  haptics server reopens the channel; start-up still reaches phoneui, so it is not the blocker [V log].
- Tried 2026-11-07: OK with an empty packet froze the UI (all idle: the plugin waits for a packet that
  never comes). Answered now with a failure status (`[10]=1`): the call fails at once, no feedback is
  played, nothing stalls (H-haptics-fail). It mattered more than thought: every touch feedback waited
  the 3 s, so start-up phase 3 went from 47 s to 30 s and an app start from ~6 s to ~1 s.

## 1C 38 -> 1C 39 (saaccessoryservice.dll, SAServer channel 35, 11.4 s)

- Request (`0x80d7f2xx`, `FUN_80d7ed18(this, 0x1C, 0x38, 0x39, ...)`) [V]: `[10]` = mode
  (1 or 2), `[11] = 0xFF`. Seen: mode 1.
- Response filter `FUN_80d7fe7a`: resource 1C, msg **39**, tid == request tid [V].
- Parser `FUN_80d7f54e` -> `FUN_80d7f4c8` -> `FUN_80d7f254` [V]: needs `[17] == 1` (else -6);
  reads `[10]`, `[11]`, **u32 `[12..15]`** (BE), `[16]`, `[19]`. u32 == 1 -> state (1,1) and done;
  == 2 -> (3, 0xB); other values go through the `[10]/[11]/[16]` accessory-type decoding.
- Proposed: u32 = 1, i.e. "nothing connected" [I meaning], everything else 0.

## Channel 7 at ~3 s: C8 0B, C8 36, C8 00, D9 00, 31 19, 31 30 (and 41 s: 09 13, 09 DC, 09 BA, C8 40, 1B 02)

Not analysed (budget). NOKIATSY on the N95 has no request timers: unanswered requests only leave
ETel requests pending [V on N95, I on N8]. On the N8 start-up passed them and reached
startup.exe/phoneui with all of them unanswered, so they are not the white-screen blocker [V log].
No profile answers them so far (GPDS 31 19/31 30, C8, D9 included).
