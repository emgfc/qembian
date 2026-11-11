# N-Gage QD (EKA1) on the `ngage` board: start here

The original Nokia N-Gage QD firmware (RH-29 V 04.10, Symbian 6.1, EKA1 kernel, Series 60
1.x) boots unmodified on our own QEMU board `ngage`. This page covers:
- the state of the work;
- how to run the phone;
- how the pieces fit;
- how to debug;
- what to do next.

Related documents:

| Document | Contents |
|---|---|
| [eka1-ngage-questions.md](eka1-ngage-questions.md) | the question log (Q1-Q56): every stall, its evidence and its answer, marked [V]/[I] |
| [hacks.md](hacks.md), section "N-Gage QD" | every workaround and guess in the board |
| [eka1-ngage-roadmap.md](eka1-ngage-roadmap.md) | the original plan (phases P0-P6), with status |
| [../eka1/README.md](../eka1/README.md) | the firmware files, extraction and analysis tools |
| [research/eka1-ngage-feasibility.md](research/eka1-ngage-feasibility.md) | the first feasibility study |
| [eka1-ngage-handoff.md](eka1-ngage-handoff.md) | a historical hand-off prompt (2026-11-10) |

## State (2026-11-11)

- **Boot.** Bootstrap → EKA1 kernel → EFile/EStart → window server → Starter's normal
  start-up list (40+ processes) → Startup (self-test, SIM and security checks) → the idle
  screen (N-GAGE QD wallpaper, softkeys "Contacts" and "Game").
- **Boot time.** About 7 s to the home screen once the phone's flash is kept. The first boot
  formats C: and restores factory settings: about 40 s to the logo, 95 s to the UI.
- **Keys.** The keypad (ekeyb's 6x5 matrix) answers the first-boot time and date queries.
- **Cellular side (NOS).** It doesn't run: its code is scrambled. A stand-in stub and a
  reply table answer what Symbian asks of it: product data, the RTC hand-over, ISI over
  PhoNet (self-test, SIM, security, network "searching", audio, SMS).
- **Display.** The board shows the window server's screen chunk (H-ngage-fb); lcd.ldd's own
  transfers (probably DMA) aren't modelled.

Not working or not modelled yet: see "Next" below. Game cards need the MMC controller.

## Running it

One-time set-up:
1. The flash files in `eka1/ngage_rom/` and the extraction (`eka1/README.md`: steps 1-3).
2. `scripts/build-qemu-ngage.sh`: builds the board into its own QEMU 11.1.3 tree
   (`build/qemu-ngage/`). It applies `patches/qemu-ngage/*`; the shared Syborg QEMU is not
   touched.
3. After changing the stub, `src/qemu-ngage/nos-standin/build.sh` (it regenerates
   `ngage_nos_stub.h`; then rebuild QEMU).

The phone in a window (one command; it ends the phone when the window closes):

```sh
python3 eka1/tools/ngage_phone.py            # --phone NAME, --fresh, --scale 3, --keep
```

- **The phone's files.** `build/ngage/phones/NAME.flash` holds the phone's own flash: C: and
  its settings, the 64 KB blocks that differ from the firmware. `--fresh` deletes it.
- **Keys.**

  | Host key | Phone key |
  |---|---|
  | F1 / F2 | left / right softkey |
  | F3 / F4 | call (green) / end (red) |
  | F5 | menu |
  | arrows, Enter | navigation, select |
  | 0-9, `*`, `#` (F10) | keypad |
  | Backspace | C |
  | Left Shift | edit |
  | F12 | power (not modelled) |

  The buttons under the screen do the same.

## How it fits together

| Piece | Where | Notes |
|---|---|---|
| Board | `src/qemu-ngage/hw/arm/ngage.c` | memory map, flash chips (CFI01, read-while-write patch), interrupt controller (4 banks, bank 3 = FIQ), 6 timers, CBUS/UEM, LCD interface and console, kernel debug port → serial, catch-all logging of unmodelled pages, persistent flash (`flash=`) |
| NOS stand-in | `src/qemu-ngage/nos-standin/nos_standin.S` | copied over the NOS entry; registers the variant interfaces as trap objects; each call goes to the board's NOS device (0x0C1F0000); also the RTC hand-over and the ISI delivery DFC |
| ISI / PhoNet | `src/qemu-ngage/hw/arm/ngage_isi.c` | decodes NOSDRIVER sends; answers from `src/qemu-ngage/isi-replies.txt` (`reply RES REQ RESP BYTES`, `len=`, `echo`, `ind UNIT OBJ ...`) |
| Keypad | `src/qemu-ngage/hw/arm/ngage_keys.c` | the matrix ekeyb scans; QEMU input → matrix |
| Machine options | `-M ngage,ppm=DIR,isi=FILE,flash=FILE` | |

Environment knobs (experiments):

| Variable | Effect |
|---|---|
| `NGAGE_VERBOSE=1` | log every interrupt-controller, timer, CBUS and NOS access (slow) |
| `NGAGE_STARTCODE=N` | start-up code (default 1 = normal, -1 = off; Q51) |
| `NGAGE_ITEM12=N` | NOS product data item 12, the SW start-up reason (Q44) |
| `NGAGE_POWERON=N` | UEM power-on reason register (Q51) |

## Debugging

### Use a session, not repeated boots

`eka1/tools/ngage_sess.py` keeps one QEMU per session (`build/ngage/sess-NAME/`) and talks
to it over the gdb protocol, the HMP monitor and QMP:

```sh
S="python3 eka1/tools/ngage_sess.py -s dbg"
$S start [--flash FILE] [--catch 'fatal,pabt,dabt,VA=LABEL?EXPR,VA=~LABEL']
$S wait 200        # until a catch freezes the machine: registers, current thread, named stack
$S x VA N | regs | objects | stack PROC::THREAD | shot out.png | mon 'HMP command'
$S break VA | cont | pause | dump DIR | stop
python3 eka1/tools/ngage_sess.py dumpx build/ngage/sess-dbg/crash-1 VA N   # offline, no QEMU
```

- **Catches.**
  - Default: `fatal` (the kernel fault routine 0x5031e340), `pabt` and `dabt` (vectors
    0x0C/0x10).
  - `VA=LABEL?r1!=0`: freeze only when the condition holds.
  - `VA=~LABEL`: log time, registers, thread and the descriptor at r1 to `trace.log`, then go
    on. Use it for timelines such as process creation (EUser `RProcess::Create`
    0x503B3514). Each hit costs about 1 s, so keep it off hot paths.
  - A crash catch also writes `crash-N/` (SDRAM and state) for offline reads.
- **Useful catch addresses.**

  | Address | What |
  |---|---|
  | 0x503af660 | User::Panic |
  | 0x503b2f90 | RThread::Panic (r2 = reason; `?r2==11` for ViewSrv timeouts) |
  | 0x503b2f38 | RThread::Kill (filter with `?r1!=0`: EFile kills its helpers with 0) |
  | 0x50960e0c | Starter's list selection (r1, r2 = keys) |
  | 0x50bba6f8 | Startup's SetState (r1) |

- **The ISI picture.** Every channel send is logged
  (`grep -a ngage-isi build/ngage/sess-*/qemu.log`). Requests without a reply are marked
  UNANSWERED: check those first when something waits.
- **Logs.** `qemu.log` (board), `serial` (the kernel's debug port: RDebug output) and
  `trace.log` (logging catches). `qemu.log` is buffered, so its tail lags.
- **The phone window** uses session `phone-NAME`; any `ngage_sess` command can join it with
  `-s phone-NAME`.

### Who is blocked on whom

1. Freeze or `pause` the machine. Find the thread in `objects`.
2. Its saved SP is at DThread+0x158 (a run address).
3. In the moving model a process that isn't running has its stack chunk at its home address:
   `home = chunk base + (sp - 0x00400000)`.
4. Print the frames from there up; the deeper words in a stack scan are stale. Example
   (q88): Phone → GSMCUS `ReadSimAlsLine` → ETel `Get` → phonetsy waits for ISI `09 31`.
5. The usual chain on this phone: app → synchronous ETel/custom API call → phonetsy → an ISI
   request with a missing or wrong reply.

### Answering an ISI request

- Find the parser of the response in the client:
  - phonetsy's per-resource switches;
  - Nokia.dsy's receive virtual;
  - SoundDevice.mda.
- Take the accepted message id, the status byte, the sub-block count and the **minimum frame
  length** from it.
- isimsg reads with `Mid(offset, len)`: a reply shorter than the parser reads panics the
  client with USER 21.
- Unsolicited indications to an object that didn't subscribe panic phonet (`ECS panic` -5).
  The DSY takes SIM and security indications without a subscription.
- Mark every reply [V] (the shape from the parser) or [I] (the chosen values) in the table.

### Reading the firmware

```sh
OD=~/gcc-builds/gcc-14.2.0/bin/arm-none-symbianelf-objdump
$OD -D -b binary -m arm [-M force-thumb] --adjust-vma=0x50000000 \
    --start-address=VA --stop-address=VA2 eka1/output/rom/ROM.bin
```

- **Module layout.** `eka1/output/rom/manifest.json` gives each module's address and size;
  user exports are named in `eka1/analysis/user-exports.tsv`. Kernel exports are ordinals in
  `eka1/analysis/kernel-api-map.tsv`; names only where `ekern_evidence.tsv` proves them.
- **Imports.** An import stub is `ldr ip,[pc]; ldr pc,[ip]` plus the export table slot; map
  the slot's address back through the kernel-api-map.
- **GCC 2.9 vtables start with two zero words.** Method N is at +8+4·N from the vtable
  pointer: calling "+0x18" once ran the wrong method.
- ROM code (XIP) sits at the same address in every process, so one hardware breakpoint
  covers all of them.
- Thread and process names are 16-bit descriptors. The current thread is at
  `[0x80006740]`; the object containers are at 0x80000398 (processes) and 0x8000039c
  (threads).
- Kernel-side names from the 6.1 SDK may be one ordinal off in "range" rows of
  `ekern-align.tsv`: confirm by code before use.

### Research agents

- For a static question ("what does X need back"), a research agent with a narrow brief
  works well:
  - name the client, the request bytes and the observed state;
  - ask for the parser, the accepted reply bytes, the minimum length and [V]/[I] marks;
  - budget about 30 tool calls and no emulator (CLAUDE.md).
- Check its answer live with a catch before relying on it.

### Performance traps

- **Device accesses in the middle of a translated block are expensive.** QEMU 11 recompiles
  the block and longjmps for each one. Keep hot guest paths in RAM; the NOS idle hook is
  answered by the stub from RAM (Q56).
- **Don't log hot paths.** `ngage_pc()` unwinds the guest state, and logging every access
  cost most of the host time (28 MB of log a minute). The board logs the first 32 accesses
  per register; `NGAGE_VERBOSE` brings back the rest.
- **Measure boot time by screen changes**, not with logging catches: QMP screendump every
  second, hash the image.

## Next

1. **Menu.** The Menu key didn't open the menu in the last test. Check that it reaches
   ekeyb (scan code 0xB4), then what the menu app waits for: phonetsy sent SIM `09 23 {25}`,
   still unanswered.
2. **Game cards (MMC).** A minimal MMC controller (interrupt sources 21-24, medmmc.pdd)
   backed by a card image as E:, then a dump of a real game card. Some N-Gage cards are
   copy-protected.
3. **Time at boot.** The RTC hand-over passes the RTC's own unset time, so every boot asks
   for the time. Pass the host time in request 1 (Q49).
4. **Power key.** UEM (CBUS dev 1 reg 0x1F bit 1, UEM interrupt 6).
5. **First-boot speed.** The C: format takes about 30 s, spent in the kernel and the NOS
   stub (medint's slot 6 calls).
6. **The LCD.** lcd.ldd's own transfers (DMA), so H-ngage-fb can go (Q47).
7. **Audio, Bluetooth, emon.** BT reports HCI errors -33; "Emon server panic" 4.
8. **Device model (P5).** `models/nokia-ngage-rh29`, sydb support, a smoke test.
9. **The real NOS.** Descrambling it is the owner's decision (Q20). It would replace most of
   the stand-in.
