# N8 (RM-596) RAPIDO audio: the DSP shared-memory ring (mock level)

Scope: the model a stand-in needs so that `mmfaudioserverstart.exe`'s `!AudioProcessingEngine`
starts, stays alive, and plays silently at (about) real time. Prior art and the start-up
handshake (MdaifDriver ctl 5/7/6, F3 0x30/0x31, the 0x32/0x33 stream set-up) are in
`docs/devices/nokia-5730-rm465/rapido-audio.md`; this page covers only what the N8 added.
Tags: [verified] = read in the N8 core decompilation (Ghidra, `build/roms/nokia-n8-rm596/<fw>/ghidra/`),
[inferred] = best guess. Nothing here is a reverse-engineering of the DSP itself.

## Who does what (N8 core)

| Module | Role |
|---|---|
| `audioprocessingengine.dll` (0x8068c6f8) | the `!AudioProcessingEngine` thread: mixes through `libeapcontrolup.dll` (EAP = the audio effects/mixer library) and pushes PCM into a DSP link |
| `aamdaif.dll` (0x80683b58) | the DSP link ("MDAIF" = modem/DSP audio interface): **owns the ring-buffer bookkeeping** (`FUN_806848bc` ConstructL, `FUN_806847ba` free-space, `FUN_80684312`/`FUN_806843ce` write, `FUN_80684734` state machine) [verified] |
| `AAMdaifApi.dll` (0x80685548) | `RBusLogicalChannel` over "MdaifDriver": ord 4 = ctl 8, ord 11 = ctl 2 dir 1 (base 0x10000000 + offset), ord 5 = ctl 2 dir 0 (the 0x50-byte info table) [verified] |
| `Aamm.dll` (0x805e...) | ISI F3 framing: ord 53 = request **0x34** (22 bytes), ord 51 = request **0x36** (18 bytes); the observer in aamdaif gets each reply as (id 0x34/0x36, u16 @10, u32 @12) [verified layout; ids inferred from the lengths and the 5730's set] |
| `cdspcontrolserver.dll` | as on the 5730: ctl 8's size / (1920, 960, 320) = frames in F3 0x32 [verified] |

## The crash we started from

`!AudioProcessingEngine` KERN-EXEC 3 in `__aeabi_memmove8` from `libeapcontrolup.dll+0x43aac`
(0x8063c324, the "same format: memmove" leg of a sample-copy dispatch), 0x3c0 bytes written exactly
at the end of the MdaifDriver window whatever its size. Cause [verified in aamdaif.dll]: the link's
ConstructL takes the ring size and offset from **ctl 8's out words**; our stand-in wrote nothing, so
size = 0 and the write position (`+0x90`) that wraps with `if (size <= pos) pos -= size` never wrapped.

## MdaifDriver ctl 8: the DSP ring of one stream [verified]

`AAMdaifApi` ord 4 -> `DoControl(8, {streamType, TUint* size, TUint* offset, TUint* x})`; all three out
words are pre-set to 0 by the caller.

| word | meaning |
|---|---|
| in `streamType` | 0, 1, 2 (aamdaif `+4`; cdspcontrolserver divides size by 1920 / 960 / 320 for these, i.e. bytes per 10 ms: 48 kHz stereo 16-bit, mono, 16 kHz mono) [verified sizes, meaning inferred] |
| out `size` | ring size in bytes -> aamdaif `+0x3c`; the write position wraps at it |
| out `offset` | offset inside the DSP window -> ctl 2 `{0x10000000 + offset, size, &ptr, 1}` (ord 11) -> user pointer `+0x44` |
| out `x` | not stored by aamdaif; not read by cdspcontrolserver [verified]; 0 |

The engine then creates a `CMMFDescriptorBuffer` over `size / 2` bytes and writes blocks with
`TPtr8(base + pos, n, n)`; each block ends with Aamm ord 53 = F3 0x34 (see below) [verified].

Stand-in (`src/standins/aamdaifdriver/aamdaifdriver.cpp`, N8 profile `config = { ring_units = 16 }`
-> `BoardConfigInt("aamdaifdriver.ring_units")`): size = `ring_units x (1920 | 960 | 320)`, offset =
the next 128 KB region of the 1 MB window (8 regions, round robin), x = 0. `ring_units = 0` (the
default; 5730/5800 as tested so far) keeps the old "write nothing" behaviour.

## Belle's control numbers (v111, Delight) [verified: boot logs 2026-11-08]

Belle's aamdaif.dll/AAMdaifApi.dll renumbered the MdaifDriver controls; the arguments are unchanged.
Start-up on 011.012 and Anna: `5, 7 x7, 6, 10, 8, 2, 8, 2, 11, 2, 11, 10`; on Belle: `1, 3 x7, 2, 6,
4, 0, 4, 0, 7, 0, 7, 6`. So Belle 1/3/2/6/4/0/7 = old 5/7/6/10/8/2/11 (2 and 6 by order: both take
no in words). The stand-in translates through `[[standins]] name = "aamdaifdriver" config = { ctl_map
= "0:2,1:5,2:6,3:7,4:8,6:10,7:11" }` (models/nokia-n8-rm596/firmware/111.040.1511, inherited by Delight); the boot
log prints `SYBSTUB: MdaifDriver control numbering ...`. Belle 5 (a1 = 0, at stream close) has no
old equivalent and maps to 15 (accepted). Old 10 / Belle 6 takes a value (0 or 1 in every log), not an out
word: the stand-in wrote through it, faulted in `!AudioServer` (`Exc ... FAR=7f000000`) and returned -38;
it now accepts values below 0x10000. Without it the ring (4) and the buffer (0)
went unanswered and `!AudioProcessingEngine` copied through a NULL pointer (`memmove` from
libeapalgorithms.dll, KERN-EXEC 3). Games that wait on the audio server then hung: Angry Birds 2.0
froze on its pause menu.

## F3 0x34 / 0x35: "data written" and the virtual DSP clock

Request 0x34 (Aamm `FUN_805e7f00`, 22 bytes) [verified offsets]: u16 @10 streamType, u32 @12 write
position (aamdaif `+0x90`, bytes), u16 @16 position in 16-bit words (`+0x94`), u16 @18 and @20 from
the stream format (sample-rate code and channels code through Aamm's converters) [inferred].

Reply 0x35: u16 @10, u32 @12 -> observer slot 1 with id 0x34 -> `FUN_806847ba(link, u32@12)` =
"compute the free space from the DSP position and write that much" [verified]. The fake modem echoes
the request's 6 bytes (`iscpeer.inl`): the DSP took everything, the engine fills the ring again.

Timing [this is the mock's model, inferred]: the real DSP answers when it has consumed; ours answers
after the time a DSP needs to play **half the ring** (`ring_units x 5 ms`, 80 ms at 16), through
`DIscPeerChannel::ReplyLater` (an NTimer in DfcThread1, serialised with the channel by the factory
mutex, dropped if the channel closed). Consequences: the engine writes at most 2x real time, never
spins, and a play request of N seconds completes in N/2..N seconds. With `ring_units = 0` the reply
is immediate (the 5730/5800 behaviour).

## F3 0x36 / 0x37: DSP position query [inferred from the 5730's layout]

Request 0x36 (Aamm `FUN_805e801c`, 18 bytes): u16 @10 streamType, u32 @12 bytes, u16 @16 position in
words; sent by aamdaif at stream start (`FUN_806844f0`) and in its state machine (`FUN_80684e34`).
Reply 0x37: u16 @10, u32 @12 (-> observer id 0x36 -> `FUN_80684716`, sets the DSP position `+0x84`
[inferred]), u16 @16 channels code (0/1), u16 @18 sample-rate code (**must be a valid code**, Aamm
panics otherwise). Reply line: `reply F3 36 37 00 00 00 00 00 00 00 00 02 00`.

## aamdaif's ring bookkeeping (for the next step) [verified unless marked]

- `+0x3c` ring bytes, `+0x44` user base, `+0x90` write position (bytes, wraps at `+0x3c`), `+0x98`
  bytes still to write after a wrap, `+0x78` bytes per frame (x `aacommon` ord 35 = frames per unit).
- `+0x84` DSP position in frames: starts at `ring / frameBytes` (whole ring free), advanced by the
  state machine (`+0x7c` 1 -> 2 -> 3 -> 0) and the 0x37 reply [inferred]. Free bytes to write =
  `frameBytes x +0x84 - ((writePos - replyPos) & 0xffff)`, capped at the distance to the ring end.
- Nothing is read back from the window: the DSP's own read pointer is never in shared memory on this
  path [verified: aamdaif only writes through `TPtr8`]. So a host-audio backend can be driven entirely
  from the stand-in: the LDD knows the ring (offset, size, stream type) and the data; pacing
  the 0x35 replies by the Syborg audio device's consumption instead of a timer closes the loop.

## Towards real sound (not done)

1. The LDD's ctl 2 dir 1 window holds the PCM in `ring_units x unit` rings per stream; a Syborg audio
   device (as the N95's `src/standins` audio path) can read region `k` of the window at the stream's
   rate. 2. Replace the fixed timer in `ReplyLater` by "answer when the device has consumed up to the
   write position of the request" (u32 @12). 3. 0x37's u32 @12 should then carry the device's read
   position (frames) [inferred meaning].

## Open points, ranked

1. The meaning of 0x35/0x37's u32 @12 (echo vs. DSP position) is inferred; a run with a tone and
   `sydb strace` on channel 36 would settle the write cadence and whether `+0x84` ever moves.
2. The stream-type -> bytes-per-10-ms map (1920/960/320) is cdspcontrolserver's; the N8 mixer may
   use only type 0.
3. `ring_units` and the 128 KB regions are arbitrary; a real DSP map comes from F3 0x31's seven
   entries (all zero from us).
