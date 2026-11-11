# N73: PN_SIM 0x3B "fa 00" (IMSI-equal / "SIM changed?" check)

Mock-level notes: just what the fake modem has to answer. Source: N73 core XIP, `Sys/Bin/NokiaDSY.DLL`
(ROM 0xF82B1C80, code Thumb; addresses below are ROM addresses). No QEMU run was made for this note.

## Who sends it

- [verified] **NokiaDSY.DLL** (the DosServer plugin, so ISC channel 24). Function 0xF82BA32A builds the
  payload `FA 00` and sends PN_SIM message 0x3B through the ISI send helper 0xF82B240A (same helper that
  sends SIM_STATUS_REQ 0xC0 `00 00` / `01 00` right after it).
- [verified] NokiaDSY maps DOS request ids to ISI requests in 0xF82BA49E: 800 -> SIM_STATUS_REQ `00 00`,
  804 -> SIM_STATUS_REQ `01 00`, **806 -> 0x3B `FA 00`**, 802 -> msg 0x11, 0x75/0x77 -> 0x2D/0x2B.
- [verified] Request 806 is issued once by NokiaDSY's own SIM start-up state machine (0xF82B99DA, kicked
  from 0xF82B9A34 together with 800 and 804). It sets a "806 pending" flag (object +48) and registers the
  transaction (vtable +24).

## What it waits for

PN_SIM response **0x3C** (dispatcher 0xF82BAC00: 0x3C -> 0xF82BA596, also handles 0x10, 0x12, 0x2C,
0x2E, 0xC1, 0xED, 0xEF). [verified]

Response payload (bytes after the message id), same convention as SIM_STATUS_RESP `<service> <status>`
in `src/standins/_shared/iscpeer.inl`:

| byte | value | meaning |
|---|---|---|
| 0 | `FA` | service type, echo of the request [inferred: not read by the handler] |
| 1 | cause | read as element 1 (`operator[]` with index 1) and compared [verified that index 1 is tested; that index 0 is the byte right after the msg id is inferred from the SIM_STATUS_RESP convention] |

Causes (names from oFono's `sim_isi_cause`, naming only):

| cause | name | NokiaDSY does |
|---|---|---|
| `2A` | SIM_SERV_IMSI_EQUAL | DOS event 820 with value 0 ("SIM not changed"), then event 807 [verified] |
| `2B` | SIM_SERV_IMSI_NOT_EQUAL | DOS event 820 with value 1, runs 0xF82BA534 (resets four stored items, 3..6, probably "SIM changed" clean-up [inferred]), then event 807 [verified] |
| anything else | e.g. `06`, `FB` | no 820, just event 807 [verified] |

Event 807 (= response to request 806, value 0, carries the tid) is posted for **every** 0x3C,
whatever the cause [verified: all branches join at 0xF82BA5CC -> 0xF82BA4F2(807, 0, tid)].
The state machine's 807 handler (0xF82B9B06) clears the "806 pending" flag and drops the pending
transaction (vtable +28) [verified]. Event 820 goes to 0xF82B9ABE, which forwards the value as DOS
key/event 0x10000009 to DOS clients [verified call; meaning "SIM changed" inferred].

So 0x3B is the "IMSI equal?" (SIM changed since last boot) query; a proposed name is
`SIM_IMSI_EQUAL_REQ` / `SIM_IMSI_EQUAL_RESP` [inferred]. The N95's sae/DSY does not ask it.

## Follow-up

- [verified] The 0x3C handler itself sends no further ISI request.
- [inferred] With 806 no longer pending, start-up should move on; the final KPSSimStatus (100052E9) write
  is in sae.exe (literal at 0xF9184EF0) and was not traced. If 100052E9 still stays 1 after this reply,
  the next thing to look at is the next unanswered request in the ISC log, not this one.

## Proposed stand-in

Canned reply (`models/nokia-n73-rm132/isi-replies.txt`, format `reply RES REQ RESP payload`):

```
reply 09 3B 3C FA 2A                                      # SIM IMSI-equal check: "same SIM" -> DOS 807 (+820=0)
```

or in `iscpeer.inl`, next to `case 0xC0`:

```cpp
case 0x3B:	// "IMSI equal?" {0xFA, 0} -> 0x3C {service, SIM_SERV_IMSI_EQUAL}: NokiaDSY posts DOS 807
	{
	TUint8 d[2] = { aLen > 10 ? m[10] : (TUint8)0xFA, 0x2A };
	Reply(res, tid, 0x3C, d, 2);
	return;
	}
```

`2A` is the safe choice: it completes the request and reports "SIM not changed" without the clean-up
path. If something must not see event 820 at all, use a neutral cause such as `06` (still completes 807).
