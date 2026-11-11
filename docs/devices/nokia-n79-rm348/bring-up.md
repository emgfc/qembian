# N79 (RM-348) bring-up notes (2026-11-07)

Firmware 30.019 (RM-348_30.019_prd.core / language / customer .fpsx). Same platform as the 5730
(BB5 RAPIDO, RAP3NAND, Symbian 9.3), so the profile started as a copy of the 5730's. Two boots of
fixing took it from first boot to the home screen and menu (L1). Seeded boot to idle: 15 s.

What was the same as the 5730 [V, tools/fw/probe.py]: ROM layout (multiple model, after-firmware
kernel side), sym92 call numbering and exec structure sizes (0 differences), display (240x320, 32 bpp
mode list), drive map (Z: on local drives 9/10/11), stand-in set, fake-modem replies, start-up waits
(SIM_IND on channel 35, SIM lock status, GetLockInfo).

What differed:
- **SEC_STATE_RESP length.** SAServer sends `08 11` on channel 35 (the 5730 never took that path,
  although its SAStartupService code is identical). The reader 0x809675fc takes bytes [10], [11] and
  [12]; the fake modem answered two bytes, so SAServer panicked `IsiMessage: OFFS -9`. The shared reply
  is now three bytes (`05 00 00`).
- **Keypad.** ITU-T keypad (the 5730 is a QWERTY slider): keymap.toml from the N79's keypad_exp.dll
  matrix at 0x8010b710, entries as on the N95. One code the N95 does not have: EStdKeyDeviceF (matrix 89),
  which physical key it is [I].
- **Drivers.** No magnetometer; NaviScroll LDD/PDD (the touch ring) added, declared missing.
- **Partition names.** The .fpsx files name the partitions `SOS+CORE`; tools/fw/inventory.py now
  accepts `+` and `_` alike.
- **Keyguard.** The keypad locked itself after about a minute without key presses on the home
  screen (unlock: left soft key, then `*`, `sydb key lsk star --delay 0.4`) [I: automatic keyguard default].
