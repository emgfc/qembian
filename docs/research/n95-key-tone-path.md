# N95 8GB: where a key tone's sound goes (static analysis, 2026-11-12)

Sources: `build/roms/nokia-n95-8gb-rm320/RM-320_35.0.001_13/ghidra/{AudioServer.dll,Aamm.DLL}.c`, `tools/xipdis.py`
of AudioServer.dll / ToneGenerator.dll, `tools/syb_sym.py`, log `build/sydb-snd95/serial.log` t=70.74-70.76 (read only).
Tags: [V] verified in code/log, [I] inferred.

## 1. Bottom line
The key tone's PCM currently goes **nowhere**. On the N95 every DevSound stream, tones included, is
handled by Nokia's `AudioServer.dll` (in MmfAudioServer.exe): `MmfDevSoundServer.dll` imports no HwDevice/Gwc/Bal,
only `AudioServerProxy` (33) and `DevSoundAdaptationLayer` (12; itself `AudioServerProxy` + one ECOM create) [V]. The only tone synthesiser
in the ROM is `ToneGenerator.dll` (RTTI `CToneGenerator`, `CSampler`, `MMMFHwDeviceObserver`,
`TPairedFreqDuration`, `CRTParser`), imported by AudioServer.dll alone [V]. When it is used, the PCM
goes down the startup-WAV path: `ToneGenerator` does `RLibrary::Load(L"\sys\bin\RawHwDevice.dll")`, `Lookup(2)()`
= factory, and drives that `CMMFHwDevice` (RawHwDevice -> RawGwc/Gwc -> Bal -> bridgeapi -> Hwa raw node
`c36a3ebc`, blocks via ctl `0x800000e`) [V load + import chain; I that the node/block set equals the WAV's].
The modem gets no frequency/duration (section 3). No EAP message, shared ring or DSP tone node is involved.

## 2. Why the key tone does not reach ToneGenerator [V]
AudioServer request 22 (handler `0x80a34c82`; DevSound fn 22 from KeySoundServer maps 1:1 to it in the
log) works on a per-stream flag word, `stream+0x70 = FUN_80a375f0(stream+0x7c, desc)`:

| bit | action in handler 22 | completes when |
|---|---|---|
| 0x10 | `Lac_ord11` -> ISI 0x77/01 route request | our reply 0x77/09 arrives |
| 0x2 | `FUN_80a35d60` -> `ToneGenerator_ord20(900 Hz, 100 000 us)` (fixed beep), RawHwDevice -> Hwa | tone observer `Finished()` (`0x80a3623e`) |
| 0x4 | `FUN_80a361ae`: copy the RMessage2, `CDeltaTimer::Queue(1 500 000 us)`; callback `0x80a35ffe` -> `FUN_80a35eaa` = switch on `iArgs[0]-19` (tone kinds: ord20 PlayTone(freq,dur), ord4 dual tone, ord11 DTMF, fixed 900 Hz, ...); **if bit 0x2 is still clear it calls `Finished(0)` without playing** (`0x80a35f0c`) | timer + observer |

Log: request 22 completes `r:0` at 70.753, 1 ms after our `0x77 09`; DevSound's "tone finished" event (fn 21)
completes at 70.755. So the key-sound stream has **only bit 0x10**: nothing is synthesised.

Bits 0x2/0x4 live in `stream+0x7c`, written only by `FUN_80a3554a(stream, cmd)` (switch8, 11 cases):
case 5: `server+0x108 & 1 ? (+0x7c |= 2, &= ~4) : (+0x7c |= 0x10)`; case 6: `&= ~2`, then `|= 4` if
`server+0x108 & 1` else `|= 0x10`; cases 7/8 = the same pair on accessory attach/detach. `server+0x108`
bit 0 is set in `FUN_80a32b5a(server, RPointerArray<accessory>)` when an entry has
`audio_common_ord49(acc) == 2 && audio_common_ord40(acc) & 2` (then `FUN_80a3554a(server+0x2c, 7)`); with
no such entry it is cleared and cmd 8 is sent. So **ARM tone synthesis (= the Hwa raw-node path) is on
only while an "accessory" of class 2 with capability bit 1 is listed**; otherwise the stream is route-only
and the tone finishes silently, i.e. the server expects the BB5 modem DSP to make the sound [I; no
0x77/0x0B message carries a frequency, so either the real modem derives a click from the route block /
stream name `00 11 0a 76 02 0b`, or the real accessory list always holds the phone's own output
(IHF/earpiece) as a class-2 entry; not resolved].

## 3. The ISI traffic per key press is parameters, not audio [V Aamm.DLL.c]
- `0B 14` = `Aamm_ord5` "set parameters": `[8]`=utid, `[10]`=1, `[11]`=device (5, 4, 2), `[0xf]`=block count,
  blocks (`Aamm_ord6`) `03 08 <param> <sub> <value16> 00 00`. Seen: dev5 p5=0x80, dev4 p2=6, dev5 p8=1 then
  p8=0 (around the route), dev4 p0c=3, dev2 p1/3=0, dev2 p1/4=0xff42 [I: gains/mutes].
- `0B 19` = `Aamm_ord3` "get parameter": `[11]`=device, `[12]`=param (5, 0c, 3f); our `1A` reply is fine.
- `0x77/01` = Lac route (nokia-dsp-playback.md section 6.2); `09` releases bit 0x10.

## 4. What our side must do (cheapest first)
1. **Make the AudioServer take the ARM path**: get an accessory entry with `audio_common_ord49()==2 &&
   ord40()&2` into `FUN_80a32b5a` (the accessory-list observer; its feeder, accessory framework via
   `audio_common.dll`, is unresolved, so the fake modem or the DOS/accessory stand-in must report such a
   device) [I]. Then every tone (fn 19-25, 22's 900 Hz beep) is PCM on the existing raw-node path; `ehwa`
   needs only what the WAV uses (node create, cmd 7/3, `0x800000e` blocks, `{8,0,0}`), plus a second raw
   node open while a WAV plays [I].
2. Without that: a host-side fixed click on the ISI pair `0B 14 dev5 p8=1 / p8=0` [I: p8 = tone on/off;
   no frequency is available]. A guess that cannot follow the UI's tone settings; not recommended.
3. No ROM patch: `ToneGenerator.dll` and `RawHwDevice.dll` are in core and load.
Next (one run): `sydb dbg` hw-breakpoints on `0x80a32b5a` and `0x80a3554a` (log r1) for the accessory list and
cmd at boot, and on `0x80a3b304` (ToneGenerator HwDevice start) once bit 0x2 is set.
