# One sound device for every Symbian 9.x phone: architecture

Status: proposal (2026-11-11), from the docs and code listed below; no runs. Tags: [V] = seen in our
docs/code/ROM inventories, [I] = inferred. Rules applied: the guest only forwards (all mixing,
decoding, timing, volume and routing on the host); protocol variants are declared in the profile;
no ROM patching for sound. Sources: `docs/devices/nokia-n95-8gb-rm320/nokia-dspbridge.md`,
`nokia-dsp-playback.md`, `docs/research/dsp-pcm-format.md`, `docs/devices/nokia-n73-rm132/dspbridge-n73.md`,
`docs/devices/nokia-5730-rm465/rapido-audio.md`, `docs/devices/nokia-n8-rm596/rapido-audio.md`,
`src/qemu-syborg/hw/arm/syborg_audio.c`, `src/standins/ehwa/ehwa.cpp`, `src/standins/aamdaifdriver/`,
`src/qemu-syborg/hw/arm/syborg_modem.c`, `platform/isi/replies-bb5.txt`, `models/*/model.toml`,
`build/roms/*/*/core-files.txt` (module inventory).

## 1. How each generation reaches the hardware today

The core-ROM inventory (`core-files.txt` of every extracted firmware) gives three audio stacks, not
two [V]:

| Stack | Phones | DSP link (kernel boundary) | ARM-side chain above it | Mixer | Our stand-ins today |
|---|---|---|---|---|---|
| **A. TI DSP bridge** (OMAP 2420 / 1710) | N95, N95 8GB (9.2); N73 (9.1, bridge 1.1.1071) | `ehwa.ldd` "Hwa": nodes per stream (raw PCM `c36a3ebc`, MP3 `b11a1f21`, AAC `5c89a1f1`, eAAC+ `8468856b`), PCM/encoded blocks by ctl `0x800000e`, DSP->ARM `{8}`/`{4}` = play complete [V] | MMFDevSound -> DevSoundAdaptation -> RawHwDevice / Mp3DecHwDevice / AacDecHwDevice -> Gwc -> Bal/bridgeapi [V] | in the DSP's EAP task: EAP msg 0 = stream {id, rate enum, ports}, EAP 7/8 = mixer ports and gains (Q14/Q15 [I]) [V msg layout] | `ehwa` + `ehwaomap`: nodes, EAP 0 format, raw/MP3/AAC/eAAC+ blocks to `syborg-audio` through ONE global FIFO and ONE voice [V]; `ehwa.bridge = "omap1710"` for the N73 [V] |
| **B. RAPIDO, "eapdsp" generation** | E71, N81 (9.2); N79, N86, 5730 (9.3); 5800, N97 (9.4) | `AAMdaifDriver.ldd` "MdaifDriver": DoControl only (5/7/6 start-up, 8 = ring of a stream type, 2 = map window) + ISI resource 0xF3 to the DSP via Aamm (0x30/31 map, 0x32/33 stream set-up with rate code + channels, 0x34/35 "data written" pacing, 0x36/37 position) [V] | DevSound -> RawHwDevice / Mp3DecHwDevice / AmrNbDecHwDevice -> `eapdsp.dll`/`reapdsp.dll` -> `aamdaif.dll` (ring writer) -> AAMdaifApi; `cdspcontrolserver.dll` runs the DSP handshake [V modules; eapdsp's data path not read: I] | in the DSP, one ring per stream type 0/1/2 (48 kHz stereo / mono / 16 kHz mono [I]) | `aamdaifdriver` (silent; ring window when `ring_units > 0`), fake modem F3 replies with a half-ring timer [V]. N81's profile says `hardware.dsp = "ti-dsp-bridge"`: wrong, its core has aamdaif/eapdsp, no Bal/bridgeapi [V] |
| **C. RAPIDO, "audio processing engine" generation** | E52, E72, E5-00, 6720 (9.3, 2009+); N8, 701, 808 (Symbian^3/Anna/Belle) | same MdaifDriver + F3 set; Belle renumbered the controls (`ctl_map`) [V] | `mmfaudioserverstart.exe` `!AudioProcessingEngine` (`audioprocessingengine.dll` + `libeapcontrolup.dll` = EAP mixer/effects **on the ARM**) -> `aamdaif.dll` ring [V] | on the ARM: one mixed PCM ring per stream type goes to the DSP [V for the N8 ring bookkeeping] | `aamdaifdriver` (`ring_units = 16`: real window, ctl 8 sizes), modem 0x35 reply after half a ring [V] |

Where the ISI audio resources sit [V]: 0x0B (`PN_AUDIO?`, Aamm, audio server start/setup; N8 also
0B 04 and 0B 5D) and 0x77 (`PN_AUDIO_ROUTE?`, Lac before every play; without the 09 reply the first
play hangs and later ones get KErrInUse) are answered by the fake modem on all BB5 phones. They carry
routing/accessory state, never PCM.

### Where the beep comes from

- Every core ROM has `ToneGenerator.dll` and `RawHwDevice.dll` [V inventory, all 30 firmwares with a
  core listing except the E71/N81 ones where ToneGenerator sits in ROFS: I]. On the N95
  `ToneGenerator.dll` imports only EUser: tones, DTMF and sequences are **synthesised to PCM on the
  ARM** and go down the raw PCM path like a WAV [V]. There is no DSP tone node with frequency
  arguments on any stack [V for A; I for B/C from the absence of any tone-like F3 message].
- Key tones: `eiksrvui.dll` KeySoundServer -> `CMdaAudioToneUtility` -> `mmfaudiotonecontroller.dll`
  -> DevSound -> AudioServer request 22 with mask bit 0x2 = `ToneGenerator_ord20` [V N95]. Stack C
  adds `tonehwdevice.dll` (a software tone HwDevice feeding the engine [I]) and, on 9.3/^3,
  `systemtoneserver.dll` (system tones policy; still PCM in the end [I]).
- The startup tone is a file (WAV on the N95, AAC "Wakeup tone" on RAPIDO phones) through
  `CMdaAudioPlayerUtility` [V]; `H-startuptone-none` on 5800/E71/N81/N8/808 hides it only because
  the silent RAPIDO stand-in never finishes it.

So "the beep" is never a hardware tone generator: **it is a short PCM stream that must start while
another stream may be playing, and must report "finished" on its own**. That is the whole
requirement, and it is what the current device cannot do (section 4).

## 2. The boundary

Three candidate boundaries for "the guest forwards, the host does the rest":

| Option | What crosses to QEMU | Fidelity | Effort | Risk | Nokia code unmodified |
|---|---|---|---|---|---|
| **1. PCM/encoded streams at the LDD** (recommended) | per stream: format, rate, channels, gain, class/route tag, a guest ring with write/read positions, EOS; encoded streams as byte rings with a codec tag | high: the phone's own DevSound, tone synthesis, EAP gains and policy all run; the host mixes | medium: one device, two stand-ins reworked (ehwa, aamdaifdriver) and the modem's F3 pacing moved onto the device clock | low: every byte the stand-ins forward is already in hand [V]; only the RAPIDO 0x35/0x37 semantics are inferred | all |
| 2. DSP task messages to the host (forward Bal messages / F3 frames; QEMU models both DSPs) | node/EAP messages (A), F3 frames + MdaifDriver controls (B/C) | same as 1 in sound, but the host has to keep two DSP state machines plus the kernel-side chunk/ring bookkeeping that must stay in the guest anyway (user mappings, TRequestStatus completion) | high: the guest LDD still exists and the host re-parses what the LDD already decoded | medium: the message set grows per phone (Belle renumbering, N73 `0x0c00000x`) | all |
| 3. Replace the DevSound adaptation / HwDevice plugins with ours talking to a simple PCM LDD (like Symbian's `ESoundSC`) | PCM per DevSound instance | highest control (we own mixing policy) | high: ECOM plugins per generation (A: HwDevices, B/C: eapdsp or the engine), ROFS edits per firmware | high: policy, routing (Lac/Aamm) and the modem audio resources bypassed; contradicts "no ROM patching" | no |

Recommendation: **option 1**. The stand-ins already decode everything needed (A: node kind, EAP 0
rate, cmd 7 channels, block bytes, EOS header; B/C: ctl 8 ring size and stream type, the window the
engine writes, F3 0x32's rate code and channels). What is missing is a device with *several* streams,
a clock the guest trusts, and per-stream completion. The variants stay in profiles: `ehwa.bridge`,
`aamdaifdriver.ctl_map`, `audio.ring_units`, plus the new keys in section 5.

## 3. The QEMU device: `syborg-snd` (syborg-audio v2)

Same slot as today (0xc000e000, IRQ 13; the board property `audiodev` as now), new ID
`0xc51d0020`, so an old ROM on the new QEMU is refused by the stand-in's ID check instead of
misbehaving. One device, 16 stream slots, one output voice; every slot is independent.

### 3.1 Registers

Global (word index from the base):

| # | name | meaning |
|---|---|---|
| 0 | ID | 0xc51d0020 |
| 1 | CAPS | bit0 FFmpeg present, bit1 MP3, bit2 AAC, bit3 AMR-NB, bit4 capture, bits 8..15 = slot count |
| 2 | CLOCK_LO / 3 CLOCK_HI | frames mixed since reset at 48 kHz (device time; advances only while the mixer runs, see 3.4) |
| 4 | MASTER_VOLUME | 0..256 (Q8), default 256 |
| 5 | ROUTE | metadata written by the guest: 0 none, 1 earpiece, 2 speaker, 3 headset, 4 BT, 5 TV-out; logged, used for an earpiece attenuation only |
| 6 | INT_STATUS | bit k = slot k raised (latched; write 1 to clear) |
| 7 | INT_ENABLE | same bits |
| 8 | CAP_SLOT | which slot is the capture slot (fixed 15) |

Per slot k, bank at `0x100 + k*0x40` (words):

| # | name | meaning |
|---|---|---|
| 0 | CTRL | write: bit0 START, bit1 STOP (drop, silence, clear EOS), bit2 PAUSE, bit3 RESUME, bit4 EOS (no more data after WRITE_POS), bit5 FLUSH (restart decoder state, keep the ring) |
| 1 | FORMAT | 0 S16LE PCM, 1 S8 PCM, 2 A-law, 3 MP3, 4 AAC ADTS, 5 AAC raw (+CFG), 6 eAAC+ (same decoder as 4), 7 AMR-NB, 15 tone (debug aid only) |
| 2 | RATE | Hz (PCM; encoded streams ignore it) |
| 3 | CHANNELS | 1 / 2 (interleaved); 3 = non-interleaved stereo (A's cmd 7 arg2 3) |
| 4 | RING_ADDR | guest physical, contiguous |
| 5 | RING_SIZE | bytes, power of two not required |
| 6 | WRITE_POS | bytes written, monotonically increasing (the device takes `% RING_SIZE`) |
| 7 | READ_POS | read-only: bytes consumed by the resampler/decoder |
| 8 | WATERMARK | raise the slot's interrupt when free bytes (`RING_SIZE - (WRITE_POS - READ_POS)`) >= WATERMARK |
| 9 | GAIN | 0..256 (Q8), default 256; a second word GAIN_R for pan if ever needed |
| 10 | CLASS | metadata: 0 other, 1 key tone, 2 system tone, 3 ringtone, 4 music/game, 5 voice; logged, no policy |
| 11 | STATUS | bit0 running, bit1 EOS reached (ring drained after EOS), bit2 underrun since last read (clear on read), bit3 decoder error |
| 12 | FRAMES_LO / 13 FRAMES_HI | frames of this stream mixed into the output (stream clock, at the stream's rate) |
| 14 | CFG_ADDR / 15 CFG_LEN | codec config (raw AAC AudioSpecificConfig) |

Interrupt sources per slot: watermark reached, EOS reached, decoder error. One IRQ line; the
stand-in's ISR queues a DFC that completes the guest requests (section 4).

### 3.2 Stream model

Any number of slots play at once: music/game (A: one node per DevSound instance; C: the engine's
mixed stream type 0), key tones (A: a second raw node; C: already mixed by the engine into the
same ring: one slot), ringtone preview (A: AAC node; C: mixed), voice (C: stream type 2, 16 kHz
mono [I]; A: not needed, no call audio exists without a network). A slot is bound to a guest ring:

- Stack A: the stand-in keeps its own contiguous ring per node (as `ehwa.cpp` does today for the
  whole device) and copies each `0x800000e` block into it: WRITE_POS += len. Zero-copy is not
  worth it here (blocks live in the client's heap).
- Stack B/C: the window the stand-in maps for ctl 2 dir 1 is already a physically contiguous
  kernel chunk; its region for stream type t becomes slot t's ring directly (RING_ADDR = the
  region's physical address, RING_SIZE = `ring_units x unit`). The engine's F3 0x34 "write position"
  is the WRITE_POS.

### 3.3 Mixing, resampling, decoding (host only)

The backend callback (`syborg_audio_callback(free)` as today) renders `free` bytes: for each running
slot pull PCM from the ring (or from the slot's decoder FIFO), resample to 48 kHz stereo (libswresample
when FFmpeg is present; an in-tree linear resampler otherwise, enough for tones and games), apply
GAIN, sum into a 32-bit accumulator, apply MASTER_VOLUME and the ROUTE attenuation, clip, write to
the one `SWVoiceOut` (48 kHz S16 stereo, opened once at realize). Encoded slots keep today's FFmpeg
parser+decoder per slot (the per-node byte concatenation that `nokia-dsp-playback.md` §6.3 asks
for); READ_POS advances by the bytes the parser took. Without FFmpeg (Android phase 1) an encoded
slot is consumed at the bitrate the ADTS/MP3 frame headers announce (a 20-line header parser), so the
phone's player still ends on time, silently; CAPS says so.

### 3.4 Timing

- The clock is the backend's consumption: `audio_be` calls the callback as the host sink drains
  (pipewire/pa/alsa) and with `-audiodev none` QEMU's dummy backend still consumes at the nominal
  rate on a host timer, so CLOCK and FRAMES advance at real time in headless runs too [V for QEMU's
  `none` backend behaviour: I, to confirm on the first headless boot]. This replaces the modem's
  fixed half-ring NTimer for RAPIDO and ehwa's 10 ms polling pump.
- Underrun (ring empty, no EOS): the slot contributes silence, READ_POS stays, STATUS.underrun set,
  no interrupt storm (one watermark interrupt, re-armed when the guest writes). Overrun is the
  guest's problem: the device never blocks, never overwrites.
- EOS: when the guest sets EOS and READ_POS reaches WRITE_POS the slot raises EOS at once (the
  backend's own latency, 20-50 ms, follows after; nothing is cut because consumed samples are
  already in the host buffer). The stand-in turns it into the per-generation "play complete".
- Pause: the slot contributes silence and does not consume; the stream clock stops.

### 3.5 Volume and routing

PCM reaches us already scaled on stack C (the engine applies the profile volume before writing
the ring [I]) and on stack A per EAP port gains (EAP 7/8, Q14 [I]); the stand-in writes GAIN from
EAP 7/8 when it can decode them, else 256. ROUTE is metadata from what the stand-ins already see
(ISI 0x77 route requests carry the route block; the modem device, host-side, can set the audio
device's ROUTE through a QOM link, no guest change). No policy on the host: the phone's audio
policy server already decides what plays.

### 3.6 Recording (cheap part only)

Slot 15 is a capture slot (ring written by the device, READ_POS by the guest; `audio_be_open_in`).
Stack A's record node (`0x8000010` empty-buffer issue, `0x8000011` cancel) maps onto it in a
later phase; stack B/C's mic path (stream type 2 [I]) is not read yet. Nothing in the first
milestones needs it.

### 3.7 Backends

`-audiodev pipewire|pa|alsa|none|wav` as now (`tools/device.py qemu-args --audio`); the device sees
only `audio_be_*`. Android: `-audiodev none` first, then a small AAudio backend (`android-port.md`
§ phase 4); FFmpeg optional (`SYBORG_NO_FFMPEG` stays).

## 4. The beep, per generation

What went wrong before [V in `ehwa.cpp`, `syborg_audio.c`, `hacks.md` 2026-11-05 review]: the device
has **one** voice, **one** queue and **one** EOS flag; the stand-in has **one** global request FIFO
(`KMaxReq = 8`), **one** `Playing` format and **one** `EosNode`. A key tone (its own raw node, its own
rate) arriving while the jingle or a game played was appended behind the other node's blocks at the
other node's format, its `{8,0,0}` could only be reported through the single `EosNode`, and a full
FIFO completed blocks without playing them. KeySoundServer then waited for a tone that never
"finished": the next tone got KErrInUse (the AudioServer's one-pending-request array, `nokia-dsp-playback.md`
§6) and the UI hung after the region query when the scrolling produced tones. On top, the 0x77/01
route request was unanswered until recently (first play of every stream hung). The 2026-11-05
revert put the `H-keysound-nommf` byte patches back; that is the "patched out" state the owner
wants gone.

How it works with the new device:

| Stack | Key tone path | Device use | "finished" back to the phone |
|---|---|---|---|
| A (N95, N73) | KeySoundServer -> tone utility -> DevSound -> ToneGenerator PCM -> RawHwDevice -> raw node: ctl 4 (node), EAP 0 (rate), cmd 7 (channels), cmd 3, blocks `0x800000e`, EOS header `0x10` [V] | the stand-in opens a slot per node at node create (CLASS 1 if the EAP stream has 1 port and short blocks [I], else 0), copies blocks into the node's ring, completes each `0x800000e` status from the watermark interrupt (not from a timer), sets EOS on the last block | slot EOS interrupt -> `{8,0,0}` (raw) / `{4,0,0}` (MP3/AAC) on the armed `0x8000005` listener for *that* node; several nodes may end in any order |
| B (5800, E71, N81, N79, N86, 5730, N97) | same user path down to eapdsp -> aamdaif ring (stream type by DevSound priority/rate [I]) -> MdaifDriver ctl 8 + F3 0x32/0x34 | ctl 8 binds stream type t to slot t with the window region as ring; F3 0x32's rate code/channels (seen by the modem device) set the slot's FORMAT/RATE/CHANNELS through the QOM link; F3 0x34's write position -> WRITE_POS; the 0x35 reply is sent when READ_POS passes that position (host-side, modem device polling the audio device at the mixer period, no guest timer) | the tone utility's "finished" comes from DevSound's own buffer accounting once the 0x35 replies keep pace; nothing else is needed (cdspcontrolserver checks no status [V]) |
| C (N8, 701, 808, E52, E72, E5, 6720) | tonehwdevice/ToneGenerator PCM -> `!AudioProcessingEngine` mixes it with everything else -> one ring per stream type -> ctl 8 / F3 0x34 | exactly as B; one slot per stream type, the engine is the mixer, the device only paces and plays | as B; the engine's `FUN_806847ba` free-space loop runs on real 0x35 replies instead of a fixed 80 ms timer |

Why this cannot repeat the old failure: per-slot EOS and per-slot pacing (A), and on B/C no
device-side stream notion at all beyond "a ring per stream type". The only serialisation left is
the phone's own policy server, which is the real phone's behaviour.

## 5. Work list, phases, risks

### Stand-ins and profile keys

| Item | Change | Status of the protocol knowledge |
|---|---|---|
| `syborg_audio.c` -> `syborg_snd.c` | new device (3.1-3.6), FFmpeg path kept, tone generator kept as FORMAT 15 for `sydb` tests | n/a |
| `src/standins/ehwa` | slot per node, ring per node, IRQ-driven completion, EAP 7/8 gains -> GAIN (optional), drop the global FIFO/pump | A: [V] blocks, EOS headers, messages; gains [I] |
| `src/standins/aamdaifdriver` | ctl 8 region -> slot binding (RING_ADDR/SIZE, START), ctl 2 dir 1 window unchanged, close -> STOP; `ring_units`, `ctl_map` as now | B/C: [V] ring sizes and window; stream-type meaning [I] |
| `syborg_modem.c` | F3 0x32 -> slot format, 0x34 -> WRITE_POS + reply when consumed, 0x36/0x37 -> READ_POS in frames (QOM link `snd` on the modem) | 0x32 fields [V]; 0x35/0x37 `u32 @12` meaning [I] (first thing to settle with `sydb strace` on channel 36 and a tone) |
| `platform/isi/replies-bb5.txt` | keep 0B/77 replies; add per-phone unanswered audio requests as they appear | [V] for N95/N8 |
| profiles | `[board.audio]`: `device = "syborg-snd"`, `dsp = ehwa|rapido` (as now), new `streams` table for RAPIDO: `{0 = "48000/2", 1 = "48000/1", 2 = "16000/1"}` (declared, not guessed from arguments), `ring_units`; stack A: `ehwa.bridge`; N81: fix `hardware.dsp` to `rapido` | [V] except the stream table [I] |
| ROM patches | remove `H-keysound-nommf`, `H-startuptone-nommf` from the N95 default (the `audio-exp` variant becomes the ROM), `H-startuptone-none` from 5800/E71/N81/N8/808 once their tone finishes | n/a |
| tools | `sydb snd` (slot table, positions, underruns) via QMP/HMP; `device-smoke.sh` gains "key tone ended within 1 s" from the serial log | n/a |

### Phases

| Phase | Content | Effort | Exit |
|---|---|---|---|
| 0 | `syborg-snd` device: slots, mixer, resampler, FFmpeg decoders, clock, IRQs, HMP dump; host-only test feeding two rings | 3-4 d | two overlapping test streams mix at the right pitch with `-audiodev pipewire` and `none` |
| 1 | N95: `ehwa` per-node slots; key tones + startup jingle + ringtone preview (AAC) with the byte patches off; `sydb` key-press loop while the jingle plays | 2-3 d | **first milestone**: key tones and a ringtone on the N95 with no audio patch in the ROM |
| 2 | N8 (011/025/Belle/Delight): `aamdaifdriver` slot binding, modem F3 pacing on the device clock, `H-startuptone-none` off | 3-4 d | **first milestone, part 2**: N8 start-up tone, key tones, ringtone preview; Angry Birds no longer freezes on sound |
| 3 | Other RAPIDO phones: C first (701, 808, E52, E72, E5, 6720: same engine as the N8), then B (5800, N97, N79, N86, 5730, E71, N81): read eapdsp's write path statically where it faults past the window | 1 d per C phone, 2-3 d for the first B phone, 0.5 d each after | smoke test per phone: idle beep, start-up tone ends |
| 4 | N73 (bridge 1710 node controls `0x0c00000x`, unverified), N81 profile fix | 1-2 d | key tones on the N73 |
| 5 | Capture slot (A record node, then B/C), AMR-NB (FFmpeg), AAudio backend for Android | 3-5 d | voice recorder app records; Android bundle has sound |

### Risks, ranked

1. RAPIDO 0x35/0x37 semantics are inferred: if `u32 @12` is a DSP read position rather than an echo,
   pacing on READ_POS is right but the units (bytes vs frames vs 16-bit words) must be settled with one
   strace run; wrong units stall or overrun the engine (phase 2's first day).
2. Stack B's `eapdsp.dll` data path is unread; the 5800 note says it faults past the stand-in's
   window, so its window size/stream-type use may differ from the N8's (phase 3 B).
3. Headless/`none` backend timing: if the dummy backend does not drive the callback at the nominal
   rate, CLOCK stalls and every RAPIDO play hangs again (the symptom `H-startuptone-none` hides).
   Fallback: a QEMU timer-driven mixer when the backend reports no consumption.
4. More unanswered ISI audio requests per phone (N8's 0B 04 / 0B 5D appeared only at run time); each
   costs a triage, not a design change.
5. KeySoundServer latency expectations: a tone must start within a few ms of the request; the
   watermark/EOS interrupts and the backend's buffer size (keep the sink period <= 10 ms) decide it.
6. Belle/CFW control renumbering and future firmwares: contained by `ctl_map` per firmware.
