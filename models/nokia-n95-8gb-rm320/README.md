# Nokia N95 8GB (RM-320)

The first qembian device: S60 3rd Edition FP1, Symbian 9.2, firmware 35.0.001 (global, with WLAN).
Profile: [device.toml](model.toml), keys: [keymap.toml](keymap.toml). Variant without WLAN (China):
[../nokia-n95-8gb-rm321](../nokia-n95-8gb-rm321).

## Status (2026-11-06): level L5-gl (docs/adding-a-device.md)

Boots to Active Standby (Offline profile, no SIM), UI usable, telephony offline, GLES on the host GPU
(sygl + syborg-gl), audio through the DSP stand-in (startup jingle, game PCM at its real format),
RomPatcher+ (9.2 kernel drivers), Switch off. Smoke baseline: `tests/device-smoke.sh`:

| run | idle | Carousel (Multimedia key) | Switch off |
|---|---|---|---|
| 1 | 14.3 s | in front, no panic, host GL | session ended |
| 2 | 14.3 s | ok | ok |
| 3 | 14.1 s | ok | ok |
| soft GL | 14.1 s | ok (software rasteriser) | ok |

## Open

Keyguard emergency-number check (CALL 0x28); Carousel landscape under strict EGL (WSEGL rotation path);
camera, slide and accelerometer stand-ins; networking (packet data) and modern TLS
(docs/research/tls-modern.md); Carousel's boot-instance race (USER-EXEC 3 on some boots).
Device-specific hacks are listed in docs/hacks.md (to move here as the refactor proceeds).
