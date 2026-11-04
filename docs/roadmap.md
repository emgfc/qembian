# Roadmap

Decision (2026-11-04, by the project owner): **option 1 first, then option A.** Options are
defined in [n95-compatibility.md §6](n95-compatibility.md).

1. **Option 1: our EKA2 kernel with a Symbian 9.2 exec personality, running the N95 8GB user side
   byte-identical** on the Syborg virtual platform. This continues until the N95 user side is
   "working": the home screen, Avkon/CONE/Uikon/WSERV up, and apps launchable.
2. **Option A: emulate the N95's OMAP2420 platform** (QEMU `n800`-family machine) and run the
   unmodified Nokia kernel and ROM.

Why this order:
- Option 1 runs on a kernel we can build, trace and debug, so user-side problems can be told apart
  from hardware problems.
- What it produces carries over to option A:
  - ROM composition, unpaging and inspection tools
  - the exec trace
  - the inventory of Nokia hardware-facing servers and the fake peers they need (modem ISA,
    DOS/SysAp power, accessories)
  - a known-good reference boot to compare against

## Option 1 work plan

| Step | Content | Exit criterion |
|---|---|---|
| E1.1 | ROM composer: our kernel-side files + the N95 user-side files at their original XIP addresses + a new root directory/TRomHeader; paging off | composed image passes `romimg.py`; every N95 user file byte-identical at its original address |
| E1.2 | 9.2 exec table in our kernel: same dispatch mechanism, 9.2 numbering; unknown/unimplemented calls trace + fault; per-call serial trace switch | kernel builds; our own text shell still boots with a matching EUSER (regression check) |
| E1.3 | Boot the composed ROM: N95 `efile.exe` as file server | `efile` reaches its first IPC/loader calls; every stall named by the SWI trace |
| E1.4 | 9.2 semantics shims as hit (active scheduler / trap handler fast calls, DLL lock and attach, `MutexWait`, loader structures) | N95 `EStart.exe` runs |
| E1.5 | Hardware-facing stubs (DOS/SysAp, modem, accessories), display 240×320, keypad mapping | N95 `ewsrv.exe` draws, then Starter/SysAp, then the home screen |

The status of each step is tracked in [build-log.md](build-log.md) (option 1 section) and the
README.

**Status 2026-11-04 (end of day):**
- E1.1–E1.3 done.
- E1.4: shims added as hit (HandleDuplicate, MutexWait, SafeInc/Dec, loader struct size).
- E1.5 in progress. Hardware stand-ins exist for ISC (silent modem), Custom, the display driver,
  the ROFS media and a 240×320 32 bpp LCD. Nokia's WSERV, FBSERV, ECOM, Starter and DosServer run.
- Fake modem peer (ISI over ISC) done for the boot-critical messages; Starter runs its start list.
- The next blocker is `DATAPORT.CSY` leaving in the comms server; needs a ROFS E32-image loader for analysis.

## Option A (later)

Start from the QEMU 0.9.1 `n800` machine (OMAP2420, Menelaus, OneNAND, CBUS, I2C, SPI already
modelled). Boot the flat N95 core image directly, skipping NOLO and XZIP. Then add Nokia-specific
devices in the order Nokia's EKERN touches them: GAPE, STI, the display and keypad path, the ISC
modem link (fake CMT peer), DVFS, the PowerVR and DSP bridge stubs.
