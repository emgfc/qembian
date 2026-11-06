# N95 controls on Syborg

Focus the QEMU display window, then use these keyboard buttons. On a laptop,
you may need Fn with the function keys if they normally adjust volume/brightness.

| Phone control | PC keyboard | QMP key name |
|---|---|---|
| Left softkey (label at bottom left: OK, Options, Select, etc.) | F5 | `f5` |
| Centre of navigation pad / Select | F6 | `f6` |
| Right softkey (label at bottom right: Back, Cancel, Exit, etc.) | F7 | `f7` |
| Applications menu | F8 | `f8` |
| Navigation pad | Arrow keys | `up`, `down`, `left`, `right` |
| Clear text | Backspace | `backspace` |
| `*` key | F9 or numeric-keypad `*` | `f9`, `kp_multiply` |
| `#` key | F10 (or `\`) | `f10` |
| Text/numbers in edit fields | Letter/digit keys | corresponding QMP keys |

**Long press** (e.g. hold F8 for the task switcher): hold the PC key. The Syborg keyboard device
reports one key-down and one key-up per press like the phone's keypad and drops the host's
autorepeat key-downs, which used to restart WSERV's own repeat/long-press timing (fixed
2026-11-06, `src/qemu-syborg/hw/arm/syborg_input.c`). From sydb: `sydb key menu:1.5`.

Enter sends the Symbian Enter key; F6 sends the phone's centre/select key. Use
F5 to activate the displayed **OK** softkey during initial date/time setup.
The softkey labels are not touchscreen buttons: the N95 UI expects physical
keys, and the modern Syborg pointer device ignores mouse input by default.

## Running the updated ROM

The corrected ROM is `build/n95/hybrid.img`. Restart QEMU with it to load the
new keyboard driver; an already running emulator retains the old ROM.

```sh
scripts/run-modern.sh --gui --qmp /tmp/n95-controls.sock \
  --dtb build/n95/syborg-n95.dtb build/n95/hybrid.img
```

With this command, buttons can also be pressed from another terminal:

```sh
python3 tools/qmp.py /tmp/n95-controls.sock key right
python3 tools/qmp.py /tmp/n95-controls.sock key up
python3 tools/qmp.py /tmp/n95-controls.sock key f5
python3 tools/qmp.py /tmp/n95-controls.sock key f6
python3 tools/qmp.py /tmp/n95-controls.sock key f7
python3 tools/qmp.py /tmp/n95-controls.sock key f8
```

The launch script creates fresh NVMemory, so this starts a fresh guest session.
For an existing run, use its QMP socket if one was enabled when it started.

## Arrow-key fix and workaround for older ROMs

The original Syborg guest driver only maps the non-extended PC navigation codes
(`48`, `4b`, `4d`, `50` hex). Dedicated arrow keys produce E0-prefixed codes;
the virtual keyboard represents that prefix by setting bit 7, yielding `c8`,
`cb`, `cd`, `d0`. Those entries were `EStdKeyNull`, so the guest ignored them.

`patches/qemu-baseport/0005-keyboard-extended-navigation.patch` supplies the
missing guest mappings, including the extended Home/End/Page Up/Page Down,
Insert/Delete, keypad Enter, right Ctrl/Alt and keypad slash entries.
QEMU's input protocol is unchanged. The patch is applied by
`scripts/setup-epocroot.sh` and takes effect after rebuilding the baseport and
composing the hybrid ROM.

Until you restart with the fixed ROM, the **numeric keypad** provides a
workaround: keypad 8 = up, 2 = down, 4 = left, 6 = right. These are separate
from the number row. QMP can send them unambiguously:

```sh
python3 tools/qmp.py /tmp/n95-controls.sock key kp_8
python3 tools/qmp.py /tmp/n95-controls.sock key kp_2
python3 tools/qmp.py /tmp/n95-controls.sock key kp_4
python3 tools/qmp.py /tmp/n95-controls.sock key kp_6
```

## Verification (2026-11-05)

Built `keyboard.mmp` with GCCE 4.4.1, repacked the kernel ROM and composed
`build/n95/hybrid.img`. On the local QEMU 11.1.3 build, QMP input confirmed:

- Down changed the country selection from Afghanistan to Argentina; Up returned it.
- F5 selected the country and opened the date editor.
- Right moved from the day to the month; Left returned to the day.
- F5 accepted the date and opened the time editor.
- F6 dismissed the time editor.

Screenshots and the guest serial log are in `build/controls-test/`. This test
covers the startup controls; applications-menu and later application behavior
were not verified.

## Keypad lock (keyguard)

The N95 firmware asks "Press Unlock and then \*". Its keyguard (`aknoldstylenotif.dll`,
0x82ddc620, verified from the code) accepts after **Unlock (F5)** either `*` (F9 / numpad `*`,
since 2026-11-06) or the **right softkey (F7)**, within its time-out. While locked, each key waits
~9 s for the modem's emergency-number check (not answered by the fake modem yet), so press Unlock,
wait for the "Now press *" note, then press `*`/F7. The "stuck idle screen after the menu" was a
different bug (Offline profile switch failed; fixed 2026-11-06, docs/roadmap.md).
