# Nokia N73 (RM-132)

Symbian OS 9.1, S60 3rd Edition, TI OMAP1710 (ARM926EJ-S). First 9.1 device and first moving-memory-
model device of the platform. Firmware: RM-132 v3.0717.1.4.1 western (sw.txt 23-04-2007), in
`roms/RM-132_Nokia-N73/3.0717.1.4.1/` (git-ignored; `FW_DIR=... tools/fw/inventory.py --device
nokia-n73-rm132`). How it was brought up and every finding: `docs/devices/nokia-n73-rm132/onboarding-log.md`.

## Level: L1 (home screen and menu)

Boots our moving-model kernel (`baseport syborgmv`, `-cpu arm926`) with the 9.1 personality
(`personalities/sym91`) to Active Standby ("Offline", no SIM) in about 48 s on the first boot, after the
city, time and date queries (answer with LSK) and the tutorial prompt (RSK). The menu opens and
applications start (Calendar checked). The UI is Chinese: this archive's language pack (`.V16`) holds
Chinese PRC (31) and language 159 only; switch in Settings.

Build and boot:
```
tools/device.py build nokia-n73-rm132 --kernel
export SYBORG_DEVICE=nokia-n73-rm132
tools/sydb/sydb.py --session build/sydb-n73 start --fresh --paused
tools/sydb/sydb.py --session build/sydb-n73 catch add --kind panic --action freeze --snapshot
tools/sydb/sydb.py --session build/sydb-n73 resume
```

## Hardware model

| part | from the firmware | ours |
|---|---|---|
| display | wsini.ini: 240x320 portrait, COLOR16MU, 320x240 at 90 degrees | `lcd_portrait` (stride 960), `d_display` with 23 caps words, portrait panel |
| keys | ekeyp.dll 6x6 matrix (keymap.toml) | QEMU translates host keys to those scan codes |
| drives | ESTARTCOMP: D: IRAM, C: NAND (6), Z: composite (7), E: MMC (1) | C: NVMemory (1), E: NVMemory 2 (2), Z: ROFS on 8 |
| modem | IscApi = the N95's protocol, older driver names | `iscdriver`, `iscdriverextension`, `ssi` stand-ins + the shared fake peer |
| DSP / audio | ehwa.ldd / ehwaomap.pdd (OMAP1710 bridge) | none yet (open item 1) |

## Hacks

No byte patches on the firmware. Platform-wide ones in `docs/hacks.md` apply (fake modem, stub
certificate, ...). The DSP bridge and Custom stand-ins run the variants this profile declares
(`config = { bridge = "omap1710" }`, `config = { api = 1 }`).

## Open items

1. Smoke test: needs seeded drives so the first-boot queries do not hold the idle milestone.
2. FaxModem.exe panics KERN-EXEC 3 three times after idle (not needed for the UI).
5. Start-up tone: the DSP node is created and fed; audio output not checked.
3. Camera lens-cover switch, landscape (camera) mode, twips of the 2.4" panel.
4. Modem: `PN_PHONE_INFO` 0x15 (INFO_PRODUCT_INFO_READ, selector 0x0A, from EInfoServer) is the only
   unanswered ISI request of the boot so far; the N95 never sends it, so its reply layout is unknown
   (left unanswered rather than guessed).

## Checklist (docs/adding-a-device.md)

- [x] firmware obtained, `firmware.sha256` written, nothing proprietary staged
- [x] inventory: core-flat.img (inflated SOS*CORE), core.json, rofs1/2, abi
- [x] personality: sym91 (exec numbering, SWI return, TLS calls, loader structures, export directory)
- [x] device.toml, keymap.toml (`device.py check` green)
- [x] `device.py build` composes
- [x] L0: user processes start
- [x] L1: idle screen, menu, an application (agent and smoke test open)
- [ ] L2..L4
