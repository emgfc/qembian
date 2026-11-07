# 5730 (RM-465) RAPIDO audio start-up: MdaifDriver + ISI resource 0xF3 (mock level)

Scope: just enough to let `MmfAudioServer.exe`'s `!CdspControlServer` thread finish starting with
audio silent. Real playback (DSP memory map, shared buffers, the 0x32/0x34/0x36 requests) is out of
scope. Static reading of the 5730 core image only (no runs). Tags: [verified] = read in the
disassembly; [inferred] = best guess.

## Who does what

| Module (core) | Role |
|---|---|
| `cdspcontrolserver.dll` (code 0x80931ea8) | the `!CdspControlServer` server; its ConstructL (0x80931f18) runs the start-up handshake [verified] |
| `AAMdaifApi.dll` (code 0x80901368, 17 exports) | `RBusLogicalChannel` wrapper of "MdaifDriver"; **only `DoControl(fn, a1)` (EUser 490) and once `DoControl(fn, a1, a2)` (491); no DoRequest at all** [verified] |
| `Aamm.DLL` (code 0x807b81b8) | ISI framing/dispatch; builds the F3 messages (0x807bb740 = msg 0x30) and parses the replies (dispatcher 0x807bb670, F3 handler 0x807bbc24) [verified] |
| `AAMdaifDriver.ldd` | the kernel driver we replace (`src/standins/aamdaifdriver/`) |

Other importers of AAMdaifApi: `aamdaif.dll` (ords 4, 8, 10, 11, 12, 14, 16), `eapdsp.dll` and
`saaudioserver.dll` (ords 5, 12, 14, 16) [verified, stub scan].

## Start-up sequence (cdspcontrolserver ConstructL, 0x80931f18) [verified]

1. `CServer2::StartL`, `new CActiveSchedulerWait` (+0x70), Aamm ord 70 (ISI channel object, the
   channel-36 / resource list e2 open), Aamm ords 50/56 (F3 client object +0x78, observer at +0x68).
2. AAMdaifApi ord 12 = Open (`DoCreate("MdaifDriver", TVersion(1,0,1))`; on KErrNotFound
   `User::LoadLogicalDevice` and retry; failures panic "MdaifApi" 2/3; then a VariationMgr.DLL
   check selects a base 0x08000000 or 0x10000000 (else panic 4); second base 0x30000000).
3. AAMdaifApi ord 9 -> **DoControl 5**, a1 -> one TInt out word (pre-set to 0 by the client).
   The function returns that out word (not the DoControl result).
   - out != 0: ConstructL returns at once (DSP already set up; no ISI, no wait).
   - out == 0: Aamm ord 37 sends **F3 0x30** and `CActiveSchedulerWait::Start()` until the reply.
   The 4 words the probe logged `{0, 0x009028e8, 0x00902984, 0x80931f5d}` are the out word followed by
   the client's saved r4, r5, lr (return address in cdspcontrolserver) — only word 0 is the argument.
4. Reply 0x31 -> Aamm builds 4 entries -> observer (cdspcontrolserver 0x80932844): `AsyncStop`, then
   for i = 0..3 **DoControl 7** `{i, B_i, A_i, 0}`, then **DoControl 6** (a1 = NULL). Results of 7 and 6
   are ignored [verified].

## ISI resource 0xF3 (RAPIDO DSP / MDAIF server, name inferred)

Request 0x30 (Aamm 0x807bb740) [verified]: 12-byte message, header only:
`00 00 00 F3 06 00 <obj> 00 <tid> 30 00 00` — bytes 10..11 are fill, there is no argument.

Response **0x31** (Aamm 0x807bb98a) [verified layout, meaning inferred]:

| offset (whole message) | size | content |
|---|---|---|
| 8 | 1 | tid (echo) |
| 9 | 1 | 0x31 |
| 10..11 | 2 | not read (fill) |
| 12, 16, 20, 24 | 4 each | A_0..A_3 (TIsiReceiveC 32-bit get; ISI big-endian [inferred]) — likely DSP-side addresses |
| 28, 32, 36, 40 | 4 each | B_0..B_3 — likely sizes |

Total 44 bytes = **34 payload bytes** after the msg id. No status byte is checked: any 0x31 counts as
success; all-zero values are fine for a stand-in that ignores DoControl 7. Aamm's 32-bit getter
bounds-checks (cf. the N95 "IsiMessage: OFFS" panics) [inferred], so the reply must be the full length.
No indication is waited for during start-up [verified: ConstructL waits for exactly one callback].

Other F3 messages (out of scope, only on DSP requests from clients) [verified ids, inferred use]:
0x32 (22 bytes) -> 0x33 (handler reads u16 at 10, 12, 14), 0x34 (22 bytes) -> 0x35 (u16@10, u32@12),
0x36 (18 bytes) -> 0x37 (u16@10, u32@12, u16@16, u16@18). Dispatcher also routes 0x3C/0x3E/0x40/0x42/0x54
and 0x52/0x57/0x58 (indications, likely) to other Aamm objects; 0xF0 = common "unknown" reply.

## MdaifDriver controls (AAMdaifApi.dll export -> DoControl) [verified numbers/layouts]

| AAMdaifApi ord (addr) | DoControl | a1 layout | needed for start-up | stand-in |
|---|---|---|---|---|
| 9 (0x8090166c) | **5** | -> TInt out ("DSP set up?") | yes | write 0 (handshake) or 1 (skip it); return 0 |
| 7 (0x80901692) | **7** | -> {index 0..3, B_i, A_i, 0} | after 0x31 | return 0 |
| 2 (0x8090164a) | **6** | NULL | after 0x31 | return 0 |
| 1 (0x8090156e) | 2 | -> {0x30000000, 0x50, &outPtr, 0} | only if eapdsp/saaudioserver call ord 5 | write a **user-readable** pointer to a 0x50-byte table, return 0 (see below) |
| 11 (0x80901530) | 2 | -> {base+off, size, &outPtr, 1} | no (playback) | out of scope |
| 5 (0x809015d6) | (2, 10, 12) | lookup id in the ord-1 table | maybe (eapdsp, saaudioserver) | table with count 0 -> returns 0 |
| 3 (0x809015b0) | 10 | -> TInt out | via ord 5 with id 2 | write 0 (then DoControl 12 is skipped) |
| — (in ord 5) | 12 | `DoControl(12, &a, &b)` two TInt outs | only if ctl 10 gave 1 | out of scope |
| 4 (0x809016c2) | 8 | -> 4 words | per DSP request | return 0 |
| 6 (0x809014fe) | 3 | -> {&TInt out, &arg} | no | return 0 |
| 10 (0x809016f0) | 9 | caller pointer | no | return 0 |
| 8 (0x80901712) | 11 | -> {value of table id 2, NTickCount} | no | return 0 |
| 15 / 13 | 0 / 1 | -> {a, b, c} | no (aamdaif.dll) | return 0 |

Ord-1 table (read directly by user code, so the pointer must be readable in the client process)
[verified layout]: `+4` u16 count; entries from `+8`, 8 bytes each: `+0` u16 (unused), `+2` u16 id,
`+4` u32 value; ord 5 returns the value of the matching id, 0 if none. Mem::Copy of 0x50 bytes from
the pointer, so a NULL pointer would KERN-EXEC 3. Cheapest valid pointer: a 0x50-byte const block in
the stand-in LDD itself, which is XIP in ROM and readable from user mode [inferred].

## Proposed reply line (devices/nokia-5730-rm465/isi-replies.txt)

```
reply F3 30 31 len=34                                     # RAPIDO DSP start: 4 x {addr, size} = 0 (cdspcontrolserver ConstructL waits; Aamm 0x807bb98a reads u32 @12..40)
```
`tools/isi.py gen` limits replies to 32 payload bytes (`SIsiReply::iData[32]`, `n > 32` check):
raise both to 40 (only `iData` size and the check change; `iscpeer.inl` copies `iLen` bytes,
`KMaxMsg` permitting). Alternative without touching the generator, a special case in
`DIscPeerChannel` next to the PP/SIM ones:
```
if (res == 0xF3 && id == 0x30)	// RAPIDO DSP start (5730): 34 zero payload bytes
	{ TUint8 d[34]; memset(d, 0, sizeof(d)); Reply(res, tid, 0x31, d, sizeof(d)); return; }
```
names.txt:
```
res F3 PN_DSP_MDAIF?            inferred  # RAPIDO audio DSP (cdspcontrolserver via Aamm.DLL, 5730)
msg F3 30 DSP_START_REQ?          inferred  # no data; resp 31 = 4 x u32 @12 + 4 x u32 @28 -> MdaifDriver ctl 7 each, then ctl 6
```

## Proposed stand-in (src/standins/aamdaifdriver/aamdaifdriver.cpp)

```cpp
// AAMdaifDriver.ldd stand-in (5730, RAPIDO audio): AAMdaifApi.dll opens "MdaifDriver" (1.0.1) and
// uses only DoControl. Audio is silent: no DSP memory, the mapping controls are accepted and dropped.
// Protocol: docs/devices/nokia-5730-rm465/rapido-audio.md.
#include "nokiastub.h"

// ctl 2 table read by user code (AAMdaifApi ord 1/5): +4 u16 count = 0 -> every id looks up as 0.
// Kept in ROM (XIP LDD) so any client can read it; a non-zero marker keeps it out of .bss.
static const TUint32 KDspInfo[0x50 / 4] = { 0x4644444d /* "MDDF" marker, not read */ };

static TInt MdaifCtl(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* w)
	{
	switch (aFn)
		{
	case 5:		// "DSP already set up?" out word: 0 -> client sends ISI F3 0x30 and waits for 0x31
		{
		TInt v = 0;	// 1 would skip the ISI handshake entirely (fallback if no F3 reply)
		return Kern::ThreadRawWrite(aClient, a1, &v, sizeof(v));
		}
	case 2:		// {dspAddr, size, TAny** out, dir}: return a readable (here: ROM) user pointer
		{
		const TAny* p = KDspInfo;
		return Kern::ThreadRawWrite(aClient, (TAny*)w[2], &p, sizeof(p));
		}
	case 10:	// TInt out: 0 (ord 5 then skips ctl 12)
		{
		TInt v = 0;
		return Kern::ThreadRawWrite(aClient, a1, &v, sizeof(v));
		}
	case 6: case 7: case 8:	// map entry {i, size, addr, 0} / map done / DSP request: accepted
	default:
		return KErrNone;
		}
	}

NO_CONTROLS
NOKIA_STUB_LDD_CTLHOOK("MdaifDriver", KCtl, ETrue, MdaifCtl)
```
(The hook gets `w[]` = first 4 words at a1, already read by nokiastub; logging stays.)
Note: ctl 2 with dir 1 (ord 11, playback writes) would also get the ROM pointer — fine while nothing
plays; a playback mock must return a writable buffer there (out of scope).

## Expected result after the change [inferred]

Log: `ctl 5` -> ISCPEER F3 30 answered 31 -> `ctl 7` x4 (`[0 0 0 0]`, `[1 0 0 0]`, ...) -> `ctl 6`;
`!CdspControlServer` then enters its scheduler and MmfAudioServer start-up completes. If anything still
waits, the fallback is `v = 1` in ctl 5 (skips F3 entirely; ctls 7/6 never come).
