> **Verified 2026-11-06 (main agent), supersedes the word-3 guess below:** EAP 0 (16 bytes) is built by
> `eapgwc_ord8` from eapcontrolmodule `FUN_80a0dad4`: `{0, stream id, mode, FUN_80a0f7c4(rate enum),
> stereo (0/1), port count, port ids...}`. `FUN_80a0f7c4` is a switch8 (table decoded from
> core-flat.img at 0x80a0f7cc): rate enum 1..9 -> code 11..3 (8000..48000 Hz, descending), 13 -> 14
> (48 kHz), anything else -> 15. Checked live: AudioServer's stream struct for Crash Bandicoot was
> {1 port, enum 5 = 22050 Hz} (breakpoint on eapcontrolmodule_ord1) and its EAP 0 carried code 7;
> the startup jingle carries code 4 = 44100 Hz stereo. Implemented in src/standins/ehwa/ehwa.cpp.

# Raw PCM node: where the stream format reaches the DSP

Research note for `src/standins/ehwa/ehwa.cpp` (raw PCM node `c36a3ebc`, kind 1). Goal: the
stand-in plays each stream at its real sample rate and channel count, not a fixed 44100/2.
Tags: [verified] = read in code, [inferred] = best guess from code shape plus one log line.
Sources: `build/n95/ghidra/{RawGwc,AudioServer,eapcontrolmodule}.dll.c`, disassembly from
`tools/xipdis.py` (AudioServer `0x80a32e6e`, eapcontrolmodule `0x80a10e06`), `build/sydb/serial.log`.

## 0. Bottom line

| what | where | encoding |
|---|---|---|
| channels | raw node **cmd 7 `arg2`** (the "len" in our `node send cmd 00000007` log line) | 1 = mono, 2 = interleaved stereo, 3 = non-interleaved stereo [verified mapping, inferred meaning] |
| sample rate | **EAP message 0, 16 bytes** (`u16[3]`), sent to the EAP control node just before the raw node is created | EAP rate enum (table below) [verified table, inferred word position] |
| link between the two | EAP stream id: EAP 0 `u16[1]` = raw node create config `c[2]` = EAP 7/8 `u16[1]` | small integer (4, 7, ...) [inferred, strong] |

So `c[2]` is not a rate: it is the EAP (DSP mixer) stream id that the raw node feeds. The 44100/2
of the jingle and the "far too fast" game are both explained: the game opened a **mono** stream
(cmd 7 `arg2` = 1; EAP 7 lists a single mixer port twice, `000c 0000 ... 000c 0001`, where the
jingle lists two ports `0006 0000 ... 0007 0001`), at a rate given by its EAP 0 message.

## 1. Channels: cmd 7 [verified]

`RawGwc_ord2` (`0x80a13802`, called by `RawHwDevice` SetConfig `FUN_80a14284`) gets a 3-word task
config `{rate, mode, x}`, stores it at gwc+0x98..0xa0, maps `mode` 1→1, 2→2, 4→3 (anything else →
`KErrNotSupported`), and sends raw `{cmd 7, arg1 = gwc+0x8c (0), arg2 = mapped mode}` (`Gwc_ord25`).
The 1/2/4 input is the `TTaskConfig` stereo mode (`ETaskMono`/`ETaskInterleaved`/
`ETaskNonInterleaved`) [inferred; it matches the EAP port count in both logs, see §0].
The rate word (`TMMFSampleRate` bit, as in `AacDecGwc_ord3`) is only stored at gwc+0x98 and never
sent by `RawGwc` [verified: no other reference in RawGwc].

RawGwc_ord2 refuses to run unless the node exists (gwc+0x4c == 1), so cmd 7 always comes after
ctl 4 and can come again (another SetConfig) [verified for the state check].

## 2. Sample rate: AudioServer → eapcontrolmodule → EAP 0 message

- `AudioServer.dll` `0x80a32e6e` (Ghidra missed the function start) builds a 7-word stream struct
  and calls `eapcontrolmodule_ord1` (create EAP stream; out: stream id at stream+0x58) [verified]:
  - `[0]` = `AudioStream_ord51() ? 2 : 1` (port count = channels) [verified code, inferred meaning]
  - `[1]` = rate enum = `FUN_80a33b78(TMMFSampleRate bit)`; 13 when `AudioStream_ord54()==11`
  - `[2]` = 1, `[3]` = `AudioStream_ord53()` 1→0, 3→1, else 2, `[4..6]` = 0
- `eapcontrolmodule` `FUN_80a0fbe8` copies `[1]` into the stream record (+0xc; 13 is stored as 9),
  `[3]` into +0x10 and appends `[0]` mixer port ids [verified].
- Rate enum → Hz (`eapcontrolmodule` `FUN_80a10e06`, a switch8 with literals at `0x80a111e4..`)
  [verified], same order as AudioServer's bit → enum (`FUN_80a33b78`) [verified]:

| enum | Hz | TMMFSampleRate bit |
|---|---|---|
| 0 | 5513 | — |
| 1 | 8000 | 0x1 |
| 2 | 11025 | 0x2 |
| 3 | 12000 | 0x200 |
| 4 | 16000 | 0x4 |
| 5 | 22050 | 0x8 |
| 6 | 24000 | 0x400 |
| 7 | 32000 | 0x10 |
| 8 | 44100 | 0x20 |
| 9 (and 13) | 48000 | 0x40 |
| 10 | 64000 | 0x800 |
| 11 | 88200 | 0x80 |
| 12 | 96000 | 0x100 |
| 14 | none (record default) | other |

- The message that carries it: in `build/sydb/serial.log` (line 3750) the EAP control node gets a
  16-byte EAP 0 just before the raw node is created:
  `0000 0004 0000 0007 0000 0001 0006 0303` → node 2 kind 1 `config 00000004`, `cmd 7 len 1`,
  EAP 7 ports `[6,0] [6,1]`. Read as `{cmd 0, stream id 4, 0, rate enum 7, 0, port count 1,
  port 6, ?}` [inferred: the field order follows the stream record; only this one sample was
  decoded]. The long (504-byte) EAP 0 messages later in that log are something else
  (parameter download); match only `len == 16`.
- EAP 7 / EAP 8 (`u16[1]` = stream id) carry per-port records `{port, channel index, gain words}`
  (0x4000/0x7fb2 etc. look like Q14/Q15 gains, `0x00ff` pads the list) and no rate [inferred].

## 3. Can the format change later?

- Channels: yes, a new SetConfig sends cmd 7 again on the same node [verified path]. The stand-in
  should apply cmd 7 whenever it arrives.
- Rate: it belongs to the EAP stream, created per DevSound stream; a different rate means a new
  EAP stream (new EAP 0 with a new or reused id) and normally a new raw node [inferred]. The
  stand-in should look the rate up by stream id when the raw node is created **and** again at cmd
  7/cmd 3 (start), so a late EAP 0 is still picked up. Re-programming the device rate per block
  (as `Pump` already does from the node) then just works.

## 4. Proposed stand-in code (ehwa.cpp)

```cpp
// EAP stream formats: EAP message 0 (16 bytes) {0, id, 0, rate enum, 0, ports, port...}
const TInt KMaxEapStreams = 32;
static TInt EapRate[KMaxEapStreams];		// Hz, 0 = unknown
static TInt EapPorts[KMaxEapStreams];

static TInt EapRateHz(TInt aEnum)	// eapcontrolmodule FUN_80a10e06
	{
	static const TInt KHz[] = { 5513, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100,
								48000, 64000, 88200, 96000, 48000 };
	return aEnum >= 0 && aEnum < 14 ? KHz[aEnum] : 0;
	}

// in 0x8000014, EAP form (m[2] == 0x20000000), after reading pl[]:
if (eapCmd == 0 && m[4] == 16)
	{
	TInt id = pl[1];
	if (id >= 0 && id < KMaxEapStreams)
		{
		EapRate[id] = EapRateHz(pl[3]);
		EapPorts[id] = pl[5];
		Kern::Printf("SYBSTUB: Hwa eap stream %d rate enum %d -> %d Hz, %d ports", id, pl[3],
					 EapRate[id], EapPorts[id]);
		}
	}

// SNode gets: TInt iEapId;   (c[2] at ctl 4)
// ctl 4, raw node:
Nodes[i].iEapId = c[2];
Nodes[i].iRate = (c[2] < KMaxEapStreams && EapRate[c[2]]) ? EapRate[c[2]] : 44100;
Nodes[i].iChannels = (c[2] < KMaxEapStreams && EapPorts[c[2]] == 1) ? 1 : 2;

// raw command 7 (m[2] == 7): arg2 = stereo mode 1 mono, 2 interleaved, 3 non-interleaved
if (m[2] == 7)
	{
	n->iChannels = m[4] == 1 ? 1 : 2;		// 3 (non-interleaved) not seen; treat as 2 for now
	if (n->iEapId >= 0 && n->iEapId < KMaxEapStreams && EapRate[n->iEapId])
		n->iRate = EapRate[n->iEapId];
	Kern::Printf("SYBSTUB: Hwa raw node %d format: eap stream %d, mode %d -> %d Hz %d ch",
				 n - Nodes, n->iEapId, m[4], n->iRate, n->iChannels);
	}
```

The EAP 0 dump needs the payload read for `eapCmd == 0` with `len 16` (the current code already
reads `pl[]` for every EAP command except 1). Fallback stays 44100/2, so the jingle is unchanged
even if its EAP 0 is missed; with EAP 0 seen it should decode as enum 8 = 44100 and cmd 7
`arg2` = 2 [inferred: the jingle's EAP 0 was not in the logs examined]. Check: the jingle run must
print `rate enum 8`; if it prints something else, the word position in §2 is wrong.
