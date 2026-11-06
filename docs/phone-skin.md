# Phone skin: an emulator window with hardware buttons (plan, 2026-11-06)

Goal: a window like the Android emulator's. It shows the N95 screen plus a button bar: power,
reset, volume, camera, media keys, multimedia key, and the slider (closed / keypad open / media
keys open, with landscape). Status: analysis only. [V] = checked, [I] = inferred.

## What we have today

- QEMU's GTK window [V]: shows the 240×320 console, takes PC keys (docs/n95-controls.md), and its
  Machine menu has Pause / Reset / Power down / Quit. QEMU's own UI cannot get custom buttons
  without patching QEMU's GTK code, which we don't want to maintain.
- Display backends in our QEMU 11.1.3 [V]: gtk, sdl, dbus, egl-headless, curses, none; VNC with
  `-vnc` as well.
- Input: QMP `input-send-event` keys (the sydb daemon's `key` op) → Syborg keyboard → our
  `EKEYB.DLL` (PC scan code → EStdKey) → the N95's own `EKData.dll` keymap (user side, unchanged
  [V hybrid manifest]) → WSERV.
- Host: Python 3 with PyQt6 and Tk available, Pillow [V]; no GTK4 bindings.
- The display path renders every WSERV rotation correctly since 2026-11-06 (fb ORIENTATION, 1280-byte
  lines, owner-checked on the screensaver), so a landscape screen mode already works on the guest side.

## How the Android emulator does it (for comparison)

The Android emulator is a QEMU fork (AOSP `external/qemu`, goldfish/ranchu boards) that does not use
QEMU's own display front end:
1. **UI linked into the emulator binary.** First an SDL skin (phone images and layouts from the
   SDK skin files), then since about 2016 a Qt window with the side bar and "extended controls".
   Buttons go to a virtual input device; rotation, sensors and battery go through virtual devices
   and the host-to-guest pipe; there is also a text console (`adb emu`, telnet 5554).
2. **Headless emulator plus a separate UI client.** Android Studio's embedded emulator (about
   2020+) runs it without a window. Frames and input travel over the emulator's gRPC service.

We follow model 2: QEMU headless (`-display none` plus a VNC/D-Bus socket), and `sydb phone` as the
only visible window, talking to the sydb daemon (our counterpart of the gRPC service). Model 1, our
own display front end compiled into our QEMU, stays an option only if the separate window is too
slow.

## Design: a separate host app, `sydb phone`

A PyQt6 window (`tools/sydb/phone.py`) attached to a running sydb session. It does not modify QEMU.

| Part | How |
|---|---|
| Screen | QEMU `-vnc unix:build/sydb/vnc.sock` next to (or instead of) the GTK window; a minimal RFB client (raw encoding, 240×320×32 bpp, about 150 lines) gives 30–60 fps with no extra dependencies. Fallback: QMP `screendump` polling (~5–10 fps). `-display dbus` is the modern alternative (shared-memory scanout), but its client needs D-Bus fd passing; use it only if VNC is too slow. |
| Phone body | The screen widget rotated by 0 or 90° depending on the slider position (the user turns the phone for media mode, like the Android emulator's rotate button), with the buttons around it. Original drawing, no Nokia artwork. |
| Keys | Mouse clicks and the PC keyboard → sydb daemon `key` op (QMP). Press and release are separate (long press for power, the camera's half press). |
| Machine | Reset → QMP `system_reset`; Pause/Resume → QMP `stop`/`cont`; Power → the phone's power key (guest shutdown); "Cold boot" → `sydb stop`/`start`; Screenshot → `sydb screenshot`. |
| Status | Uptime, agent, focus, latest events from the daemon (as `sydb tui` does). |

## Each control: host side vs guest side

| Control | Host (skin) | Guest side needed | Effort |
|---|---|---|---|
| Reset / reboot | QMP `system_reset` | none (drives persist) | done in QEMU, button only |
| Pause / resume, screenshot | QMP | none | button only |
| Power key: short press (profile list), long press (switch off) | key down/up | map a PC scan code to the N95 power key's EStdKey in our `EKEYB` table [I: EStdKeyDevice2, check in EKData.dll]. A real switch-off then needs our BSP power-off path to end QEMU (or reboot) [I] | S |
| Volume up / down | keys | EKEYB entries for EStdKeyIncVolume/DecVolume [I: N95 keymap codes to confirm] | S |
| Camera key, half press (focus) / full press (capture) | two keys | EKEYB entries for the N95's two camera scan codes [I]. The Camera app then needs a camera device (none yet; a host webcam or test-pattern stand-in later) | S for keys, L for camera |
| Multimedia key | key | EKEYB entry [I]. It opens Carousel, which needs the GPU work (docs/gpu-roadmap.md) | S + GPU |
| Media keys (play/pause, stop, prev, next) | keys | EKEYB entries [I]; the music player listens through `mpxmediakeyhandler.dll` / RemCon key conversion [V files exist, I path] | S–M |
| Slider: closed / keypad open / media keys open | 3-position switch; media position rotates the skin | Nokia's kernel `keypad.dll` (0x8014e4b0, N95 kernel area, **not in our hybrid** [V]) reads the slide switches. We need a stand-in that reports slide state the way SysAp/Avkon expect (switch key events and/or a P&S/HAL value [I]), plus the landscape screen-mode switch on media-open [I: SysAp or Avkon asks WSERV]. Needs a short research brief (mock-level) | M |
| Auto-rotate (accelerometer) | rotate button / drag | Nokia's `accelerometerldd.ldd` / `accelerometerpdd.pdd` are kernel side and absent [V]; user side `RRSensorApi.dll` [V]. A stand-in LDD fed from the skin | M, later |
| Headset, charger, memory card in/out | toggles | accessory/charger P&S and the MMC door switch (stand-ins exist only partly) [I] | M, later |

S = hours, M = a day or two, L = a week or more.

## Order

1. `sydb phone` window: VNC screen, keypad keys already mapped, machine buttons (reset, pause,
   screenshot, cold boot). Pure host work.
2. Side keys: power, volume, camera (2 stages), multimedia, media keys. Each is one `EKEYB` table entry
   plus one QEMU key name, after confirming the N95 scan codes from `EKData.dll`.
3. Slider stand-in, after a mock-level research brief on how `keypad.dll` reports slides and who
   consumes it (SysAp, Avkon, AknKeyRotator), then landscape on media-open.
4. Accelerometer stand-in and accessory toggles, when an app needs them.

The skin talks only to the sydb daemon, so the same buttons can become MCP tools (`ui_key` already
takes key names; slider/power would get their own ops).
