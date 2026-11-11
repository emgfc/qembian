# Nokia DSP bridge: what the fake DSP must do to play audio

Static analysis of `RawGwc.dll`, `RawHwDevice.dll`, `Gwc.dll`, `Bal.dll`, `BalBufferManager.dll`
(Ghidra output in `build/n95/ghidra/`, gaps filled with `tools/xipdis.py`). Builds on
`docs/nokia-dspbridge.md` (node create, 0x8000014 send, 0x8000005 listener, 0x8000015 receive);
nothing from there is repeated. Tags: [verified] = seen in code, [inferred] = best guess.

## 0. Bottom line

- Node `c36a3ebc-4bed09e5-8885d88f-06523118` is the **RAW PCM node** ("Raw" task): the UUID literal
  lives in `RawGwc.dll` (`0x80a13d4c`, `RawGwc_ord3+370`) and `RawGwc` is only imported by
  `RawHwDevice.dll` (the DevSound PCM HwDevice ECOM plugin) and `SbcEncHwDevice.dll` [verified].
  WAV playback, the Startup tone (WAV through `CMdaAudioPlayerUtility`) and key tones all end in
  this node; MP3 goes through `Mp3DecHwDevice.dll` → `Mp3DecGwc.dll` (same `Gwc.dll` shape, 26
  Gwc imports; its own UUID, not analysed) [verified for module shape, inferred for the path].
- **PCM format of a raw-node stream** (2026-11-06, docs/research/dsp-pcm-format.md): not in the node
  itself. eapcontrolmodule first creates an EAP stream with EAP message 0 (16 bytes)
  `{0, stream id, mode, rate code, stereo, port count, ports}`, rate code = `FUN_80a0f7c4` (3..11 =
  48000..8000 Hz, 14 = 48000, 15 = unsupported); the raw node's create config word is that stream id;
  RawGwc's cmd 7 `arg2` is the channel layout (1 mono, 2 interleaved, 3 non-interleaved). The
  stand-in (ehwa.cpp) plays each raw node with its stream's format [verified, live].
- The whole protocol for play-to-end is **4 ARM→DSP commands, of which only one needs an
  answer, plus one buffer control**: cmd 7 (config), cmd 3 (start), issue-buffer control
  `0x800000e` per PCM block (its `TRequestStatus` completion = "buffer consumed"), and one
  DSP→ARM message `{8, x, y}` after the last buffer = "stopped / play complete". Nothing else.
- **Why the player gave up**: our stub answered cmd 7 with `{0,0,0}`. `RawGwc`'s `MessageReceived`
  (`0x80a13aa0`) accepts exactly one message word: `u32[0] == 8`. Anything else →
  `KErrNotSupported` (−5) to the HwDevice → DevSound error → stop/delete [verified]. Commands
  7/3/1/4 expect **no reply at all** (no wait, no timer armed on this path) [verified].

## 1. Who creates what (play path)

`MMFDevSound` → `DevSoundAdaptationLayer` → `RawHwDevice` (`CMMFHwDevice`, object 0x44 bytes)
→ `RawGwc` (gateway, derives from `Gwc`) → `Bal`/`bridgeapi` → `ehwa`. [verified]

- `RawHwDevice::Init` (`0x80a140ec`): copies a 0x304-byte config block out of the HwDevice init
  args (`FUN_80a14502`): words `[0..2]`, four 0xac-byte stream definitions at `+0xc`, `[0xb3..]`
  buffer sizes, `[0xbf]/[0xc0]` = input/output enable (exactly one must be set, else −5). Then
  `RawGwc_ord4` (NewL, `Gwc_ord32` ctor, `Gwc_ord1`: `Bal_ord18` load + 3 active objects +
  timer) and `RawGwc_ord1(config)` [verified].
- `RawGwc_ord1` (`0x80a13846`) = node create: `Bal_ord25` (node attrs, 2 bytes in/out flags),
  `Bal_ord16/17` (`bridgeapi_ord3`: attach stream def 0 (out) or 1 (in), 0xac bytes), `Gwc_ord5`
  (buffer manager, `Bal_ord10`), `BalBufferManager_ord3` ×2 (buffer pools from `config[0xb3..]`
  / `[0xb9..]`: `{index, count, size, .., type@+0x10, ..}` 0x18 bytes; type 2 → `BalBuffers_ord12`,
  else `BalBuffers_ord7` — whether that is the `0x4000016` DSP chunk or heap was not resolved,
  the stub does not care, see §3), `Bal_ord1` (ctl 4 node create with args
  `{&dirFlag, config[0], config[1], config[2]}`), `Gwc_ord17/18` (bind pool to the in/out stream
  AO), `Bal_ord28` (`0x8000007`), `Gwc_ord22` (`0x8000005` listener) [verified].
- `config[0..2]` [inferred, strong]: a `TMMFCapabilities`-like triple `{rate mask, encoding,
  channels}`: `RawGwc_ord2` maps `config[1]` 1→1, 2→2, 4→3 (TMMFSoundEncoding 8-bit PCM /
  16-bit PCM / A-law) and the startup tone sent `arg2 = 2` = 16-bit PCM. Rate is a Symbian
  `TMMFSampleRate` bit (1=8000, 2=11025, 4=16000, 8=22050, 0x10=32000, 0x20=44100, 0x40=48000,
  0x200=12000, 0x400=24000), channels 1/2. These three words are `p1, p2, p3` of the ctl 4 block
  (`TBuf8<0x14>{p1=config[0], p0=&dir, bridgeNodeStruct*, p2=config[1], p3=config[2]}`), so the
  stub can read rate/encoding/channels at node create. Validate by logging ctl 4 once.

## 2. Message set (ARM→DSP via `0x8000014`, 12-byte raw form `{cmd, arg1, arg2}`)

| cmd | sent by | args | reply expected | who checks |
|---|---|---|---|---|
| 7 | `RawGwc_ord2` (`0x80a13802`), from HwDevice `FUN_80a14284` (SetConfig, before Start) | `arg1` = 0, `arg2` = encoding code 1/2/3 (§1) | **none**; `Bal_ord23` only waits for the send status | any message ≠ `{8,..}` → error −5 [verified] |
| 3 | `Gwc_ord30(mode=1 play, cmd=3)` from HwDevice Start (`0x80a14060..0x80a14096`) → vtable+0x38 (`0x80a13aef`) → `Gwc_ord9` → `{3, stale, stale}` | arg1/arg2 = leftovers of `Gwc+0x3c/+0x40` | none | — |
| 2 | same, record (`mode=2, cmd=2`) [inferred from the second Start branch] | | none | — |
| 4 | `Gwc_ord29` (Pause) → vtable+0x40 → `Gwc_ord8(4)` | `{4, stale, stale}` | none | — |
| 1 | `Gwc_ord28(gwc, 1, 0)` (HwDevice `0x80a140a0`, a Stop variant) → vtable+0x3c → `Gwc_ord7(1)` | `{1, ..}` | none | the normal stop/teardown path uses `Gwc_ord28(gwc,0,0)` and sends nothing |

DSP→ARM (listener → `0x8000015`, raw 12-byte form, `iCmd != 0x20000000`):

| word0 | meaning | handling |
|---|---|---|
| 8 | "stopped / stream finished" | `RawGwc` rewrites it to 2 and calls HwDevice slot +0x14 (`0x80a1432c`): `Gwc_ord28(gwc,0,0)` (cancel both stream AOs, no DSP command), then if the HwDevice is not already stopping (`obj+0x3c == 0`) → `MMFHwDeviceObserver::Stopped()` → DevSound play complete [verified] |
| anything else | — | `KErrNotSupported` → HwDevice `Error()` → DevSound stops. **Never send.** [verified] |

Start order seen at run time: ctl 4, 5, `0x8000007`, `0x8000005`, cmd 7 (then our bad reply
killed it; cmd 3 and the first buffer were never reached). [verified]

## 3. Data path (play)

1. `Gwc_ord30(1, 3)`: sends cmd 3, then `FUN_80a42fcc(outAO)`: takes a buffer from the pool
   (`BalBufferManager_ord2`), calls `RawGwc` slot `FillThisBuffer` (`0x80a13a72`: buffer
   `Offset(+4)` then `Gwc_ord12` → HwDevice `0x80a1428e`), which wraps it in a `CMMFPtrBuffer`
   and calls DevSound `FillThisHwBuffer`. The client writes PCM starting 4 bytes into the DSP
   buffer [verified for the +4/−4 calls, inferred that they are "skip header"].
2. `ThisHwBufferFilled` (`0x80a141f0`): copies the MMF buffer descriptor back into the DSP
   buffer (`FUN_80a145a4`, 3 words at buffer+0x10 = `TPtr8{type|len, max, ptr}`), sets
   `buffer+4 = iLastBuffer`, calls `RawGwc_ord3` (`0x80a139dc`): `Offset(−4)`, header bytes
   `data[0..2] = 0`, `data[3] = 0x10` if last buffer or length 0 else 0, then `Gwc_ord19` →
   `Bal_ord6` → **control `0x800000e`** [verified]:
   - a1 = node token; a2 → 7 words `{ TPtr8 type|len, TPtr8 max, user ptr, TRequestStatus*,
     client data length, &outAO.iOut (TInt), stream index }` (`FUN_80a41f2c`; `len`/`ptr`
     describe header+PCM in the caller's address space; the status is set to `KRequestPending`
     with flag bit 1 before the call) [verified].
   - **Kernel must complete the status with 0 when the data has been consumed** → outAO `RunL`
     (`0x80a438c4`) → `Gwc_ord15` → HwDevice `0x80a142f6` returns "continue" = (last length ≠ 0
     && !iLastBuffer) → next `FUN_80a42fcc` → next `FillThisBuffer`. After the EOS buffer the
     client stops requesting and waits for message `{8,..}` [verified].
   - Cancel: `CActive::Cancel` → `DoCancel` (`0x80a438fe`) → `Bal_ord9` = **`0x800000f`**
     (a1 = node, a2 = stream index) then `User::WaitForRequest`: the stub must complete the
     pending `0x800000e` status (`KErrCancel`) when `0x800000f` arrives, or teardown hangs
     [verified].
3. Record (not needed now): `Bal_ord5` = `0x8000010` issues an empty buffer (same block, data
   length 0), completion length is written to word[5]; cancel = `Bal_ord7` = `0x8000011`
   [verified numbers, inferred semantics].
4. Format: no sample-rate/channel message exists on this path; the only format information is
   `config[0..2]` at node create (§1) and cmd 7 `arg2` [verified absence, inferred location].
5. `Gwc_ord4`/`0x400000b` shared-buffer path and `DspMemMapper` (`Gwc_ord16/21`) are not used by
   `RawGwc` [verified].

## 4. Stub design for `ehwa.cpp` (+ emulator audio device)

Per node (one is enough): `{rate, enc, ch}` from ctl 4; `pendingIssue = {TRequestStatus*, client,
streamIdx}`; `eosSeen`. The emulator device ("syborg-audio", host playback) takes
`{format, rate, ch, ptr, len}` and raises an IRQ/DFC when a block is done.

| on | action | reply/complete |
|---|---|---|
| ctl 4 (node create) with uuid c36a3ebc | read `p1,p2,p3` → rate bit, encoding (1=8-bit,2=16-bit,4=A-law), channels; `Printf` them | token as today |
| `0x8000014` cmd 7 / 3 / 2 / 4 / 1 | log; cmd 1 or 4 → tell device stop/pause (optional) | complete send status 0; **do not queue any listener message** |
| `0x800000e` | read the 7 words; read `len` bytes at `ptr` (`Kern::ThreadRawRead`); header = first 4 bytes, `eos = hdr[3] & 0x10`; forward `{enc, rate, ch, ptr+4, len−4}` to the device; keep `{status, client}` | complete the status with 0 when the device has queued/played the block (for a first cut: immediately; the device then paces itself and may run ahead) |
| device "queue drained" after an `eos` block (or immediately after completing the eos block, first cut) | queue `ReplyWords = {8, 0, 0}` and complete the armed `0x8000005` listener with `KErrCompletion` (−17), exactly as in `nokia-dspbridge.md` §5.3; `0x8000015` returns the words | HwDevice → `Stopped()` → play complete |
| `0x800000f` / `0x8000011` | drop the device queue for that node | complete the pending `0x800000e`/`0x8000010` status with `KErrCancel` if any |
| `0x8000010` (record) | not needed; complete with `KErrNotSupported` or leave pending | — |
| `0x8000006`, ctl 6/7, `0x4000001` | as today | — |

Keep the `0x8000014` handler of `nokia-dspbridge.md` §5.3 for the EAP node (cmd 1 ack with
`{0,0,0}` is right there) but key it on the node: for the raw node no ack. [verified both]

## 5. Tone player path

`ToneGenerator.dll` imports only `EUser`/runtime (no Bal/Gwc/bridge): tones (`PlayTone`,
DTMF, sequences) are **synthesised to PCM on the ARM** and go through the same `RawHwDevice` /
raw node PCM path as a WAV [verified for the import set, inferred for the DevSound wiring]. There
is no DSP tone node with frequency/duration arguments. So key tones (`CMdaAudioToneUtility` in
`eiksrvui.dll`) and the startup tone are both covered by §2–§4; MP3 needs the `Mp3DecGwc` UUID
and (probably) a decoded-frame variant of the same issue/complete loop, not analysed.

## 6. AAC and MP3 decoder nodes

Static analysis of `Mp3DecHwDevice.dll`/`Mp3DecGwc.dll`, `AacDecHwDevice.dll`/`AacDecGwc.dll`
and `EaacplusDecHwDevice.dll`/`EaacplusDecGwc.dll` (all in the core ROM; Ghidra output in
`build/n95/ghidra/`, vtable slots and message handlers read with `xipdis`). Only differences from
the raw node (§1–§4) are listed; everything not mentioned is identical (node create via
`Bal_ord25/16/17`, pools, `Bal_ord1` ctl 4, `Gwc_ord17/18`, `Bal_ord28`, `Gwc_ord22`; data via
`FillThisBuffer` → `0x800000e` with the 4-byte header → status completion → next buffer; cancel
`0x800000f`).

### 6.1 Who is who

| codec | HwDevice (ECOM) → Gwc | node UUID (16 bytes, pointed to by the `Bal_ord25` struct `{TPtrC8 attrs, uuid*}`) | notes |
|---|---|---|---|
| MP3 | `Mp3DecHwDevice` (impl uid at `0x80a57150`) → `Mp3DecGwc` | `b11a1f21-11d64b05-c000dcb0-36c01f4f` (`0x80a579f4`) [verified] | no config message at all |
| AAC (LC/Main/SSR/LTP, ADTS or raw stream) | `AacDecHwDevice` (impl uid `0x101faf22`) → `AacDecGwc` | `5c89a1f1-11d63d83-c000d7b0-36c01f4f` (`0x80a4802c`) [verified] | one config message, cmd `0xf` |
| eAAC+ (HE-AAC) | `EaacplusDecHwDevice` → `EaacplusDecGwc` | `8468856b-4722a0da-eeb804b2-f26919f1` (`0x80a49750`) [verified] | config goes in the node-create attrs, raw-style messages |

Which of the two AAC devices the ROFS ringtones (`Z:\data\Sounds\Digital\*.aac`, ADTS) use is not
visible statically: DevSound picks the HwDevice by FourCC, and `AACAudioPlayControllerPlugin.DLL`
most likely asks for `' AAC'` → `AacDecHwDevice` [inferred]. The stub must key on the UUID at ctl 4
and support all three; log the UUID once to confirm.

The HwDevice `Init` (`Mp3DecHwDevice 0x80a56d24`, `AacDecHwDevice 0x80a46b60`) builds a
**0x16c-byte** create block (not 0x304): words `[0..2]` are copied from a descriptor inside the
DevSound init args (`args+0x244`, `Ptr()+4`), then two 0xb0-byte stream definitions at `+0xc`
(out) and `+0xbc` (in), both attached as stream index 0 (`Bal_ord16(0, cfg+0xc)`,
`Bal_ord17(0, cfg+0xbc)`), then pool descriptors. `config[0..2]` again become `p1,p2,p3` of the
ctl 4 block [verified offsets; meaning of the three words inferred = same rate/encoding/channel
triple as §1 — log them]. The `Bal_ord25` attrs blob is empty for MP3, 2 bytes `{aacProfile, 0}`
for AAC, 10 bytes for eAAC+ (see 6.4) [verified].

### 6.2 ARM→DSP commands (`0x8000014`, `{cmd, arg1, arg2}`)

| node | start (HwDevice `Start` → `Gwc_ord30` → vt+0x38) | stop (`Gwc_ord28(gwc,1,0)` → vt+0x3c) | pause (`Gwc_ord29` → vt+0x40) | config |
|---|---|---|---|---|
| raw (§2) | 3 | 1 | 4 | cmd 7 |
| MP3 | **3** (`0x80a577b8`: default when the HwDevice passes 0) | **6** (`0x80a577c6`) | **5** (`0x80a577d4`) | **none** (`Mp3DecGwc_ord2` is `return 0`; `SetConfig` sends nothing) [verified] |
| AAC | **3** (`0x80a47db8`) | **6** | **5** | **`{0xf, sfi, profile}`** via `AacDecGwc_ord5` → `Gwc_ord25` → `Bal_ord23` (same send path as raw cmd 7: no reply awaited) [verified] |
| eAAC+ | **2** (HwDevice passes 2 explicitly, `0x80a48414`; vt+0x38 returns −2 if 0) | **1** | **4** | none by message [verified] |

None of these commands waits for an answer [verified: all go through `Gwc_ord7/8/9/25` →
`Bal_ord23`, as in §2]. Reply to **nothing**.

AAC cmd `0xf` arguments [verified]:
- `arg1` = MPEG-4 `sampling_frequency_index` (0=96000 … 3=48000, 4=44100, 5=32000, 6=24000,
  7=22050, 8=16000, 9=12000, 10=11025, 11=8000). `AacDecGwc_ord3` (HwDevice `SetConfig`,
  vt slot 10) maps the `TMMFSampleRate` bit of `TTaskConfig[0]` to it; the custom interface
  `0x101faf23` (`SetAudioConfig`, `0x80a46e32`) sets the same index directly from its word 0.
- `arg2` = profile/object type 0..3 (Main, LC, SSR, LTP), from `AacDecGwc_ord4` (CI
  `0x10207aae`) or `SetAudioConfig` word 1.
- Sent when both are known: during `SetConfig`/CI if the node already exists (state 1), or
  right after node create (`AacDecGwc_ord1` tail) if they were set before. May be sent more than
  once. The ADTS header carries the same information, so the FFmpeg path can ignore it.

### 6.3 Data blocks (`0x800000e`, same 7-word block, same `+4/−4` header skip)

The client writes the **encoded stream as-is** (ADTS frames for the ringtones, MP3 frames; no
AudioSpecificConfig, no stripping) starting 4 bytes into the DSP buffer. Buffers are padded to an
**even length** (one zero byte) by every decoder Gwc [verified]. Header bytes:

| node | byte 0 | byte 1 | byte 2 | byte 3 | EOS condition |
|---|---|---|---|---|---|
| raw / MP3 (`Mp3DecGwc_ord3` `0x80a57634`) | 0 | 0 | 0 | `0x10` = EOS | `len == 0 \|\| iLastBuffer` |
| AAC (`AacDecGwc_ord6` `0x80a47a98`) | frame-mode flag (CI `SetFrameMode`, `AacDecGwc_ord2`) | 0 | **1 = EOS**, 2 = "conceal error" (header-only block, len 4, after `AacDecGwc_ord7`) | 0 | `iLastBuffer \|\| (len == 0 && !conceal)` |
| eAAC+ (`EaacplusDecGwc_ord3` `0x80a49368`) | 0 | 1 if the buffer was padded | conceal flag (`ord9`) | `0x10` = EOS | `iLastBuffer \|\| (len == 0 && !conceal)` |

AAC detail [verified]: with frame mode **off** (the default) `AacDecGwc` keeps 16-bit alignment
by holding back the last byte of an odd-length buffer and prepending it to the next one
(`+0x100` carry flag, `+0x104` saved byte; `FillThisBuffer` reserves 2 bytes). The DSP therefore
receives the exact byte stream in order; only the EOS buffer may end with 1–2 zero pad bytes. With
frame mode **on** each buffer is whole frames and an odd one gets a trailing zero byte. MP3 always
pads. Consequence for the stub: concatenate payloads per node and run them through an FFmpeg
parser (`av_parser_parse2` for `AV_CODEC_ID_AAC`/`MP3`), which resyncs on the syncword and
tolerates the pad bytes; do not assume one buffer = one frame.

Completion is the same as §3: complete the `0x800000e` status with 0 when consumed; `continue`
= `(lastLen != 0 && !iLastBuffer)` (`Mp3DecHwDevice 0x80a56f20`, same code in the AAC devices).

### 6.4 DSP→ARM messages (`0x8000005` listener → `0x8000015`) — the part that can stall

Each Gwc's `MessageReceived` (vtable slot 11) switches on `u32[0]`, rewrites it and hands it to
the HwDevice (`MGwcObserver` slot +0x14). Exact tables [verified]:

| node (handler) | word0 | Gwc action | HwDevice action |
|---|---|---|---|
| MP3 (`0x80a57752`) | **4** | → 0 | `Gwc_ord28(gwc,0,0)` (cancel stream AOs) then `Stopped()` → DevSound play complete (`0x80a56f46`) |
| | 6 | → 1 | `Stopped()` without cancel |
| | 0, 3, 5, 7 | ignored | — |
| | 1, 2, ≥ 8 | `KErrNotSupported` → `Gwc_ord31` → HwDevice `Error()` | DevSound stops. **Never send** (so `{8,..}` from the raw stub would kill MP3) |
| AAC (`0x80a47d3a`) | **4** | carry := 0, → 0 | cancel + `Stopped()` (`0x80a46d9e`) |
| | 6 | carry := 0, → 1 | `Stopped()` |
| | 7 | carry := 0, swallowed | — |
| | 0, 3, 5, **0xf** | ignored (0xf = echo of the config cmd; not required) | — |
| | 1, 2, ≥ 8 except 0xf | error −5 | DevSound stops. **Never send** |
| eAAC+ (`0x80a4948e`) | **8** | → 2 | cancel, then `Stopped()` if not already stopping (`+0x44 == 0`) (`0x80a486c8`) |
| | 10 | → 3, swallowed | — |
| | anything else | ignored, no error | — |

No "decoder configured", position, bitrate or underflow message is waited for anywhere: the
client's only wait is on the `0x800000e` status and, after the EOS buffer, on the stop message
above [verified for the three Gwc handlers and three HwDevice handlers]. The HwDevice branch
"2 → `Error(KErrCorrupt)`" (MP3/AAC) is unreachable through the Gwc.

### 6.5 Stub table (`ehwa.cpp` + emulator device), delta to §4

Per node keep `{uuid → format RAW|MP3|AAC|EAACP, rate/enc/ch from ctl 4, pendingIssue, eosSeen}`.

| on | action | reply/complete |
|---|---|---|
| ctl 4 with uuid `b11a1f21…` / `5c89a1f1…` / `8468856b…` | format := MP3 / AAC / AAC; log `p1,p2,p3` and (eAAC+) the 10 attr bytes | token as today |
| `0x8000014` any cmd (3/6/5, 2/1/4, 0xf) | log; AAC 0xf: store `sfi`, `profile` (informational, FFmpeg reads ADTS); 6/1 → device stop; 5/4 → pause | complete the send status 0; **queue no listener message** |
| `0x800000e` | read the 7 words, `len` bytes at `ptr`; `hdr = data[0..3]`; `eos = (AAC) hdr[2]==1 : hdr[3]&0x10`; AAC `hdr[2]==2` → header-only, drop; forward `{format, ptr+4, len−4, eos}` to the device, which appends to the node's parser stream (FFmpeg aac/mp3 parser + decoder) | complete the status with 0 once queued |
| device drained after `eos` | MP3 → `{4,0,0}`; AAC → `{4,0,0}`; eAAC+ → `{8,0,0}`; raw → `{8,0,0}` (§4); deliver via the armed `0x8000005` listener exactly as §4 | HwDevice → `Stopped()` → play complete |
| `0x800000f` | drop the node's parser/decoder state and device queue | complete pending `0x800000e` with `KErrCancel` |

The only new state versus §4 is the format tag and the per-node byte accumulator; the message
numbers are the only thing that differs between the nodes, and getting them wrong is fatal on
MP3/AAC (−5 → `Error()`), silently ignored on eAAC+.

## 6. Why key tones get KErrInUse

Static analysis of `AudioServer.dll` (`build/n95/ghidra/AudioServer.dll.c` + `tools/xipdis.py`),
`Lac.DLL` and `Aamm.DLL` (Ghidra run via `tools/syb_sym.py fn`). Tags as above.

### 6.1 The array at this+4: one pending client request waiting on event bits

The object is a small "pending request" helper embedded at **stream+0x20** (stream =
`[session+0x2c]`, fetched by `FUN_80a37880`, which leaves −2 if the session has no stream):

| offset | content |
|---|---|
| +4 | `RArray` of 0x30-byte entries `{RMessage2 (0x28 bytes), waitMask @+0x28, token @+0x2c}` |
| +0x24 | copy of the client's `RMessage2` for the request being set up |
| +0x4c / +0x54 | full mask / bits not yet armed |
| +0x50 | token (`session+0x20`); +0x58 = first error seen |

Three operations [verified]:
- **Begin** `FUN_80a3631a(this, &msg, mask, token)` (`0x80a3631a`): `mask == 0` → leave −6;
  **`Count() >= 1` → `User::Leave(KErrInUse)`** (`0x80a36338`); else store msg/mask.
- **Arm** `FUN_80a36354(this, bit)`: on the first armed bit the message is **appended** to the array
  (`RArrayBase::Append` at `0x80a3638c`); sets `bit` in the entry's waitMask.
- **Complete** `FUN_80a363c2(this, err, bit)`: clears `bit` in entry 0; when the mask reaches 0 the
  `RMessage2` is completed with the stored error and the entry is **removed** (`RArrayBase::Remove(0)`
  at `0x80a36398`-ish). Destructor `FUN_80a362d4` completes leftovers with −3.

So the array never holds more than one entry: *the previous request on this stream whose event
bits have not all come back*. It is not a list of streams, sessions or routes.

### 6.2 Which request sits there, and what releases it

The AudioServer session `ServiceL` (`FUN_80a3454e`) dispatches **function 17..27** through an
`__ARM_switch8` table at `0x80a3515a` (index = fn − 17). Case 5 = **request 22** → handler
`0x80a34c82` = "Start/Play stream" [verified table decode, name inferred]. Before dispatch the
stream's event mask is computed by `FUN_80a375f0(stream, desc)`: `mask = stream+0x7c`; if
`AudioStream_ord46(desc) == 8` bit 4 (0x10) is added, otherwise bit 2 (0x4) is removed; `mask == 0`
→ the request is completed with 0 immediately (`0x80a3514a`). Handler 22 then does, in order:

| mask bit | action | armed bit | completed by |
|---|---|---|---|
| 0x10 | `FUN_80a35b50` → `AudioStream_ord9/15`, build block, **`Lac_ord11`** | 0x10 | AudioServer observer `FUN_80a35c86(code)`: 1 → err 0, 2 → −2, 3 → −6, **0 → ignored** (`0x80a35c8c`) |
| 0x2 | `ToneGenerator_ord20` (ARM tone synthesis) | 0x2 | tone callbacks `0x80a361ec`/`0x80a3623e` (mask 7) |
| 0x4 (only if 0x2 unset) | `FUN_80a361ae` → adaptation `vt+0x18` start (DSP/HwDevice path) | 0x4 | same callbacks |

The entry present when the key tone arrives is **the previous request 22 of the same stream, still
waiting for bit 0x10 (the Lac/modem acknowledgement)** [verified for the mechanism; that it is the
key-sound stream's own earlier Play is inferred, consistent with "no DSP traffic"]. Nothing on the DSP
side is involved: bit 0x10 is modem traffic.

What `Lac_ord11` sends (`0x80885662` → `Aamm_ord71` `0x8088125c`) [verified bytes, names inferred]:
- an ISI message with **resource `[3] = 0x77`**, utid `[8] = 0`, **msg id `[9] = 0x01`**,
  `[0xb] = 2`, `[0xe] = 0x0014` (16-bit), a 0x16-byte block at `[0x10..]` (first two bytes 0xff or 0:
  mute/route flags, `[0x15]` = 1 or 3 derived from the stream name `"\x02J:"`/`"\x03JD:"`), `[0x20] = 1`,
  `[0x22]` = padded name length, `[0x24]` = name length, `[0x26] = 1`, then the stream-name bytes.
  Every Lac→Aamm call (`Lac.DLL.c` lines 1973/2052/2110/2207: play, stop, route changes) uses the
  same msg id 0x01 with a different block.
- Aamm's resource-0x77 client (`Aamm_ord69` registers `FUN_808805e8(.., 0x77)`; receiver
  `FUN_80881488` `0x80881488`) dispatches on `[9]` of the incoming message, utid not checked:

| incoming `[9]` | Aamm → Lac vtable | Lac (`0x808875f4` table) → AudioServer observer |
|---|---|---|
| **0x02** | slot 0, arg = `[10]` status | `0x808859dc`: status 0 → `observer(0)` = **ignored**, status 1 → `observer(3)` = −6, other → `observer(2)` = −2 |
| 0x08 | slot 1 | no-op (`bx lr`) |
| **0x09** | slot 3 | `0x80885a02`: **`observer(1)` = success**, request completes with 0 |
| 0x0b | slot 2 | no-op |

So the releasing event is a **resource 0x77 message with msg id 0x09** (payload unused); a 0x02
response with status 0 is merely "accepted, still pending". Our fake modem has `reply` lines only for
resource 0x0B (`00→01`, `14→15`, `19→1A`), so a 0x77/0x01 request hits
`Kern::Printf("ISCPEER: ch %d no reply for resource 77 msg 01")` (`iscpeer.inl` line ~430) and the
first Play of every stream hangs forever; every later Play on that stream → `KErrInUse` (−14)
[verified code path, runtime log line to be confirmed].

### 6.3 Stub change (fake modem, not `ehwa.cpp`)

Add to `platform/isi/replies-bb5.txt + models/<id>/isi-replies.txt` and regenerate `build/roms/<id>/<fw>/gen/isireplies.h` with
`scripts/build-standins.sh`:

```
res 77 PN_AUDIO_ROUTING?       inferred  # Aamm 0x77 client (Aamm_ord69/71), used by Lac.DLL for play/stop/route
msg 77 01 AUDIO_ROUTE_REQ?     inferred  # resp 02 [10]=status (0 = pending, 1 -> -6, else -2); IND 09 = done -> observer OK
reply 77 01 09 00 00                     # route done: Aamm msg 09 -> Lac vt+0xc -> AudioServer observer(1) -> Play completes 0
```

Minimal form: answer the 0x01 request directly with msg id **0x09** (two zero bytes; Aamm reads
nothing from it). If a closer imitation of the real modem is wanted, make it a special case in
`iscpeer.inl`: `Reply(0x77, tid, 0x02, {0x00})` followed by `Reply(0x77, tid, 0x09, {0x00,0x00})`;
the 0x02/status-0 message alone does **not** release the entry. Do not answer with status 1/2 in
`[10]` of a 0x02 unless a failure is intended (entry released, Play fails with −6/−2).

After the entry is released, each key press: request 22 → Lac ack (immediate) → bit 0x4 branch
(DSP) if the stream's mask has it → the raw-node path of §2–§4. [inferred: whether bit 0x4 is set for
the key-sound stream depends on `AudioStream_ord46(desc) == 8`; if the tone still produces no DSP
traffic after this fix, log `FUN_80a375f0`'s result for the stream.]

**−21 with the startup tone enabled** [inferred, no −21 literal exists in `AudioServer.dll`,
`Lac.DLL` or `Aamm.DLL`]: the startup tone's own stream hangs on the same unanswered 0x77/01 and so
stays "playing" in the audio policy; the key-sound client (lower/equal priority) is then refused by
the policy server with `KErrAccessDenied` before it reaches request 22. The same reply line fixes the
hang; for the policy to release the device the startup tone must also *finish*, i.e. the raw-node
PCM path (cmd 7/3, `0x800000e` completions, final `{8,0,0}`) of §4 must be in place. No additional
modem message is needed.
