---
name: symbian-debugging
description: Playbook for debugging a qembian phone (Symbian on our EKA2 kernel in QEMU) that crashes, hangs, restarts, shows a black screen or misbehaves. Use before investigating any boot failure, panic, kernel fault, stuck app, missing UI or wrong behaviour on a device or firmware, and when bringing up a new firmware. Covers triage order, sydb catches, kernel/IPC/DLL/local-drive traces, gdb with kernel symbols, reading firmware code (ROM and ROFS), and the traps that produce wrong conclusions.
---

# Debugging a qembian phone

Tool reference: `docs/tools.md` (each tool), `docs/sydb.md` (the bridge), `docs/hacks.md` (every
workaround). This file is the method: what to look at first, how to get the facts, and how not to fool
yourself. Rules from CLAUDE.md still apply (own sydb sessions, never the user's drives, stop your
sessions, no QEMU in research subagents unless asked).

## 0. Session hygiene

- Your own session: `SYBORG_DEVICE=<id> python3 tools/sydb/sydb.py --session build/sydb-<name> ...`.
  Every sydb command for that phone needs the same `SYBORG_DEVICE` and `--session`. Or a throw-away
  instance: `tools/instance.py create <name> <id> --ephemeral --replace`, then `sydb --instance <name> ...`
  (the instance sets the device). Never a bare `sydb` and never the user's instances
  (`tools/instance.py list`): those are the user's phones. To reproduce on the user's phone state,
  `tools/instance.py clone NAME NAME-test` (drives copied; delete it after).
- `start --fresh` = blank drives (first-boot wizard, no agent on the first boot); `start --seed
  build/seeds/<id>` = drives past the wizard, agent installed (the boot hook starts it ~1 s in).
- `start --paused` stops before the first instruction: arm catches and breakpoints, then `resume`.
- `--rom FILE` boots another composed image (trace builds, below) with the same drives.
- `stop` every session you started. `pgrep -af qemu-system-arm` shows leftovers.

## 1. Triage order (cheapest decisive fact first)

1. **Did something die?** `sydb log --grep 'Panic|Killed|FAULT|Exc '` (or grep `<session>/serial.log`).
   `SYBHACK: ... not faulting` lines are system-critical threads we keep alive: they are still the bug.
2. **Unanswered modem requests.** `python3 tools/isi.py log <session>/serial.log` prints the ISI
   requests our fake modem never answered. A server stuck in a telephony call stalls everything that
   waits on it (Anna's black home screen: the TSY's NET_MODEM_REG_STATUS on PN_MODEM_NETWORK 0xC8 ->
   ETel request never completed -> connection monitor blocked -> home screen blocked in its connect).
   Compare the list with a firmware or phone that works: the difference is the suspect.
3. **What is in front?** `sydb wg` (window groups, `*` = focus), `sydb ps`, `sydb threads NAME`.
4. **Is anyone drawing?** Count draw activity per second:
   `grep -a "KHR: fill\|content updated" serial.log | awk '{print int($1)}' | uniq -c`. Compare with a
   working firmware. No draws = a stuck app or server, not a graphics bug.
5. **Who is blocked on whom?** Live: `sydb strace MATCH --secs 5` (needs the agent). From boot: the IPC
   trace (section 3); find the last `MsgAc` of the stuck thread without a matching `MsgCo`, then follow
   the server it called, recursively, until you reach a device request (ISI send, driver control).
6. **Screenshot** (`sydb screenshot OUT --scale 0.5`) only after the log facts; a black screen has many
   causes (backlight: tap first; stuck UI; composition).

## 2. Catching the moment

```
sydb start --fresh --paused
sydb catch add --kind panic --thread apsexe --action freeze --once     # or --category KERN-EXEC --reason 3
sydb catch add --kind kill --thread halsettings --action freeze         # Killed with a reason
sydb catch add --at 0x82338abc --max 1 --action freeze                  # a cold address (e.g. NKFault)
sydb resume; sleep 10; cat <session>/catches/*.txt
```
The dump has registers and a call-stack scan symbolised against ROM modules and RAM code segments.
Kernel faults: catch at `NKFault` (address from the kernel symbols, section 4). Then `sydb dbg attach`
on the frozen machine gives a real backtrace with kernel symbols.

## 3. Trace builds (kernel debug output from boot)

The udeb kernel prints `__KTRACE_OPT` output for the bits in the ROM header's trace mask. Compose a
separate image, never replace the normal one:
```
B=$(python3 tools/device.py paths <id> dir)     # the device's build dir (build/roms/<id>/<firmware key>/)
SYBORG_DEVICE=<id> python3 tools/device.py build <id> --trace 0x80000800 --out $B/hybrid-ipc.img
sydb --session build/sydb-x start --seed build/seeds/<id> --rom $B/hybrid-ipc.img --timeout 30
```
Bits (`kernel/eka/include/nkern/nk_trace.h`; keep 0x80000000): `0x800` KIPC (MsgAc/MsgCo/IPCRD: every
message, client, server, result, descriptor reads), `0x400` KDLL (code segments, LibraryAttach, call
lists), `0x00400000` KLOCDRV (local drive requests and results), `0x00800000` KFAIL. Logs get large
(KIPC: ~35k lines by 16 s); boot with a short `--timeout` and grep. Timing changes, so do not chase
races with it. File-server opcode names: `userlibandfileserver/fileserver/sfile/sf_ops.h` order
(0 AddFileSystem, 2 MountFileSystem, 26 FormatSubClose, 30 FileOpen, 52/53 FormatOpen/Next,
75 ScanDrive) - verify against the firmware, Nokia's efile may differ.

## 4. gdb with kernel symbols

The kernel sits after the firmware in the hybrid ROM, so its symbols need an offset:
```
ENTRY = word at (EKERN.EXE address from <build dir>/hybrid-manifest.txt) + 16, read from hybrid.img
OFFSET = ENTRY - 0x8108                      # _E32Startup is at 0x8108 in the .sym
sydb dbg attach
sydb dbg cmd "add-symbol-file $PWD/build/epocroot/epoc32/release/armv5/udeb/_<baseport>_ekern.exe.sym -o OFFSET"
```
`<baseport>`: `syborgfx` (flexible model: N8, 5800), `syborg` (N95), `syborgmv` (moving: N73).
Then `hbreak 'Mmu::AllocPhysicalRam(...)'`, `bt`, `p *(SPageInfo*)ADDR`, `finish`, `watch *(unsigned*)VA`
(found who wiped a DLL's static data: `DEpocCodeSeg::InitData` from a nested `LibraryAttach`).
- Conditions: `condition 1 (int)$r0 < 0` or `($r0 & 0xffffffff) == 0xffffffec`; a plain `$r0 == -20`
  silently never matched.
- After `continue`, wait with `sydb dbg wait 30` and read the JSON (`thread`, `pc`, `at`, `lr`); read
  registers with `sydb dbg regs` (JSON). `dbg cmd` returns while the target runs; ask again after the wait.
- Function arguments shown at a breakpoint on entry are garbage until the prologue ran; use registers.
- User addresses (0x00400000.., 0x7xxxxxxx) are per process: gdb reads the current process only. In the
  flexible model ROFS DLL data sits at the same address in every process (watchpoints hit everywhere:
  check the thread in each stop).
- User-side breakpoints on EUSER exports: address from `syb_sym.py sym 'User::Leave\(int\)'`; filter by
  caller with `condition N (unsigned)$lr >= LO && (unsigned)$lr < HI`.

## 5. Reading firmware code

- ROM (core) modules: `tools/syb_sym.py addr|sym|str|callers|mod` and `tools/xipdis.py core-flat.img
  core.json EUser.dll > out.dis` (whole module, imports named).
- ROFS (RAM-loaded) modules, e.g. backend.dll, libGLESv1_CM.dll: `tools/rofs.py IMG` lists,
  `tools/rofs.py --extract sys/bin/X.dll OUT IMG`, then `tools/e32load.py OUT X.elf --base CODE --rom
  core-flat.img --rom-json core.json` (deflate and byte-pair). CODE and the data address come from the
  `SYBCODE:` line of the boot log. `strings -el X.elf` shows panic texts and DLL names quickly.
- Raw Thumb disassembly when objdump treats code as data:
  `objcopy -O binary -j .text X.elf X.text; objdump -D -b binary -m arm -M force-thumb --adjust-vma=BASE`.
- Exec stubs are ARM `svc N; bx lr` pairs in EUSER: read N from the stub a function calls.
- `tools/fw/probe.py --device ID header` (ROM layout), `tools/swi_map.py` reports in
  `<build dir>/abi/` (exec numbering vs ours: `swi-n95-vs-ours.txt`; build dir = `tools/device.py paths ID dir`).

## 6. Traps that produced wrong conclusions here

- **Symbol names are nearest-export-below.** `syb_sym` and the catch dumps name an internal function
  as `SomeExport+offset` (`TRealX::TRealX(double)+b4`, `TChar::IsSpace()+e`): the name is meaningless
  for large offsets. Names come from the profile's `firmware.sdk_lib` import libraries: the wrong SDK
  (a 9.2 SDK for a Symbian^3 ROM) or a firmware newer than the SDK (Belle-only ordinals show as
  `ordNNNN`) gives wrong or no names. Run with the right `SYBORG_DEVICE`; `syb_sym.py reindex` after the
  firmware changed.
- **Our source is not the firmware.** Fault enums, opcode numbers, struct layouts (TLocalThreadData grew
  on Belle, TLocDrvRequest flags), kernel call numbers can differ from our EPL tree. Confirm against
  the firmware's disassembly before trusting a name from our headers.
- **Garbage in a buffer can be legitimate** (blank media before a format reads zeros/garbage). Check
  which call and which drive a dump belongs to (thread name, request object, timestamp).
- **Stack scans list stale words.** Only consecutive frames near the top are trustworthy.
- **Baseport variants are copies.** `build/epocroot/sf/adapt/qemu/baseport/syborgfx|syborgmv` are made
  from `syborg` at setup: an edit to `syborg/` only is not built for the N8/5800/N73. Edit `syborg/`,
  write the patch (`patches/qemu-baseport/NNNN-*.patch`, labels `a/baseport/syborg/...`), copy the file
  into the variants, add the patch name to `.patches-applied`.
- **Stand-in protocol variants are declared, never guessed** from arguments (`[[standins]] config`,
  e.g. `d_display.partial = 0` for the N8).
- **The smoke test boots the device you name** (`tests/device-smoke.sh ID`, seeds in
  `build/seeds/<id>`); before 2026-11-08 it always booted the N95.
- **Timing changes behaviour.** Breakpoints on hot paths and trace builds move races; prefer catches.

## 7. Comparing firmwares (bring-up)

### Lesson from the Settings-entry stall (2026-11-09)

Read `docs/devices/nokia-5800-rm356/settings-delay.md` and the review in
`docs/devices/nokia-n8-rm596/settings-delay.md` before re-investigating slow Settings.
The 5800 cause was a wrong SIM service byte in a delivered response, not a missing
packet. The earlier IPC investigation incorrectly excluded the modem.

- Reproduce the exact timing and navigation the user gives. A visible UI may
  still be initializing synchronously and unable to process the next tap.
- Treat IPC duration rankings as leads, not a wait graph. Separate asynchronous
  notifications from synchronous requests. Inspect raw matching calls/completions;
  the current tool pairs only client name/opcode, without message/session IDs.
- Track thread renames (`GSApp.exe::GSApp` becomes `Settings::Settings` here).
  A convenient filter can remove the very request being investigated.
- `User::WaitForRequest` does **not** distinguish an idle scheduler from a
  synchronous wrapper waiting for completion. Inspect the caller and request.
- A modem `deliver` log does **not** prove protocol success. Validate transaction
  and service identifiers, payload shape/status, and the upper-layer completion.
- Prefer a reversible one-variable A/B/A experiment, then statically check the
  original firmware's response path. Preserve negative errors; do not force success
  or shorten a timeout merely to make the UI responsive.
- A shared stand-in fix reaches a phone only when its ROM is rebuilt (`device.py build ID`): check
  the reply bytes in that phone's serial.log before concluding a fix "did not help" there.

Run the same probe on a firmware that works and one that does not (011.012 vs Anna, Anna vs Belle):
unanswered ISI lists, draw activity per second, composition commits (`GCEBE: commit` / layer lines),
deaths, kernel call numbering (`abi/swi-n95-vs-ours.txt`). The first difference that blocks something
is usually the cause. Record each finding in the device's bring-up notes
(`docs/devices/<id>/bring-up.md`) with [V]/[I] marks, and every workaround in `docs/hacks.md`.
