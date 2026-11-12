# N95 8GB (RM-320, S60 3.1): what the modem must send for "SIM present, ready, PIN off"

Static only (no runs). Source: Thumb disassembly of `NokiaDSY.DLL.elf` (the Ghidra C for these handlers is
stubs), `NOKIATSY.DLL.c`, `startup.exe`/`sae.exe` tables, the SIM-present log `build/instances/n95-test/serial.log`,
`docs/devices/nokia-n95-8gb-rm320/n95-startup-animation.md` (startup's SIM gate). [V] read in code/log, [I] guess.
Offsets: [3] resource, [8] tid, [9] msg, data from [10]. Keys below are DOS events (CommonDSY ord3/ord11) that sae
turns into P&S 101f75b6/100052xx.

## 1. NokiaDSY (channel 24) is a state machine; our `C1 01 06` parks it [V]

Subscriptions `FUN_808be8d6`: (9,EF) (9,ED) (8,10) (9,C1) (9,3C) (8,12) (0,2E) (-1,73). Each state owns one
"step" (send + parse, returns a result code); the state handler maps the code to the next state (factory
`FUN_808beae6`, switch table at `0x808beaf4`). Boot path, all [V]:

| state (ctor) | sends | parse (step) | result -> next |
|---|---|---|---|
| 1 `0x808bee70` | `09 C0 {00 00}` | `0x808bfb54`: [11] 01/1C/4B -> 3, 06/24 -> 4 | 3: DOS (0x500,1) -> state 3; 4: (0x500,2) (0x1000000b,4) -> state 11 (no SIM) |
| 3 `0x808bef62` | `08 11 {00 00}` SEC_STATE_REQ | `0x808bfd20`: [10] cause 05 -> 9; 16 -> ignore; 01/02/03/07/06 -> 14/10/12/8/15; 1A -> 16; 1E -> 17 | 9 -> state 4 (flag +0x24 = 1); 10..13,16 -> DOS (0x1000000b, 2/8/6/9/5) -> state 16 (PIN/PUK) |
| 4 `0x808bf29e`(1) | `09 C0 {01 00}` SIM-lock query | `0x808c006c`: [11] 01 -> 19, 09 -> 20, 08 -> 21, 24 -> 18, **else 1 = wait forever** | 19/20 (ctor flag set): DOS (0x10000008,2) + **(0x1000000b,7)** -> state 6; 21: (0x10000008,1) (0x1000000b,1) |
| 6 `0x808bf29e`(0) | `09 C0 {01 00}` again | same; passive step `0x808bfee0` as well | 19/20 remembered until result 7 (SIM ready) -> re-query -> publish as in 4 -> state 6 (steady state) |

Passive step `0x808bfee0` (present in every state): `09 ED` (no data) -> 7 "SIM ready"; `09 EF` [10]==05 with
[11]: 0B -> 7, 06 -> 5 (SIM removed -> state 15), 05/07 -> 16, 08 -> 21, 2A -> 22, 2B -> 23 (DOS (0x1000000b,1)),
04/0A/27/other -> nothing (**our `05 4A` is ignored**). `08 10` SEC_STATE_IND: [15]==5 -> 9, [15]==1 -> switch on
[11] (not needed). Other services in SIM_IND are ignored by the DSY.

Log check: `c1 00 01` -> state 3 (`08 11` sent), `12 05 00 00` -> state 4 (`c0 01 00` sent), **`c1 01 06` -> result 1,
nothing else ever happens** (4.585 s), so sae keeps 100052e9 = 1 / 100052f0 = 3 and startup stays at 213 [V log].

## 2. What sae/startup need [V]/[I]

startup's `CStartupPSObserver` (startup-animation.md, `0x833a9cc4`) continues only when `100052e9 == 8 && 100052f0 == 1`;
e9 in 2..7,9..11 or f0 == 2 shows "SIM problem". sae maps DOS 0x1000000b -> 100052e9 as value+1 (0->1, 4->5 seen
without SIM [V]), so the state-4 publish (0x1000000b,7) gives **e9 = 8** [V by arithmetic, I until seen]. 0x10000011 ->
100052f0 direct (3 at init, 1 in no-SIM state 11 `0x808bf7c2` [V]). With a SIM the DSY never publishes 0x10000011 on
the boot path; state 4 publishes (0x10000008,2) instead (0x10000008 -> 100052e8?). Whether sae then sets f0 = 1
itself is [I]: watch `SYBPROP ... 100052f0` after the fix; if it stays 3, the candidates are the SEC_STATE_IND
`08 10` path above (result 9) and sae's handling of 0x10000008 = 2 (not traced, budget).

## 3. Modem changes (channel 24 = NokiaDSY, channel 7 = NOKIATSY)

1. `reply 09 C0 {01 ..} -> C1 {01 01}` (SIM lock service: 01 = no lock) **[V, the blocker]**. Today modem.simst gives 06.
   01 09 would also pass (result 20). Keep `09 C0 {00 00} -> C1 {00 01 00 FF 00 00}` and `08 11 -> 12 {05 00 00}` [V].
2. Replace the `05 4A` indication. For channel 24 send, after the lock reply (`after = "09 C0 01"`, or simply after
   the second C0): `09 ED {}` (SIM_SERVER_READY_IND, 0 data bytes, result 7) **or** `09 EF {05 0B}` (service 05,
   SIM_SERV_INIT_OK) [V parser]. It is optional for boot (state 4 publishes on the lock reply alone); in state 6 it
   makes the DSY re-send `09 C0 {01 00}` once, which we answer again with `01 01` [V]. Never send `05 06` (SIM removed).
3. Channel 7 (NOKIATSY `FUN_808c83a6` [V]): reads [10],[11],[12] unconditionally and [14] for (01,02): an indication
   needs >= 3 data bytes (2 bytes = `IsiMessage` panic as on the 5730 [I]). "SIM ready" for ETel is
   `09 EF {0D 0B 08 00 00}` (service 0D, status 0B, [12] == 8 -> +0x38 = 1 and a callback). Send it to channel 7 after
   its own `09 C0 {00 00}` (3.133 s) or together with item 2. `(07,4F)` = data available (optional), `(05,06)` resets.
4. Nothing else is awaited: the DSY sends no PN_SECURITY code/PIN queries at boot (only 11/12, IND 10); NOKIATSY has
   no request timers, its `09 13` service-table, `09 1D` IMSI (already `1E {2D 01 ..}`), `09 29/2D/62/BC/D4/DC` are
   completions only (24/FB accepted) [V log + n8-sim-present-mock.md]. `09 3B {FA 00}` -> `3C` (state 8, result 1C ->
   wait, 2A -> 22, 2B -> 23) is only used on SIM re-insert (state 7), not at boot [V].

Proposed profile (`models/nokia-n95-8gb-rm320/model.toml`): modem.simst service 01 -> `01 01`; indications =
`[{res=0x09,msg=0xEF,data="05 0B",channel=24,after="09 C0 01",sim=true}, {res=0x09,msg=0xEF,data="0D 0B 08 00 00",channel=7,after="09 C0",sim=true}]`
(check that `after` can match the service byte; otherwise key both on the second `09 C0` of the channel).
