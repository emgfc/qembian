# N8 Belle: why start-up ends in "emergency calls only" (105) with no SIM

Static analysis (2026-11-11, no QEMU run). Firmware: Delight core ROM
`build/roms/nokia-n8-rm596-delight/*/core-flat.img` (sastartupservice.dll code 0x80e6f478-0x80e74210),
compared with N8 011.012 (`build/roms/nokia-n8-rm596/*/`, sastartupservice at 0x80d86xxx). SF sources:
the Symbian Foundation `oss.FCL.sf.os.devicesrv` drop (`sysstatemgmt/`). Markers: [V] = read in the firmware
or in the SF source that the firmware demonstrably follows, [I] = inferred.

Disassemblies used (scratchpad, `tools/xipdis.py`): `sas-delight.dis`, `sas-011.dis`.

## 1. Who decides 105 and 103

### The state decision (sysstatemgr policy plugin) [V source, I that Belle's binary is the same]

`ssmpolicyplugins/ssmstartuppolicy/src/ssmstartuppolicy.cpp`:

```
case ESsmStateSecurityCheck: aNextState = SelectStateAfterSecurityCheck();
SelectStateAfterSecurityCheck():
    RProperty::Get(KPSUidStartup, KPSSimStatus, value)
    retVal = ESsmStateEmergencyCallsOnly;                      // default -> global state 105
    if (value == ESimUsable(102) || value == ESimNotSupported(105)) retVal = NonCritical;   // -> 104
    else if (value == ESimNotPresent(104) && IsSimlessOfflineSupported()) retVal = NonCritical;
IsSimlessOfflineSupported() = (feature 7 KFeatureIdOfflineMode || feature 6 KFeatureIdFlightMode)
                               && feature KFeatureIdFfSimlessOfflineSupport
```

`KPSSimStatus` values (`clayer/inc/simutils.h`, `KSimUtilsEnumerationFirstValue = 100`):
**100 uninitialised, 101 ESimUsable, 102 ESimReadable, 103 ESimNotReady, 104 ESimNotPresent,
105 ESimNotSupported** [V]. That matches the serial-log values (103 = NotReady, 104 = NotPresent).

So the rule is: after the security check the phone goes to CriticalPhaseOK only if KPSSimStatus is
101 (usable), 105 (no SIM supported) or 104 (SIM not present) with SIM-less offline enabled. **103
(ESimNotReady) always gives 105 EmergencyCallsOnly.** That is the measured N8 Belle case.

### Who writes KPSSimStatus [V source]

KPSSimStatus is a system-wide property (SwP) guarded by `custcmd/src/swppolicy_simstatus.cpp`:
transitions to ESimNotReady are **always allowed** (`case ESimNotReady: EAllowed`), so a later
NotReady overwrites an earlier NotPresent. Writers:

1. The security-check custom command `custcmd/src/cmdsimsecuritycheck.cpp` (runs during state 103):
   `ESIMPresent` -> adaptation answers ENo -> `SimRemoved()` -> SwP KPSSimStatus = **104**;
   then `ESIMLessOfflineSupported` (inform adaptation only) -> `ESIMLock` -> adaptation answers
   ESimLockOk -> `SetSecurityCheckOK()` -> `ESecurityCheckOK` (status stays EStrtSimlessOffline).
2. The SIM event observer `clayer/src/ssmsimstatusobserver.cpp` (NotifySimEvent on the SIM adaptation):
   `ESsmSimUsable -> 101, ESsmSimReadable -> 102, ESsmSimNotReady -> 103, ESsmSimRemoved -> 104`.
   The events come from the Nokia adaptation (sastartupservice.dll) through
   `clayer/src/startupadaptationadapter.cpp::EventL`:
   `EFatalError(100)+ESimRemoved(100) -> ESsmSimRemoved`, `ESimEvent(103)+ESimNotReady(102) -> ESsmSimNotReady`.

### The Belle path from the channel-35 answers to 104, 104, 103 [V disassembly]

sastartupservice.dll (Delight) keeps one shared state object (`this+20`); field `+28` is the last
SIM status byte, `+20` = "status known", `+76` = "SIM usable already reported", `+12` = current global state.

- `0x80e6fea4(obj, b)`: `obj[28] = b; obj[20] = 1`. Called from
  - the SIM_STATUS_RESP (09 C1) handler, service != 1 branch, 0x80e71b2c: `b = byte[12]` of the C1
    (= our `06`), and
  - the SIM_IND (09 EF) handler 0x80e71040/0x80e70ffa: if `byte[10] == 5` then `b = byte[11]`
    (our `05 06` stores 6 again).
- C1 service 1 (0x80e71a4e): status 1 -> 111 ESimLockOk, 8/9/10 -> 108/109/110, 0x24 -> 111 when
  no SIM was reported usable (`obj[76]==0`), else periodic retry. Same as 011.012 (0x80d89c54).
- ESIMPresent decision (0x80e71c34 ff.): status **1, 0x24, 0x4B, 0xFC -> EYes(100)** and `obj[76]=1`;
  **6 -> ENo (101)** [I: 0x80e71d52 stores r7, the no-value]; anything else -> SIM_STATUS_REQ is sent
  again (0x80e71cb0 -> 0x80e704f4, poll). So 6 is the only "no SIM" answer this code accepts.
- PIN mapping in the same handler: 2 -> 104 EPIN1Required, 3 -> 102 EPUK1Required, 44 -> 105 EUPIN,
  45 -> 103 EUPUK (0x80e71d0c-0x80e71d2a).
- **SIM event raiser 0x80e71056** (called from the channel-35 message dispatcher 0x80e6fe24 after *every*
  message handled by the SIM command object, when `this[84] != 0` [I: set by the first
  ESecurityStateChange command, jump-table case 1 at 0x80e6fcd4] and the stored status is not 0x4a/0x49):
  ```
  r5 = obj[28]  (stored SIM status);  state = obj[12]
  if obj[12] == 111 (BTSap) -> nothing
  cmd[24] == 0:
      if obj[76]==0 and r5 in {0x49, 0x4a}: raise ESimEvent(103) ESimUsable(100); obj[76]=1
      if r5 == 6:
          if state == 103 (SecurityCheck):                              <- Belle only
              raise EFatalError(100) with ESimRemoved(100)             -> SSM: KPSSimStatus = 104
              raise ESimEvent(103)  with ESimNotReady(102)             -> SSM: KPSSimStatus = 103
              obj[76] = 0            (cmd[24] stays 0: evaluated again on the next message)
          else:
              cmd[24] = 1; raise ESimEvent(103) ESimNotReady(102); obj[76] = 0
  cmd[24] == 1: ... (SIM back: 0x49/0x4a -> usable)
  ```
  011.012 (0x80d88f8a): `if r5 == 6: cmd[24] = 1; obj[76] = 0; raise ESimEvent(102)` - no state test,
  no EFatalError, and because `cmd[24]` becomes 1 it is raised **once** [V].

Putting it together with the measured log (N8 Belle): custcmd `SimRemoved` -> **104**; then on the next
SIM message on channel 35 that the dispatcher routes to the command object (our C1 `01 01`, the SIM-lock
answer) the raiser runs with stored status 6 in state 103 -> `EFatalError/ESimRemoved` -> **104** (second
write) and `ESimEvent/ESimNotReady` -> **103**; the custom command completes, the policy reads 103 ->
**105 ESwStateEmergencyCallsOnly** [V code path; I that the C1 `01 01` is the triggering message - the
raiser also runs on the earlier C1 `00 06` and SIM_IND, but those raises land before or are what the
log shows as the first 104/103; the log's "103 right after the service-01 reply" fits the re-run].
On 011.012 the single NotReady (103) is raised at the first C1 `00 06`, then the custcmd writes 104, and
104 wins. The 701/808 do not go through PN_SIM 09 at all (PN_UICC 8C) so this raiser never sees a 6.

Why the keys/Autolock/context framework follow: state 105 runs `emergencycallsonlycmdlist` and never
`ESsmStateNonCritical` -> no ESsmNormal, so the non-critical start-up list (Autolock, context framework,
sensor/rotation activation, key captures of the home screen) never runs [V cmd lists, I for each client].

## 2. What a real N8 Belle without SIM gets, and the MTC question

- `15 01 {0A 00}` is **MTC_STATE_REQ(target 0x0A = RF_INACTIVE)** built by 0x80e702dc (byte 9 = 1,
  byte 10 = target). It is sent from the ESecurityStateChange handler for `ESIMLessOfflineSupported`
  (SF: "inform adaptation only"): Belle's adaptation puts the modem RF off itself when there is no SIM
  [V the sender; I the command that triggers it, from ordering and the jump table case 6 at 0x80e71584].
  The response handler 0x80e71626 accepts `MTC_STATE_RESP` (0x64) **byte 10 == 0 (MTC_OK) or 6
  (MTC_ALREADY_ACTIVE)**; anything else = failure (-2 to the client). Our `64 {00 00}` is accepted.
  Channel 35 does not subscribe MTC_STATE_INFO_IND (15 C0) and nothing waits for one. **MTC is not the
  problem** [V].
- Service-01 SIM_STATUS: `01` -> ESimLockOk is right; `0x24` would also give ESimLockOk when no SIM was
  reported usable [V]. No change needed.
- The real no-SIM answer: this code accepts only **6** as "SIM not present" for ESIMPresent, and the same
  6 drives the NotReady raiser. On a real phone the raiser runs on the same messages, so the only
  difference can be *when* it runs relative to the custcmd's SimRemoved write - and the modem sends no
  further PN_SIM message to channel 35 between the ESIMPresent answer and the SIM-lock answer except the
  SIM-lock answer itself. I cannot show statically that a real SIM-less N8 Belle avoids this; N8 Belle
  phones do boot to "Offline" without a SIM in the field, so [I] the real SIM server answers the
  SIM-lock query (service 01) with a different status or the state is not 103 any more when the second
  evaluation runs. Emergency-calls-only is *not* the genuine SIM-less behaviour: the SF policy and the
  feature data (below) both allow SIM-less offline.

## 3. Feature flags (featmgr `Private/10205054/features.dat`, rofs1) [V]

Parsed with a 12-byte-entry reader (header 16 bytes "feat", version 1, count):
- id 7 `KFeatureIdOfflineMode`: supported (flags 1) on 011.012, 111.040 stock, Delight and the 701.
- id 6 `KFeatureIdFlightMode`: 0 everywhere.
- `KFeatureIdFfSimlessOfflineSupport`: its number is not in our SF tree. Every id whose supported bit
  differs between the four files was listed (192 ids); **no id is set on 011.012 and the 701 but clear on
  the N8 Belle files** (the pattern a SIM-less-offline difference would need, since 011.012 and 701 both
  pass the policy with ESimNotPresent). 111.040 stock and Delight have identical flags. So the feature
  data is not what differs [V data, I that the policy reads the same id on all three].

## Proposed fix (fake modem / profile, exact bytes)

Goal: the stored PN_SIM status in sastartupservice must not be 6 any more when the SIM-lock answer
(C1 `01 01`) arrives, while ESIMPresent still gets its 6.

**Fix A (profile only, N8 Belle firmwares; try first).** Add a second SIM_IND right after the existing one
so the stored status becomes a neutral value (0 = SIM_SERV_NOT_AVAIL in ISI naming; the raiser ignores
everything but 6/0x49/0x4a, and ESIMPresent has already been answered):

```toml
# models/nokia-n8-rm596/firmware/<111.040 stock, delight, reborn>/firmware.toml
[modem]
indications = [
  { res = 0x09, msg = 0xEF, data = "05 06", channel = 35, after = "09 C0" },  # as today: ESIMPresent = No
  { res = 0x09, msg = 0xEF, data = "05 00", channel = 35, after = "09 C0" },  # clears the stored 6
]
```
Frames on the wire (channel 35, after the C1 `00 06` reply): `09 EF 05 06` then `09 EF 05 00`.
Expected log: KPSSimStatus 104 (custcmd) with at most one 103 *before* it, no 103 after the `01 01`
reply, global state 104 -> 110. The 011.012 profile keeps the single indication (its raiser runs once
and the IND `05 06` is what answers ESIMPresent there, model.toml comment).

**If A does not change the order** (the events are raised at the C1 `00 06` but delivered late), the
raiser must not see a 6 at all after the ESIMPresent answer: answer SIM_STATUS service 00 with `06`
only for the *first* request and send the neutral IND; or, as a last resort, a code patch in
`code-patches.txt` for Delight at 0x80e71146 (`cmp r0,#103` -> `cmp r0,#0`, bytes `2867` -> `2800`),
which makes Belle's raiser behave like 011.012 (raise NotReady once, set cmd[24]=1, no EFatalError)
[V addresses; patch untested].

No MTC change: `15 01 {0A 00}` -> `15 64 {00 00}` is accepted as is.

## Result (main session, 2026-11-11) [V]

- Fix A (a second SIM_IND `05 00` after `05 06`) did not help. A breakpoint at the raiser's `cmp r5,#6`
  (0x80e7113e) showed it runs twice, status 6 then 0: the "not ready" event raised on the first run is
  processed after the SSM wrote 104, so 103 still wins.
- In any state other than 103, status 6 raises once and sets the done flag (0x80e711a6). Applied fix: the
  SIM_IND `{05 06}` on channel 35 at subscription (~9.7 s, state 100), as a modem reports a missing SIM when its
  SIM server starts; no `after`. Result on the Delight: KPSSimStatus 104, global state 104 at 33 s, 110 at 115 s
  (the 701 shows the same 104 -> 110 gap). Applied to 111.040.1511, Delight and Reborn firmware.toml.
- Still open after it: Menu does not open the menu at 110, Autolock does not run (101F8767/0x501 = 0).

## Real N8 comparison (2026-11-11) [V]

Real N8 Delight, no SIM (oracle `n8-boot.log`, not committed): KPSSimStatus 104, 103 -> 104 at 18.5 s -> 110 at
20.8 s, Autolock (lockapp 34.0 s, autolock 35.1 s, 101F8767/0x501 = 1). Real modem answers over USB phonet
(`tools/phonet_query.py`): `09 C0 {00 00}` -> `C1 {00 06 00 FF 00 00}`; `09 C0 {01 00}` -> `C1 {01 24}`;
`15 02` -> `65 {0A 0A}`; `31 11 {00 FF FF 00 00 00}` -> `12 {01}`; `09 2D {04 00}` -> `2E {04 FB}`.
Applied: replies `31 11 12 01` and `09 2D 2E 04 FB` (platform/isi/replies-bb5.txt), the MTC state query reports
the last requested state (syborg_modem.c). Result: 104 -> 110 in 12.7 s (was ~80 s), lockapp/autolock at
33.5/34.1 s, lock status 1, the Menu key opens the menu. Not applied: service-01 `24` (our start-up stops at
the security check with it; the real SIM server must send more). Still unanswered: GSS 0x54, 0x19, 0x30, 0x06.
Rotation on Belle: the context framework activates orientation (all contexts right), but accssy refuses a
property request on the XYZ channel with KErrNotReady (channel not open yet, accssy 0x80b1b38e) [V].
