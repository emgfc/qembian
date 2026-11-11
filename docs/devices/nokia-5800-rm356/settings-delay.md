# Settings ignores an immediate tap: wrong SIM service in the modem reply

Investigated 2026-11-09, Nokia 5800 RM-356 V31.0.101. **Cause confirmed by an A/B/A
experiment.** This covers main menu → Settings → tap Phone immediately after the
Settings list appears. It does not establish the cause of every slow UI action.

## What blocks

Settings draws its list, then makes ETel custom IPC **1000012, GetAlsBlocked**, on
its UI thread. The request asks the SIM for service **0x37** through PN_SIM message
0x29. Our shared canned response to message 0x29 always says service **0x35**:

```
request:  00 00 00 09 06 00 00 00 06 29 37 00
response: 00 6c 00 09 06 00 07 00 06 2a 35 24
                                      ^^ wrong service
```

The transaction ID is echoed correctly. The service discriminator is not. The
telephony request remains pending until its roughly ten-second timeout, returning
**-33 (KErrTimedOut)**. The already-drawn Settings window consequently cannot
handle the queued tap until that call returns.

Origin: `platform/isi/replies-bb5.txt` contains `reply 09 29 2A 35 24`, introduced
for an N73 request and inherited by the 5800. The canned-reply lookup in
`src/standins/_shared/iscpeer.inl` matches only resource and message ID, not the
service byte. Both ISC stand-ins include this generated table.

Source corroboration (the source is a guide, not proof of firmware identity):
`oss.FCL.sf.os.cellularsrv/telephonyserverplugins/common_tsy/customapi/inc/RMmCustomAPI.h`
names opcode 1000012; `customapi/src/RMmCustomAPI.cpp` implements its synchronous
overload with `Get(ECustomGetAlsBlockedIPC, ptr)`.
`oss.FCL.sf.mw.phonesrv/cellular/sssettings/src/rsssettings.cpp`,
`RSSSettings::Get(ESSSettingsAlsBlocking)`, calls that overload and maps any error
to “ALS blocking not supported.” These sources are in the Symbian Foundation (EPL) source drops.
The full UI caller stack was not captured; the live IPC and intervention below
establish the blocking request independently.

## Controlled experiment [verified]

Isolated session: `build/sydb-delay-5800`, seeded from
`build/seeds/nokia-5800-rm356`. No owner's drives or source code were changed.
KIPC was enabled through sydb for each run. Open Settings from its bottom-left
main-menu icon, then tap Phone twice approximately 1.4 seconds later (the list is
visible by then). Capture screens every 250 ms for about 18 seconds.

Only the two canned-table service bytes in the running VM were changed with GDB;
no timing, graphics, WSERV, SIM status, or UI code was changed.

| Run | Reply payload | IPC start → completion (serial seconds) | Duration | Result |
|---|---|---|---|---|
| Original | `35 24` | 185.853 → 195.696 | 9.843 s | -33, timed out |
| Correct service | `37 24` | 240.009 → 240.010 | 0.001 s | -1, not found |
| Original restored | `35 24` | 278.455 → 288.642 | 10.187 s | -33, timed out |

The corrected reply retains the existing negative SIM status (0x24); it does not
pretend ALS is implemented. It makes the negative answer consumable immediately.
The patched run reaches the Phone page by 1.935 s from opening Settings; the
original run remains on Settings through the timeout. Returning the bytes to
0x35 reproduces the stall. This rules out rendering throughput as the cause of
this particular pause.

Local evidence (git-ignored): `build/sydb-delay-5800/serial.log`,
`als-evidence.txt`, `second-timing.txt`, `patched-timing.txt`,
`reverted-timing.txt`, corresponding PNG sequences, and `reproduce.py`.
The reproduction script assumes the main menu is already visible and Settings
is closed. Screenshot and input commands use the sydb daemon directly.

For this exact composed ROM, the table entries were found at guest addresses
0x82744d54 and 0x8274ed24, each beginning `09 29 2a 02 35 24` followed by 62 zero
bytes. The intervention changed byte +4 to 0x37 using `sydb dbg write`, verified
the memory, and detached. **Do not reuse these addresses on another ROM/build.**
Both bytes were restored, and the test session was stopped afterward.

## Static firmware verification [verified, 5800 V31.0.101]

Disassembled the original `NOKIATSY.DLL` with `tools/xipdis.py`, using
`build/roms/nokia-5800-rm356/<fw>/core-flat.img` and `core.json`. Local output:
`build/sydb-delay-5800/nokiatsy.dis`. The following addresses refer to original
firmware code, not our rebuilt kernel or guessed nearest-export symbol names.

| Address | Firmware behavior |
|---|---|
| 0x809aa972–0x809aa98a | Selects service 0x37 and calls the request builder at 0x809a6a2a. |
| 0x809a6a2a–0x809a6a86 | Builds payload `[service, 0]`; service 0x38 has additional data. Sends resource 9, message 0x29. |
| 0x809aa3c4 / 0x809aa42c–0x809aa430 | Dispatches response message 0x2A to 0x809a6a8a. |
| 0x809a6a92–0x809a6a9c | Reads byte 10 through isimessage.dll ordinal 20 and compares it with 0x37. |
| 0x809a6afa–0x809a6afc | The alternative accepted service is 0x38. A 0x35 response takes the return path without completing the ALS request. |
| 0x809a6a9e–0x809a6aae | Reads byte 11 (status) and passes it to the firmware error converter at 0x809b2834, selector 1. |
| 0x809a6ab8–0x809a6ad8 | Negative results skip reading byte 12; success reads the ALS state there. A two-byte negative payload therefore does not omit required success data. |
| 0x809a6ae0–0x809a6aee | Loads literal 0x000f425d from 0x809a6b8c, subtracts 17, and calls the completion helper with IPC **1000012** and the converted result. |

This explains both observations: the old service is discarded before completion,
whereas the correctly labelled negative reply reaches the normal error path.
It is not an application patch, forced success, reduced timeout, or injected input
event. The existing modem model still supplies “data unavailable,” rather than
implementing ALS or simulating a provisioned SIM. The correction is to the reply's
identity; full modem fidelity is a separate limitation.

## Implemented repair and compatibility limits

For PN_SIM 0x29 replies, preserve the requested service byte (request offset 10)
and return the appropriate negative status for an unsupported service. For the
observed 0x37 request, response 0x2A with payload `37 24` is verified to complete
the request immediately. Keep the existing N73 service-0x35 behavior; globally
changing the static entry from 0x35 to 0x37 would simply break the other case.
Implemented in `src/standins/_shared/iscpeer.inl`: for resource 9/message 0x29
with at least 12 request bytes, reply 0x2A with `[request[10], 0x24]`. Removed the
constant-service table entry. The ordinary reply helper still echoes the
transaction ID and constructs the framing.

This is **not version-gated**. An N73 service-0x35 request gets exactly the old
`35 24` payload. Requests with other service values now receive that value back.
Regression 2026-11-10: N95 and 5800 smoke tests pass; the N73 (fresh boot) sends service
0x35 and gets the same `2a 35 24` as before. The handler now shares the 0x1F/0x1D
"data not available" branch (same reply shape: requested service, 0x24). Other services/firmwares could require
different negative statuses or layouts; declare such differences in the device
profile if demonstrated, rather than guessing from argument values.

The native QEMU binary and 5800 stand-ins/ROM were rebuilt on 2026-11-09. The fix
lives in the guest stand-ins inside `build/roms/nokia-5800-rm356/<fw>/hybrid.img`;
rebuilding QEMU alone does not install it. A fresh test session using the rebuilt
ROM (`build/sydb-delay-fixed`) completes IPC 1000012 from 33.430 to 33.431 seconds,
result -1, without GDB memory edits. The 5800 smoke test passed boot, agent and
non-black-screen checks; power-menu shutdown is a declared skip for this device.
Logs: `build/sydb-delay-5800/rebuild-rom.log`, `rebuild-qemu.log`, `smoke-fixed.log`,
and `build/sydb-delay-fixed/{serial.log,timing.txt,reproduce.py,fixed-*.png}`.

## Earlier N8/WSERV hypothesis

The existing N8 Delight log (`build/sydb-delight/serial.log`, 181.341 s) contains
the **same** request `29 37 00` and wrong response `2a 35 24`. This establishes
that the protocol defect also occurs there. The corrective A/B/A test was run
only on the 5800; it does not prove that every N8 delay has this cause. With the fix built into
the N8 Delight ROM (2026-11-10) the query completes in 2 ms and the sub-views that took 3.7-8.4 s
open in 1.2-1.4 s (numbers in the N8 note).

Do not infer a blocked synchronous flush from WSERV opcode 65545. In the
available `w32cmd.h`, 65545 is 0x10000 + EWsClOpRedrawReady (9), an asynchronous
redraw notification; 4 is EWservMessSyncMsgBuf. The mapping still needs checking
against each firmware. `ipc_latency.py` pairs by client name and opcode without
a message/session identifier; notification waits, tracing gaps, client renames,
and same-opcode calls to different servers can mislead its duration reports.
The table above uses the actual request, modem exchange, and matching completion
for the unique synchronous custom opcode, not a generic slow-request ranking.
