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
