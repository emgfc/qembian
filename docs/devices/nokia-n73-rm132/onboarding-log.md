# Nokia N73 (RM-132): onboarding log

What was found while adding the N73 (first Symbian 9.1 device) and where `docs/adding-a-device.md`
did not help, so the flow gets better with each phone. [V] = checked in the firmware or by a run,
[I] = inferred. Firmware: `roms/RM-132_Nokia-N73/3.0717.1.4.1/` (RM-132 v3.0717.1.4.1 western, git-ignored).

## Firmware container

- Same FIASCO/BB5 container as the N95 (`tools/fiasco.py` parses it unchanged) [V], but the files and
  partitions differ:
  - `N73_3.0717.1.4.1_western_C00_PRD_cc.fpsx`: core. Partition **`SOS*CORE`** (not `SOS+CORE`):
    the XIP core image is **compressed** and ROFS1 follows it uncompressed in the same partition.
  - `N73_rofx_3.0717.1.4.1_PRD.V16`: the language ROFS (`ROFx` magic) as a bare chunk stream, no
    partition record (`_pre`).
  - `N73_userarea_3.0717.1.4.1.U03`: a FAT16 user area image (C: defaults), also `_pre`.
- **Compression [V]**: the flashed partition (record payload at 0x420000 + chunks from 0x420400) starts
  with a 0x3D0-byte raw header (`a3 95 97 80` partition image header), followed by **one raw
  deflate stream** (zlib 1.2.1 `inflateInit2_(-15)` in the `SOS_XZIP`/`SOS_UZIP` loader, which copies
  the header verbatim and inflates the rest into RAM). Inflated: 0x1353030 bytes; the ROM image starts
  at +0x30 (= flash offset 0x400 of the decompressed area). Compressed stream ends at partition offset
  0xACD3E8; ROFS1 starts at 0xE7FC00. Found by reading the loader's driver function (memcpy 0x3D0, then
  inflate) after blind inflate attempts at offset 0 failed.
- The core is **not demand paged** (9.1); no `rom_unpage` step.

## ROM layout: a moving-memory-model ROM

- `romBase = 0xF8000000`, `kernData = 0x64000000`, ROM size 0x1400000 (20 MB) [V header]. That is the
  **moving memory model** (ARMv5, OMAP1710's ARM926EJ-S), not the multiple model our N95 kernel uses.
- Five **fixed processes** with data at moving-model kernel-section addresses [V core.json]: efile.exe
  (0x65100000), c32exe.exe (0x65400000), FbServ.exe (0x65600000), EwSrv.exe (0x65800000),
  AknIconSrv.exe (0x65B00000). Only a moving-model kernel can host them at those addresses; user DLLs
  in the core have no writable static data.
- Consequence: a second kernel build, **moving memory model on ARM926** (`baseport/syborgmv`, generated
  by `tools/baseport_variant.py` from the patched Syborg baseport, which already carried the commented
  moving/ARM926 options). Remapping a 0xF8000000 ROM into the multiple model would have meant moving
  the kernel's own mappings and faking fixed processes: an imitation, not compatibility.
- Getting the moving kernel up: two GCCE asm constraint errors (patch
  `patches/kernelhwsrv/platform/0011-moving-model-gcce.patch`) and QEMU's `arm926` cache type register
  (ctype 0, which no ARM926EJ-S has; `patches/qemu-modern/0003-arm926-cache-type.patch`, now OMAP1710's
  32K I / 16K D). The text-shell ROM then boots to EStart on `-cpu arm926` [V].

## Personality sym91

- EUSER exec stubs: the 9.1 table is **the 9.2 table minus what 9.2 appended** (F1b, Se6..Sed): the
  ordered SWI stub sequences of the N73 and N95 EUser.dll are identical apart from those [V,
  `difflib` over the stub order]. Export-name pairing (`tools/personality/derive_map.py`) agreed on 117
  calls but is noisy where an export's SWI list is a call-graph closure, so the stub order decided.
- Loader structures differ [V, copy-in sizes in the phones' own ekern exec handlers]:
  | structure | 9.1 (N73) | 9.2 (N95) | ours |
  |---|---|---|---|
  | TCodeSegCreateInfo | 380 (ends at iClientProcessHandle) | 472 | 472 |
  | TProcessCreateInfo | 416 (= 380 + 36) | 512 | 520 |
  | TLibraryCreateInfo | 16 | 16 | 16 |
  9.1 lacks the demand-paging fields, so TProcessCreateInfo's own fields move: the kernel now copies
  in/out per personality (`personalities/sym91/patches/0012-sym91-loader-abi.patch`). Without it our
  kernel would have written 92 bytes past efile.exe's structure on its stack.
- The slow exec table of the N73's ekern has 0xE9 entries (3 more than EUSER uses).

## Composing a moving-model ROM

- **Interleaved layout [V]**: the N73 ROM mixes kernel- and user-side files (user IscApi.dll at
  0xF8093120, kernel ebridgeutil.ldd up to 0xF8529450), so the N95's single cut point does not exist.
  Our kernel side goes **after the firmware image** instead: `kernel.place = "after-firmware"` makes
  `scripts/build-standins.sh` put a pad file right after our ROM directory, rombuild then lays our
  kernel files out from 0xF9353000; `tools/romcompose.py` checks each of our files against the
  firmware's user side and keeps our whole tail (rombuild puts each image's DLL reference table and
  some export directories just past the extent the directory lists).
- **Kernel limit [V]**: our ROM header's iKernelLimit (0x65100000) was exactly efile's fixed-process
  data address; the kernel put supervisor stacks there. romcompose keeps the firmware's (0x65D00000)
  when its user side has fixed processes.
- **GCCE DLL entry glue [V]**: `AddressIsInCodeSegment` (kernel d_entry.cpp, user uc_dll.cpp) had two
  data words in the instruction stream. At 0x80000000 they execute as harmless conditional ops; at
  0xF9xxxxxx they fault, so detaching a kernel DLL crashed
  (`patches/kernelhwsrv/platform/0013-gcce-dll-entry-inline-words.patch`).

## Running the 9.1 user side (personality sym91, personalities/sym91/README.md)

- **SWI return convention [V]**: 9.1 EUSER stubs are `mov ip, lr; swi n` with no return instruction;
  the N73's own ekern saves `{fp, ip}` and returns through a user-mode `bx ip` trampoline. Our kernel
  returned to the instruction after the SWI, so every exec call fell through into the next stubs
  (first symptom: efile's ProcessCommandLineLength "bad handle", really HalFunction falling through).
- **TLS calls [V]**: `UserSvr::DllTls(handle)` / `DllSetTls(handle, ptr)` have no DLL-uid argument in
  9.1 (9.2 passes 0). Symptom: the RVCT runtime's exception globals were NULL, the first leave crashed
  in `__cxa_allocate_exception`. Two shims in `personalities/sym91/exec.map`.
- **Export directory [V by symptom]**: our ReadExportDir copies the ordinal-0 word first (9.2+); the
  9.1 loader indexes buf[ordinal-1], so every RAM-loaded import was off by one (StartupLanguageService
  ordinal 8 ran export 7, a NULL store). Moving-model ReadExportDir reads from ordinal 1 for sym91.
- **Custom LDD [V]**: the N73 opens it as interface 1.0 and does the start-up handshake with requests
  (0 = sync, 1 = start-up mode into a TInt, 5 s timeout -> mode 10); the N95 uses 2.0 with controls.
  One stand-in serves both by the client's version (hooks now receive it).
- **Display driver [V]**: the N73's CDisplayDriver keeps 23 caps words (0x5C bytes); the N95 stand-in
  wrote 26 (a 12-byte heap overflow). `d_display` now takes caps size, panel orientation, stride and
  panel rows from the board config (`[board.display]` caps_words, native_orientation, line_pitch,
  panel_rows); `lcd_portrait` is the LCD HAL without the N95's landscape-stride changes.
- **Not a fault**: kernel "Exc 1" dumps from `K::USafeReadAndParseDesHeader` with tiny addresses: ECom
  sends an IPC with an integer flagged as a descriptor; the kernel marks the argument undefined and
  the server reads the raw value. Debug-kernel noise.
- **Find handles [V]**: 9.1 passes a find handle as one TInt (by value to FindHandleOpen, by reference to
  ObjectNext); ours is a 16-byte TFindHandle. ObjectNext wrote 16 bytes into a 4-byte field and
  FindHandleOpen read through the value as a pointer (alignment fault in MmfAudioServer). Shims
  `ObjectNext91`/`FindHandleOpen91`; the object id is recovered from the container. A diff of every
  exec handler's copy sizes between the N73 and N95 kernels found no other structure change.
- **LockedInc/LockedDec [V]**: on ARMv5 (no LDREX) 9.1's User::LockedInc/LockedDec are fast execs
  F0A/F0B, `!invalid` in the 9.2 map (the N95 EUser uses LDREX). RCriticalSection::Wait always blocked:
  FbServ and the window server deadlocked, the screen stayed white. Shims `LockedInc91`/`LockedDec91`;
  the start-up splash renders after it.
- **DSP bridge [V]**: the N73's bridge is the N95 protocol except the image load (ctl 0x4000002: image
  in the client's buffer, return 0; docs/devices/nokia-n73-rm132/dspbridge-n73.md). Picked by the
  client's argument at first (a hack); now declared in the profile (`config = { bridge = "omap1710" }`).
- **SIM and telephony [V]**: after SIM_STATUS (disconnected) NokiaDSY asks PN_SIM 0x3B "fa 00" (IMSI
  equal?, docs/devices/nokia-n73-rm132/isi-sim-3b.md; reply 3C FA 2A). The reply sat in the queue:
  DosServer was blocked in a synchronous ETel call (custom request 1000019) whose TSY waited for an
  unanswered PN_PHONE_INFO 0x02 (product profile read, older numbering of the N95's 0x29), so it never
  read channel 24 again. Found by lining up the IPC trace (`syb_report` "likely blocked") with the
  ISCPEER send times. Then SIM 0x29/0x31 ("not available"); start-up ran to Active Standby.
- **Audio gateway [V]**: eapgwc.dll asks for its shared buffer with ctl 0x400000A (N95: 0x400000B);
  the stand-in ignored it and eapgwc dereferenced NULL. Same body now.
- **Result**: Active Standby at ~48 s (first boot, queries answered), menu and Calendar work.
- **Was open: DSP bridge [V]**: the N95's TI DSP bridge stand-in (ehwa/ehwaomap, OMAP2420) answers the
  N73's bridgeapi.dll wrongly (it calls RMutex::Wait on a garbage object after ctl 0x4000002). Left
  out; but then MmfAudioServer's resource thread dies, and when MmfAudioServer exits 30 s later EikSrvs
  crashes (null + 4). The OMAP1710 bridge needs its own mock before the N73 reaches idle.

## How the bugs were found (method that worked)

`sydb start --paused` (QEMU `-S`), `sydb catch add --kind panic --action freeze --snapshot`, `sydb
resume`: the first catch gives the thread, PC, caller and a stack scan; `sydb dbg break` on the
suspect exec stub or kernel function then shows registers and the kernel backtrace (kernel symbols
of the device's own kernel build: `tools/ksyms.py` reads them from the kernel-side ROM). The first
three faults were read from register dumps alone, which cost two rebuilds; with the paused start and
catches each later fault took one boot.

## Where the onboarding flow did not help (feed back into docs/adding-a-device.md)

1. Step 2 assumes `tools/fw/inventory.py` handles the container; it only knows uncompressed `SOS+CORE`
   + `SOS+ROFS` partitions and a paged core. Needed: compressed `SOS*` partitions (raw header + raw
   deflate), ROFS inside the core partition, partition-less `_pre` ROFx/UDA files.
2. Nothing in the flow checks **the ROM's memory model and base** before the personality step; it is
   the first thing to look at (TRomHeader iRomBase/iKernDataAddress), because it decides the kernel
   build. `[firmware.rom].memory_model` existed in the schema but no tool read it.
3. Step 3 (personality) only talks about exec numbering and driver vtables. The **loader structures**
   (efile.exe is the firmware's own) are a third ABI surface; the measurement method (copy-in size
   constants in the phone's own ekern handlers, found through its slow exec table) belongs there.
4. `tools/romcompose.py` hard-coded ROM base 0x80000000.
5. The flow says "`tools/device.py new`" which does not exist.
6. Step 5's first boot should start **paused with catches armed** (`sydb start --paused`, catch,
   `resume`): early faults happen in the first second, before a catch can be added to a running
   session. `--paused` did not exist; added.
7. sydb's debugger, `e32load.py`, `romcompose.py` and `ksyms.py` assumed the N95's ROM base and kernel
   addresses; all now read them from the ROM header or the device's kernel-side ROM.
8. The personality step did not mention the **stub shape** (return convention) or wrappers that branch
   straight to a stub with fewer arguments: both are visible in a few minutes of disassembly and
   each one broke the 9.1 boot.
9. "Copy the N95's stand-ins" is only safe after checking each client's interface: the DisplayDriver
   caps size, the Custom interface version and the DSP bridge all differed. Rule: compare the
   client DLL's requests (control numbers, buffer sizes, opened interface version) before reusing a
   stand-in; a wrong mock is worse than a missing driver.
10. TOML: `[standins.missing]` after `[[standins]]` attached to the *last* stand-in entry, so
    `device.py get standins.missing.drivers` found nothing (also in the N95 profile). Fixed: the list is
    now `kernel.missing_drivers`.
