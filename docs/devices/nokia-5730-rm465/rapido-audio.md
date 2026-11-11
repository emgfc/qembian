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

## Proposed reply line (models/nokia-5730-rm465/isi-replies.txt)

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

## Playback requests (silent)

The ring-buffer side of playback (ctl 8's out words, F3 0x34/0x35 pacing, 0x36/0x37) was worked out
on the N8: `docs/devices/nokia-n8-rm596/rapido-audio.md`. Its `ring_units` stand-in config is off here.

Follow-up (static reading only). Blocker seen: `!CdspControlServer` after MdaifDriver ctl 8 sends
F3 0x32 `00 00 | 00 02 | 00 01 | 00 03 | 02 00 | 00 00` and waits for 0x33.

### What one DSP request does (cdspcontrolserver 0x80932118, one branch per command 7, 8, 9, 11, 12, ...) [verified]

1. **ctl 8** (AAMdaifApi ord 4) with a1 -> `{cmd, &o1, &o2, &o3}`. cmd is 0/1/2 for server commands 7/8/9.
   The three out words are pre-set to 0 by the client. cdspcontrolserver reads **only o1**, as
   `o1 / 1920` (cmd 0), `/ 960` (cmd 1) or `/ 320` (cmd 2), which becomes u16 @20 of 0x32. o1 = 0 is fine.
   **ctl 8 does not have to write anything** for this path [verified]. In aamdaif.dll (the PCM data path,
   not this blocker), o1/o2 are the size/offset passed to ctl 2 dir 1, so 0 means empty buffers [inferred].
2. Aamm ord 38 -> **F3 0x32**, then `CActiveSchedulerWait::Start`.
3. **0x33** -> Aamm 0x807bb954 -> observer slot 6 (cdspcontrolserver 0x809328b4) = `AsyncStop` only.
   The values are passed but ignored [verified].
4. Aamm ord 51 -> **F3 0x3B**, then **a second wait**. This is the next blocker once 0x33 is answered [verified].
5. **0x3C** -> Aamm 0x807bc94c -> observer slot 0 (0x80932812) = `AsyncStop` only [verified]. Then the
   server writes a status string (0x8093202c) and completes the request. No further wait, and no ctl
   9/11/12 [verified for commands 7 and 8; other commands use the same pattern, inferred].

The F3 sub-objects (+0x11c: 0x31/33/35/37, +0x120: 0x3C/3E/40/42/54, +0x124: 0x52/57/58) all report to
the same observer, cdspcontrolserver+0x68 (Aamm 0x807bb0c8) [verified]. Its slots:

| reply | slot | cdspcontrolserver action |
|---|---|---|
| 0x31 | 5 | memory map (above) + AsyncStop |
| 0x33 | 6 | AsyncStop |
| 0x37 | 7 | nothing (`bx lr`) |
| 0x35 | 8 | nothing |
| 0x3C / 0x3E / 0x40 | 0 / 1 / 2 | AsyncStop |
| 0x42 / 0x54 | 3 / 4 | nothing |

So cdspcontrolserver checks no status anywhere: any well-formed reply counts as success [verified].
Request -> reply pairs: 0x30->31, 0x32->33, 0x34->35, 0x36->37, 0x3B->3C (ord 51), 0x3D->3E (ord 36),
0x3F->40 (ord 39), 0x41->42 (ord 52, sent without a wait). cdspcontrolserver never sends 0x34 or 0x36: it
does not import their Aamm ordinals [verified].

### Field meanings (mock level)

0x32 request [verified offsets; meanings inferred]:

| offset | field |
|---|---|
| @10 | command index 0/1/2 |
| @12 | constant 2 |
| @14 | channels code (Aamm 0x807bcb18: 1 -> 0, 2 -> 1; so 1 = stereo) |
| @16 | raw format arg (3) |
| @18 | sample-rate code (0x0200, about 48 kHz) |
| @20 | buffer frames (o1 / 1920 ...) |

Parsing traps that **panic** inside Aamm (`User::Panic`) on bad values [verified]:
- **Sample-rate code** (Aamm 0x807bca9a). It must be one of 0x0002, 0x0004, 0x0008, 0x0010, 0x0020,
  0x0040, 0x0080, 0x0100, 0x0200, 0x0800 or 0x1000; anything else panics with code 6 (0 does too).
  These map to the TMMFSampleRate bits 8k, 11.025k, 12k, 16k, 22.05k, 24k, 32k, 44.1k, 48k, 88.2k and
  96k [inferred].
- **Channels code** (Aamm 0x807bcb34). It must be 0 or 1, else panic 0.

The reply fields and their checks:
- **0x33**: u16 @10 and u16 @12 (echo of the request, inferred), u16 @14 = sample-rate code (**must be valid**).
- **0x3C** and **0x3E**: u16 @10, u16 @12 (status/handle, inferred, unchecked).
- **0x40**: u16 @10.
- **0x35**: u16 @10, u32 @12.
- **0x37**: u16 @10, u32 @12, u16 @16 = channels code (0/1), u16 @18 = sample-rate code (**must be valid**).

All getters come from isimessage.dll (ords 22/23). "02 00" is valid whether it is read big-endian
(0x0200) or little-endian (0x0002) [verified set].

### Reply lines (models/nokia-5730-rm465/isi-replies.txt)

```
reply F3 32 33 00 00 00 02 02 00                          # DSP stream setup: @10/@12 echo, @14 rate code 0x0200 (Aamm 0x807bca9a panics on 0); cdsp slot 6 = AsyncStop only
reply F3 3B 3C 00 00 00 00                                # DSP request step 2 (Aamm ord 51): cdsp slot 0 = AsyncStop; values unchecked
reply F3 3D 3E 00 00 00 00                                # (Aamm ord 36) cdsp slot 1 = AsyncStop
reply F3 3F 40 00 00                                      # (Aamm ord 39) cdsp slot 2 = AsyncStop
reply F3 41 42 00 00                                      # (Aamm ord 52, no wait) ignored; stops "no reply" noise
reply F3 34 35 00 00 00 00 00 00                          # not sent by cdsp; harmless if another client sends it
reply F3 36 37 00 00 00 00 00 00 00 00 02 00              # not sent by cdsp; @16 channels 0 (mono), @18 rate 0x0200 (both checked)
```
Lines 1–4 are needed (3 and 4 for commands whose branch uses ord 36/39, inferred); lines 5–7 are optional.
All are 32 bytes or less, so the generator works without changes.

### MdaifDriver (aamdaifdriver.cpp)

- **ctl 8**: return 0 and write nothing. That is enough for cdspcontrolserver [verified]. The current
  default (KErrNone, no write) already does this.
- **ctl 9** (aamdaif ord 10; a1 is a small integer, 1 or 2, not a pointer) and **ctl 11**: return 0.
  Not used on this path [verified, cdspcontrolserver does not import them].
- **ctl 12**: only reached through AAMdaifApi ord 5 when ctl 10 returns 1. Keep ctl 10 at 0 [verified].
- Suggested change to the ctl-2 hook: when `w[3] == 1` (aamdaif's writable buffer, size taken from
  ctl 8's o1 = 0), write NULL instead of the ROM table pointer, so a zero-length `TPtr8` never points
  into ROM. Mapping a real buffer is out of scope [inferred].

Expected log after the change [inferred]: `ctl 8` -> F3 32 answered 33 -> F3 3B answered 3C -> the
request completes, the tone path finishes silently. If something still waits, look for an unanswered
F3 3D/3F or a resource 0x0B/0x77 request from the `!AudioServer` thread.
