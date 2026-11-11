# N-Gage (EKA1) bring-up: question log

Start with [eka1-ngage.md](eka1-ngage.md) (state, running, debugging, next steps).

The method for the `ngage` board (roadmap P1 onward, `docs/eka1-ngage-roadmap.md`):
- Every stall or unknown becomes a numbered question here.
- Each question has:
  - **why**: the boot step it blocks, with a run and PC;
  - **evidence**;
  - **sources tried**: RE of the ROM, web, other projects' source (QEMU sx1/omap1, WindEmu,
    rengage, EKA2L1 as reference), datasheets, common knowledge;
  - **answer**, marked [V] (verified in the ROM code or a run) or [I] (inferred: a best
    guess with its reason).
- The board changes only from an answer, and the change names the question (`Qn`).

The phone in a window (like `sydb phone`): `python3 eka1/tools/ngage_phone.py [--scale 2]`. It starts
its own session (`sess-phone`), shows the screen and the N-Gage keys (buttons and host keyboard,
real down/up events), and stops the session when the window closes.

Crashes and anything that needs a look at a live or frozen machine: `eka1/tools/ngage_sess.py`
(a persistent session: `start` boots with catches on the fatal-fault routine and the abort
vectors; `wait` returns when one freezes the machine and prints registers, the current thread
and the named stack. Then `x`, `objects`, `stack`, `shot`, `mon`, `break`/`cont` work on the
frozen machine, and every catch writes `crash-N/` (SDRAM + state) for offline reads (`dumpx`,
`ngage_sess.dump_reader`)). It runs without per-exception logging, so it reaches the same
point about 3x faster than `ngage_run.py` with `-d int`.

Experiments: `eka1/tools/ngage_run.py --name runN [--secs S] [--break VA] [--watch VA[:SKIP]]`
(`--break`/`--watch`: gdb stops at the first hit and prints the registers and the stack). It boots ROM.bin on
`build/qemu-ngage/build/qemu-system-arm` (built by `scripts/build-qemu-ngage.sh`) and reports
the PC, exceptions and each unmodelled physical page touched. Logs are in `build/ngage/`.
Addresses are physical unless marked virtual. ROM offsets equal physical addresses below
0x02000000.

## Open

- **Q5. What does flash type 8/9/10 (Intel 0x8874/0x8876/0x8872) mean to the bootstrap?**
  It decides geometry and the bank table.
  - Why: chips 0 and 1 are identified per chip, and the result is stored in a table (0x5d44
    loop). Which device code matches the real RH-29, and what size does it imply?
  - Sources to try: web for Intel 0x8874 (StrataFlash Wireless family?); the bootstrap's use of
    the stored type (who reads the table filled at 0x5db8).
  - Current guess: 0x8874 for both chips, 32 MB each (chip stride is 0x02000000) [I].
- **Q6. What is the byte at 0x0C0047FD that the halt path sets (bit 3)?** Internal SRAM
  holds the copied ID-read stub at 0x0C004748-0x0C0047FC. The byte right after it could be a
  status or "boot failed" flag that a debugger or the next stage reads [I].
- **Q7. What are 0x08000000-0x08000014 and 0x0C150004?** They are written once at start
  (0xb24: 0x7037f800, 0xb2950489, 0x37, 0x20202112 x2; then 0x80 to 0x0C150004). Likely
  the external memory interface (chip-select timings) and a clock/watchdog control [I]. They
  are write-only so far: no answer needed until something reads them.

- **Q12. Why does the stored ROM digest not match the image?**
  - [V] The bootstrap (0x50003238, called from 0x50003568) MD5s each {virtual start, end,
    digest} entry at ROM 0x8f00/0x8f18 and halts at 0x50003230 on a mismatch. MD5 is confirmed
    by its init constants (0x50003aa0).
  - [V] Entry 0x8f18 covers 0x50009000-0x51164FFF. That is exactly up to the image's trailing
    erased gap (data ends at ROM offset 0x1165000). Entry 0x8f00 is all 0xFF (unused).
  - [V] The ROM's own MD5 code on the board computes the same digest as Python's MD5 of
    ROM.bin, but the stored digest differs.
  - Rejected: chip-1 part blank, either part alone, 16-bit byte swap, end exclusive, from 0,
    to rom_size, whole file.
  - The flash files are the official RH2941.0C1/.026 (2004-09-10). Most records carry the
    "+1" checksum anomaly, so it doesn't mark modified blocks.
  - [I] The flasher ("WD2 ALGORITHM", flash-file header) writes the real digest while
    programming, or hashes something else. Open: no source found.
  - Workaround **H-ngage-md5** in the board: at load time it writes the image's MD5 into its
    flash copy of the entry (the file on disk is unchanged, and a warning names it on every
    boot).
- **Q20. Can the NOS run? Its code is scrambled in the flash file.**
  - [V] ROM.bin 0x00C000-0x2AC000 (2.6 MB, the NOS code: nosstarter's entry 0x5149E4C4 maps
    to offset 0x9E4C4) has 7.95 bits/byte of entropy. The bootstrap, the NOS strings and data
    (0x2AC000-0x31B000) and the whole Symbian ROM are plain.
  - [I, strong] The flash file holds the MCU code scrambled (the DCT4 MCU-file scrambling),
    and the flasher descrambles while programming. That would also explain Q12: the MD5 is
    over the descrambled flash. A descrambled image whose MD5 matches the stored digest would
    verify both.
  - The only descrambler at hand is TriX `fmt_dct4crypt.c` (GPL; `eka1/references/`). The
    project rule allows GPL sources as references only, so nothing was implemented from it.
    **Decision for the owner**: see "Decisions" below.
  - Meanwhile **H-nos-none** (roadmap P3): a board code patch makes `nosstarter.ext` skip the
    NOS entry call.
- **Q23. The real product-data values** (items 14, 15, 5). The stand-in writes 0.
  `product_info` accepts item 14 when value/100 is 0, 1004 or 4400, and requires item 15
  <= 3 (it's an index).
- **Q29. What makes the MMC socket report "no card"?**
  - [V] `epbus.dll` binds three named interrupts (class: ekern export 492, bound by name
    through export 29): `IrqMMCCmdIdle` (event 1), `IrqMMCDataIdle` (event 2) and
    `IrqMMCCmdRdy` (event 4).
  - [V] By the interrupt map (Q30) these are controller sources 23, 21 and 24
    (`IrqMMCDataRdy` = 22). Event arming goes through DFCs (export 428 queues a DFC).
  - [V] The kernel creates the socket itself: ekern imports `epbus` exports 23, 31 and 50
    (`Kern::PBusController`). `medmmc.pdd`, `hotswapdrv.ldd` and `GD1DRV.ldd` (game cards)
    sit on it.
  - **Owner's choice:**
    - (a) a minimal MMC controller at 0x0C120600 (raise CmdIdle after a reset/command,
      answer "no response" to card commands), which is also the start of game-card
      support;
    - (b) a declared kernel patch so the socket reports no media.
- **Q37. The LCD interface at 0x0C121200.**
  - [V] `lcd.ldd` accessors: +0x200 control/configuration, +0x204 control (2, then 1) and
    status (bit 2 = ready; 0x50401bd4 waits for it before each data word), +0x208 data.
  - [V] The bootstrap uses the same block (pc 0x6658/0x66e0): it writes control words
    0x49200000/0x09200000/0x09280000 around each data word, sends **0x11** (DCS "sleep
    out"), then **0x04** (read display ID; reads +0x208 back), with 0x07840003 as a read
    set-up.
  - [I] A serial command interface to a DCS-style panel controller.
  - Board: the status register reads with bit 2 set. Without it `lcd.ldd` spun forever
    (649k exceptions in 100 s), and Starter, SharedDataServer, DosServer and two AppRun
    processes now start (run97).
  - Next (P4): decode the +0x200/+0x208 command stream into a 176×208 12 bpp display.

## Answered

- **Q56. Where does the host time go?**
  - [V] QEMU runs at 100% of one core: EKA1's null thread never sleeps (it calls the NOS
    idle hook in a loop), so CPU use alone says nothing.
  - [V] perf (`-e cpu-clock`; hardware events and call graphs are not permitted here)
    shows almost no time in translated code. It goes to TB lookups (`tb_tc_cmp`),
    `cpu_unwind_data_from_tb`, longjmp clean-up and big-QEMU-lock churn. Two causes:
    - logging every interrupt-controller, timer, CBUS and NOS access with `ngage_pc()`
      (an unwind each): 28 MB of log a minute;
    - QEMU 11 recompiles a block (`cpu_io_recompile`) for each device access that is not
      the block's last instruction. The NOS stub writes ten device registers per call.
  - Board changes:
    - hot-path logging limited to the first 32 accesses per register (`NGAGE_VERBOSE`
      for all);
    - the stub answers the idle hook from RAM; the board sets `RX_FLAG` in the stub's
      bss when an ISI delivery waits.
  - [V] Boot time is set by the guest's work (C: format, factory restore; Q55), not by
    these costs. A PC sample during the C: format: ekern 74%, the stub 25% (medint's
    slot 6 calls), medint 1%.

- **Q55. Why does every boot take 155 s?**
  - [V] The trace (ngage_sess logging catches on RProcess::Create) shows two costs:
    - EStart formats C: (32 s);
    - Starter runs RRfsClient::PerformRfs, the first-boot "restore factory settings"
      (43-68 s).
    Both happen because C: was blank on every boot.
  - Board: `-M ngage,flash=FILE` saves the changed 64 KB flash blocks at exit and applies
    them at start. The second boot reaches the UI at ~90 s.
  - [V] The ~40 s pause left on later boots was Starter in PerformRfs again: RfsServer runs
    RFormat::Next, because NOS item 12 = 105 tells Starter "format the user area". With a
    kept flash, item 12 is now 100 (normal start).
  - [V] Result: about 7 s to the home screen with a kept flash. A factory-fresh boot shows
    the logo at ~38 s (the C: format) and the UI at ~93 s.
  - Open: the time query shows on every boot. The RTC is never set: the stand-in hands
    rtc.dll its own unset time (Q49).

- **Q54. How fast are medint's flash writes?**
  - [V] medint.pdd programs a halfword per timer-3 FIQ, with a period of 0x1FE ticks.
  - [I] At 34.67 MHz that is 14.7 us, an Intel word-program time. At /15 (the old guess)
    the DBMS commits outlasted ViewSrv's timeout. Timer 3 now runs at 34.67 MHz.
- **Q53. How do key presses reach Symbian?**
  - [V, static RE of ekeyb.dll by a research agent; checked live in q83] ekeyb scans a 6x5
    matrix itself:
    - columns at 0x0C150400 (+00 input, active low; +02 drive latch);
    - rows at 0x0C15040C (+0x10 = row interrupt enable);
    - CTSI status 0x0C150100 bit 2, interrupt source 25;
    - scan codes from layout 3 (0x5041c4b0, row*5+col);
    - Kern::AddEvent from its DFC.
    The power key is the UEM's (CBUS dev 1 reg 0x1F bit 1) and isn't modelled. The board
    models the matrix (`ngage_keys.c`) and maps host keys to positions.
  - [V] q83: the left softkey answers the first-boot time and date queries, and the phone
    reaches the idle screen (status pane, N-GAGE wallpaper).
  - [V] Resolved, q86-q91. The ViewSrv 11 kills came from synchronous waits inside Phone.app:
    1. profileengine's DBMS commit to C: (flash writes): fixed by timer 3 at full speed
       (Q54);
    2. ReadSimAlsLine: fixed by the 23-byte `09 32` reply.
    The menu app then starts. It blocks in GetSimServiceTable (SIM 09 13 {11}, open).
- **Q52. What does Startup wait for in a normal start?**
  - [V] A research agent's static analysis, checked live in q72 with breakpoints:
    - DOS event 1 = 3 (the self-test passed).
    - Startup's SetState goes 1 -> 2 -> 4.
    - State 4 waits for SharedData `state.val` (0x10005943) == 213. Phone.app sets that
      (0x50ca0a84) once its engine is up. Phone never gets there.
    - The next gate, state 5: sae's SIM keys, System Agent 0x100052E9 == 7 and
      0x100052F0 == 0 [I meaning].
  - [V] q73: Phone.app is blocked in construction: RPhCltServer::Connect, a synchronous
    CreateSession to PhoneServer. PhoneServer is blocked in a synchronous ETel custom call
    (GSMCUS RCtmGsmPhone::GetHandsfreeMode, IPC 0x2733). phonetsy holds that call without
    sending ISI (handler 0x5092d24c). Open.
  - [V] An unsolicited MTC_STATE_INFO_IND to phonetsy's object makes phonet.dll panic
    ("ECS panic" -5), so phonetsy gets indications only after it subscribes.
  - [V] Resolved, q75. 0B 11 {06 01} is phonetsy's own GetHandsfreeMode request. Its
    response parser (0x5093d2cc) needs subtype 06, exactly 1 sub-block, and a u16 at
    sb+4 (0x000D = handsfree). With `12 06 01 06 08 00...`:
    - the IPC completes;
    - Phone.app constructs and calls SetState(213);
    - Startup goes to state 5 (it waits there for SIM state 7 and security 0 from sae).
  - [V] q79: SIM_IND (09 EF {05 08 08}) to the DSY's object 0x4E on unit 9 runs the DSY's
    handler 0x50ff9cbc, and sae publishes SIM status 0x100052E9 = 7. The same indication on
    unit 7 (ETel's channel) or units 8/10 has no effect.
  - Answered: phonetsy's 09 60 {FA 00} -> 61 {FA 31} (parser 0x5093e5b4); the DSY's 09 3B
    {FA} -> 3C {FA 2A}.
  - [V] Resolved, q81/q82, with three replies from the agent's static analysis:
    - an unsolicited PN_SECURITY 08 10 {02 01} to the DSY (object 0x4E) publishes security 0;
    - phonetsy's emergency-number read gets 09 3E {FA 01, 40 bytes: "112", "911"};
    - phonetsy's service-name read gets 09 22 {2C 01, 36 zero bytes}.
    Startup then goes 5 -> 6 (animation/welcome) -> 8 (SetState 212). At ~154 s the screen
    shows the S60 UI: the N-GAGE background with the first-boot "Time:" query (12:00 AM, OK).
- **Q51. Why does Starter run the short list (SelfTest ... Startup, CalenSvr) without Phone?**
  - [V] Starter's selection records (resource 2) map (WD2 reason, SW reason) to a list:
    | Keys | List |
    |---|---|
    | (0, 100) | 3, the full normal list |
    | (4, 100) | 6 |
    | (1, 100) | 7 |
    | (0, 201) | 8 |
    | (0, 202) | 9 |
    | (5, 100) | 4, test |
    | (7, 100) | 5, SysAp + Startup |
    | (0, 210) | 10 |
    | (0, 250) | 11 |
  - [V] Starter selects once. If no record matches, it panics "StarterServer" -1, except
    that an unknown SW reason falls back to 100 (q66: (0, 105), then (0, 100)).
  - [V] The WD2 reason is the variant's start-up mode (helper.ldd DoControl 0; 6 becomes 5).
    ecust's getter 0x503439ac works in two ways:
    - cold (0x0C150000 & 0x0E == 0): only 4 (UEM reg 0x15 bit 4) or 9;
    - reset (bits set): the code byte at 0x0C00428E / 0x420E / 0x420A / 0x4206, mapped by
      0x50343880 (1 -> 0, 2 -> 4, 3 -> 1, 4 -> 5, 5 -> 6, 8 -> 7, other -> 9).
    So a normal start (mode 0) needs a reset source plus code 1. Only ecust reads these
    bytes, and no plain code writes them.
  - [I] The NOS (scrambled) or the hardware provides them on a real power-on.
  - Board: H-ngage-startup sets them once the stand-in has run (code 1, `NGAGE_STARTCODE`).
  - [V] q66: keys (0, 105), then (0, 100), and 32 processes, including Phone, Watcher,
    ScreenSaver, Autolock and Startup. Startup then waits (open: what for), and the screen
    stays on the NOKIA logo.
  - Mode 9 (UEM bit 3) gives (9, 100), which has no record: Starter panics and the kernel
    faults (PLAT 91, a critical thread panicked).
- **Q50. How do NOSDRIVER channels carry ISI, and how does the NOS answer?**
  - [V] nosdriver.ldd passes calls straight through to the NOS channel object (+0x38):
    DoRequest(reqNo, a1, a2) goes to +0x10, DoControl(fn, a1, a2) to +0x14, DoCancel to +0x0C.
  - [V] A send is DoControl(4, a1), with a1 an 8-bit descriptor holding a PhoNet frame:
    `00 00 00 RES LEN_lo LEN_hi ROBJ SOBJ TID MSG DATA...`. The length is little-endian
    and counts from ROBJ.
  - [V] A receive is DoRequest(0, a1 = a 500-byte buffer, a2 = a TBuf8<2>).
  - [V] Completing is DThread::Write(a1, frame) (export 479), then
    DLogicalChannel::Complete(0, result) (export 64: client thread at channel+0x14, statuses
    at +0x18+4n).
  - [V] Clients accept a reply that mirrors the request: ROBJ = the request's SOBJ,
    SOBJ = 0, the same TID, the response id. Example: the DSY self-test (35 02 -> 03,
    35 00 -> 01, 59 01 -> 02) then advances exactly as on the N95.
  - Board: `ngage_isi.c` plus the stub's delivery DFC; replies in
    `src/qemu-ngage/isi-replies.txt` (BB5 format, plus `echo` and `ind`).
  - [V] phonetsy panics (USER 21 in isimsg.dll) on replies shorter than it expects: GPDS
    31 1A `00 00` and audio 0B 12 `00 01`. Those two stay unanswered.
- **Q49. Why does DosServer die in rtc.dll at ~85 s?**
  - [V] Session `q50`: rtc.dll 0x5034a944 calls method +0x48 through `[rtc+0x120]`,
    which is NULL. Only rtc request 1 (vtable entry 6, 0x50349394 -> 0x50349088) fills
    +0x120, with `Variant::Get(3)`. No Symbian code calls entry 6 (no pointer or
    branch to it in the ROM; it is never hit in a boot), while the alarm paths test the
    variant's NOS status bit 0 and then use +0x120.
  - [I] The NOS makes request 1 when its RTC service comes up (it takes three 64-bit
    times by pointer).
  - The variant holds the RTC object at +8, but only after nosstarter ran (q53: 0 at
    the stub's entry).
  - Stand-in change: on the first NOS call after the variant has the RTC object, the
    stub makes request 1 with the RTC's own time for all three times [I].
    It returns 0 (state 9, not 2), but +0x120 is set. `q55`: no fault in 270 s. 24
    processes, including EikSrvs, SysAp, Startup, PhoneServer and LogServ. The screen
    stays on the NOKIA logo.
- **Q48. Why does the kernel jump to 0x00800080 after the logo?**
  - [V] Session `q49` (frozen on the prefetch abort): EKern::Supervisor makes a virtual
    call at 0x5039a580 through an object whose vtable is nosdriver.ldd's (0x511643d4,
    flash chip 1). The ROM file holds the right pointers there, but the machine read
    0x00800080: the flash status word. medint.pdd was programming C: on the same chip,
    and QEMU's CFI01 model switches the whole chip out of read-array mode.
  - [I] The part (Intel 0x8874, StrataFlash Wireless L18, 256 Mbit) reads while it
    writes, in 16 Mbit partitions.
  - Board change: `patches/qemu-ngage/0001-pflash-cfi01-rww-partitions.patch` (ngage tree
    only): reads outside the partition of the last command return array data;
    both chips use 2 MB partitions.
- **Q47. Why are frames white once SplashScreen runs?**
  - [V] `q46t`: after the normal list starts, lcd.ldd (lr 0x50406564) sends its
    own panel set-up (11, 3a, 36, 29, 38, 13), then 2a/2b/2c. The board counted
    that RAMWR as "the OS drives the panel" and stopped showing the screen chunk
    (H-ngage-fb). No pixel words follow from the CPU, so the panel keeps the
    bootstrap's white fill.
  - [I] lcd.ldd moves the pixels by DMA (not modelled yet).
  - Board change: H-ngage-fb stays on until lcd.ldd writes a whole frame of pixel
    words itself.
  - [V] `q47`: with that change the NOKIA logo shows from ~50 s on a normal boot
    (SplashScreen through the window server; no diagnostic).
- **Q46. Why does the flash format stall after the first block program?**
  - [V] medint.pdd times the program/erase with timer 3. It writes the count (+0x10)
    without writing a mode, so our timer never armed. Its completion interrupt is
    taken on the FIQ side (bank 3), which mirrors pending sources 0-31 of bank 0.
  - Board changes: a non-zero count write arms the timer as one-shot. Bank 3 is
    modelled as the FIQ mirror of bank 0.
- **Q45. Which variant slots must exist for the internal-flash media driver?**
  - [V] medint.pdd calls the variant's `Get(6)` and `Get(7)` (vtable +0x100) and
    uses the results as objects without a NULL check. The format path faulted there.
    The NOS normally registers these slots.
  - Board change: nos_standin.S registers trap objects for slots 6 and 7, next to
    slot 3 (H-nos-standin).
- **Q44. Does NOS item 12 = 105 format blank internal flash instead of RAM fallback?**
  - [V] EStart 0x503e9bce compares item 12 to 105 after its initial C: mount
    fails. Equality jumps to 0x503e9c9e, calling 0x503e98a0; otherwise it removes
    the flash filesystem/driver and mounts another filesystem before that call.
  - [V] 0x503e98a0 opens RFormat and loops over RFormat::Next. In `q44_format`,
    item 105 enters this path after mount returns -20. After 125 seconds, only
    EKern/EFile/EStart exist; the CPU is in the fatal loop at 0x5031e394.
    Formatting has not reached Starter. Stack evidence
    is saved in `build/ngage/q44_format.stacks` (includes stale frames).
  - [V] Resolved by Q45 and Q46. The fault was medint.pdd's NULL `Get(6)`, and then
    the stall after the first block program. Item 12 = 105 is now the board's
    default (H-nos-standin). C: formats (39,052 flash writes), mounts as type 6, and
    Starter runs the normal list (SplashScreen, SelfTestServer, EInfoServer, randsvr,
    RfsServer, BTTestServer; 13 processes, commit 66e5aca).
- **Q43. Would accepting RAM C: get past the recovery list?**
  - [I] Diagnostic only: change the RFs::Drive result's media-type word to 6
    at 0x509610ea through a runner logging breakpoint. This does not implement
    flash; it isolates the consequence of Q42. Trace list selection and process
    creation, and keep all debugger artifacts outside git.

  - [V] `q43_accept_ram` selects (4,100), launches additional processes, and
    renders the blue NOKIA logo (`build/ngage/q43_live.png`). The machine later
    reaches the fatal loop again; this is a diagnostic success, not a complete boot.
  - Obsolete since C: formats (Q44-Q46).
- **Q42. Why does Starter reject C: and force startup keys (0, 210)?**
  - [V] `q39_keys`: startup reason is initially 4; list selection uses (0, 210),
    resource ID 10. ROM 0x509610cc calls `RFs::Drive(TDriveInfo&, 2)` (C:)
    through import 0x503ded80 -> 0x503d9414. It accepts only a successful call
    with first TDriveInfo word == 6. Any other result triggers 0x509612ea,
    overwriting the startup keys with (0, 210).
  - [V] `q42_drive`: RFs::Drive succeeds (r0=0), but returns words
    `{5, 1, 0x11, 5}`: C: is RAM, not the required type 6 (flash).
    The DOS startup reasons are initially `{4, 0}`; Starter normalizes SW to 100.
  - [V] EStart takes the RAM fallback after its flash mount fails: `q44_format`
    captures -20 at 0x503e9b4a. The short list is selected by the disk check,
    so its selection does not establish a charging/alarm startup reason.
- **Q41. Does an unanswered NOS/ISI request determine Starter's startup keys?**
  - [I] The handoff proposes a cellular startup reason or SharedData publication.
    Trace the writers of Starter +0x9c/+0xa0 before changing ISI responses.
  - [V] Resolved by Q51: the keys come from the start-up mode (WD2 reason) and the SW reason, not from ISI.

- **Q40. Who should start Eikon before Starter opens BackupServer?**
  - [V, prior handoff] Starter's backup-wrapper creation leaves with -1; cleanup
    dereferences a NULL array. EikSrvs is absent; EikSrvc can launch it on demand.
  - [I] Starting the normal resource list may launch the missing server indirectly.
    Verify the actual list keys before adding a server-launch workaround.
  - [V] Resolved by Q51: the normal start-up list starts EikSrvs.
- **Q39. Which inputs select Starter's startup resource list?**
  - [V] Completed `exp12_1` and `exp12_2`: NOS item 12 values 1/2 produce
    `auxstart 1`/`auxstart 2`, respectively. Both have the same six
    `RProcess::Create` caller addresses and end at PC 0x5031e394 (fatal loop).
    Their descriptor-print commands were invalid, so these traces alone do not
    identify the two AppRun arguments. Neither reaches a visible splash.
  - [V] Starter 0x50960e0c scans resource records containing two uint16 keys
    and an int32 resource ID. Construction at 0x509603dc supplies fields +0x9c
    and +0xa0 as these keys; 0x509608ac also selects a list through this helper.
  - [V] These are WD2 and software startup reasons, obtained through SysStartup
    -> RDosHelper (sysutil 0x5096f21c). Q42 identifies the subsequent disk override.
- **Q36. What does the NOSDRIVER channel ask for?** (run93)
  - Starter or EStart opens a NOSDRIVER channel (unit 13). The stand-in constructs the NOS
    object; `nos_channel_open(object=Media.Lffs PDD?, unit 13)` is called.
  - Then one DoRequest (request 0, status pointer 0x005020FC, arguments in user memory),
    and the channel is deleted (vtable +0x08 with 3).
  - run97: with timeouts working, EStart cancels the request (DoCancel, -3 KErrCancel)
    and closes the channel. Then the boot goes on.
  - [V] NOS-side strings name the other end: `ENOS/ECS/NOS_DRIVER/SRC/Decsimpphonet.cpp`
    and `ENOS/CMT_EXT/EKIF/INTERFACE/E2n_channel.cpp` (`ekif_msg_header_ptr`). So
    NOSDRIVER channels are the PhoNet (ISI) transport, "EKIF" EPOC↔NOS channels.
  - The user-side clients that name NOSDRIVER: `phonet.dll`, `phonetsy.tsy`,
    `dataport.csy`, `SoundDevice.mda`, `vcsvr.exe`, `emon.exe`, `Nokia.dsy`, `EStart.exe`,
    the test servers.
  - Next: the channel requests once `phonet.dll` opens its channels. That's where the ISI
    stand-in answers.
  - [V] Answered by Q50.
- **Q32. What does EStart wait for on the NOSDRIVER channel?** (run82-83)
  - **Answered by Q33/Q34:** EStart was waiting on the HelperDrv channel (`helper.ldd`,
    device at channel+52), not NOSDRIVER. Its request 1 = an asynchronous NOS read of
    item 12.
  - [V] After the timer fix (Q31), EFile mounts its drives and starts EStart. EStart loads
    the LDDs `HelperDrv` and `NOSDRIVER` and opens one channel.
  - Its main thread's stack shows `RBusLogicalChannel::DoRequest` and `nosdriver.ldd`
    frames; then everything idles.
  - [I] It waits for the NOS's answer on that channel: the start of the ISI stand-in (roadmap
    P3).
  - Next: the channel's request numbers and what `nosdriver.ldd` expects from the NOS side
    (the stand-in device).
  - [V] Answered by Q50 (the NOSDRIVER transport).
- **Q30. Interrupt map.**
  - [V] ekern holds the interrupt name table (102 pointers at 0x5031B930-0x5031DAA8, names
    from 0x50340160).
  - [V] IRQ controller source = name index - 9. Checked three ways: `IrqTimer0` = 0 (the
    timer); `IrqCTSI_IRQ` = 25 (`cbus.dll`/`ekeyb.dll` enable bank 0 bit 25);
    `IrqGPIO8High` = 46 (`cbus.dll` enables bank 1 bit 14).
  - The IRQ sources:

    | sources | names |
    |---|---|
    | 0-5 | Timer0-5 |
    | 6-13 | DmaChannel0-7 |
    | 14 | FlashCopyRdy |
    | 15 | LCDBuffer |
    | 16-20 | Ir4M |
    | 21-24 | MMC DataIdle/DataRdy/CmdIdle/CmdRdy |
    | 25/26 | CTSI_IRQ/FIQ |
    | 27/28 | APIIOAbort/MissAlign |
    | 29-82 | GPIO0-26 Low/High |
    | 83 | STIAbort |
    | 84-86 | CCP |
    | 87 | DIFFIFO |
    | 88-90 | PIPO |
    | 91 | WBCDMAStatus |
    | 92 | RTCWBStatus |

  - Indexes 0-8 (DMA0-6, Timer0/1) are a FIQ group [I].
  - Interrupt ids in the ASSP API (ekern 0x5031c7f4): above 92 they go to the controller as
    `id - 93`, ids 0-92 take another path (0x5031e998).

- **Q27. Internal-flash media geometry.** [V] `medint.pdd` `DoCreate` (0x50381d2c)
  reads the flash type from the variant (+0x10C).
  - Types 10/6/19/11: one layout ({0x400000, 0x2000000, 64 blocks}).
  - Other types (our 0x8874 = type 8): 2 MB at 0x02600000, 32 blocks of 64 KB.
  - Seen on the board: block unlocks of 64 KB from chip-1 offset 0x400000, then 8 KB blocks
    0x5F0000-0x5FE000 (possibly `ecust`'s, not `medint`'s).
  - So the device ID (Q5) decides where C: lives. Still open: the real RH-29 chip IDs.
- **Q26. What is the user side waiting for?** run46: 21,735 user-mode exceptions (SVCs:
  `User::Fold` 0x4F, `WaitForAnyRequest` 0x4D, heap, `TTrap`, `LockedInc/Dec` and others),
  then the machine idles in the null thread.
  - `ngage_run.py --objects` (run51) lists the containers: processes EKern and EFile;
    threads Supervisor, NULL, Main (the file server), LoaderThread and StartupThread; PDDs
    Media.Lffs, Media.IRam and Media.MmcF; servers FileServer and Loader. No EStart and no
    system servers yet.
  - **Narrowed (run55-run77): the boot waits on media unit 0, the MMC socket (Q29).** The
    chain, each step [V]:
    1. StartupThread is in `RFs::AddFileSystem` (EFSRV, Thumb), with KRequestPending at
       0x80D01AF4. Its thread object points at it (+0xE8; saved user SP at +0x158).
    2. The FileServer (Main) took the message: `DServer` +28 receive status = 0, while the
       Loader's is set. Main's live SP (0x80601330) sits in `TBusLocalDrive::Caps` ←
       `ELocal.fsy`, waiting on a kernel-server request: SVC 0xC00076, function 0x3A,
       arguments {version 0x10000, &status, drive 0}.
    3. The Supervisor (kernel server) is back in its receive loop (0x5033a8dc), so the
       request waits on something asynchronous.
    4. `medint.pdd` `DoCreate(unit)` accepts only unit 1 (else -5 KErrNotSupported, its
       normal answer). It was offered unit 1, then unit 0. So unit 0 belongs to another
       driver: the MMC socket.
    5. `epbus.dll`'s socket writes MMC controller 0x0C120604 = 0x40. Its state machine
       (+0xDCC) then waits (0x503f5c60) for event bit 1 in +0xDA8 (`IrqMMCCmdIdle`, Q29),
       which the board never produces.
  - Ruled out: kernel timers and DFCs (the tick counter advances 24/s and the DFC queue is
    empty); `medint`'s -5 leave (normal); removing `MEDMMC` (the hang stays).
- **Q25. What is the null thread's slot-3 item-13 call?** [V] ekern 0x5032bb54: on every
  pass of the idle loop it calls slot 3 method 18 (item 13, no buffer), then checks two
  globals and idles. [I] The NOS gets CPU time or a sleep hook from Symbian's idle thread.
  The stand-in returns 0.
- **Q21. Who provides variant slot 3 (`product_info.dll` faults without it)?**
  - [V] `ecust`'s variant object (`Kern::Variant()`, ekern export 469 [I name], the global
    at 0x8000038C) keeps 9 interface slots at +28. Vtable +0xFC is `Register(n, p)`
    (fills an empty slot only), and +0x100 is `Get(n)`.
  - [V] Registrants in plain ARM calls: slots 0 and 5 `ecust`, 1 `rtc`, 2 `ekeyb`, 4 `lcd`,
    8 `product_info`. No module registers slot 3 this way.
  - [V] `product_info.dll` calls slot 3's method +0x48 (item 14, 4 bytes), divides by 100 and
    compares with 1004 and others. It faults "Product_info.DLL" when slot 3 is NULL.
  - [I] Slot 3 is the product-data/PMM interface that the NOS registers when it starts.
  - **Answered** by H-nos-standin (2026-11-10): the stand-in registers a trap object in
    slot 3. Seen calls, all method 18 = `Read(item r1, r2, out r3, sp[0], length sp[1],
    sp[2])`:
    - `product_info` reads items 14 and 15 (4 bytes);
    - `ecust` reads item 5 into variant+0x94 (4 bytes);
    - the null thread calls item 13 with no buffer on every pass (Q25).
  - [V] The NOS provides it; the stand-in registers it (H-nos-standin).
- **Q33. The Symbian ↔ NOS interface.**
  - [V] `nosdriver.ldd` (0x4E0 bytes) is a thin shim. It imports exactly two NOS functions
    from the NOS's export table (0x51163EF8):
    - 0x514A260C: constructs an 8-byte channel object, vtable at +4 (+0x08 delete,
      +0x0C DoCancel, +0x10 DoRequest, +0x14 DoControl);
    - 0x514A1C90: called from DoCreate with the unit.
  - No other module calls NOS code.
  - `helper.ldd` (HelperDrv, used by EStart) talks to the NOS through the variant's
    slots instead: slot 3 (NOS data, method 18 `Read`), slots 1/2/5 (rtc, ekeyb, ecust),
    and the NOS status word.
  - Its channel vtable 0x5037F91C: DoCancel 0x5037F028, DoRequest 0x5037F0AC, DoControl
    0x5037F644 (13 functions).
  - EStart made DoControl 0 (returns the variant's start-up mode, 6 mapped to 5), then
    DoRequest 1.
  - Board: both NOS exports jump into the stand-in stub. The channel object is trap object
    65 (logged `ngage-nos: slot 65 ...`).
- **Q34. Asynchronous NOS reads.**
  - [V] Slot 3 method 18 `Read(item, r2, out, …, length)`: with r2 = 0 it's synchronous.
    With r2 = a TDfc (callback at +8, argument at +12) the NOS completes later: it writes
    the item and queues the DFC (`Kern::Add(TDfc&)`, ekern export 7).
  - `helper.ldd` request 1 reads item 12 (2 bytes, DFC at channel+0x44). Its callback
    completes EStart's request with the 16-bit value.
  - `ecust` reads item 5 asynchronously at start-up (DFC at variant+0xB4). Its callback
    0x50344874 resets the phone (variant +0xD8) unless the start-up mode (variant +0xD0)
    is 1 or 2. [I] So item 5 is a notification the NOS sends later, e.g. a shutdown or
    reset request.
  - Board: the device returns the DFC in +0x2C and the stub queues it. Item 12 completes
    with 0 [I value]; item 5 stays pending. With that, EStart starts EwSrv, FbServ and
    Starter (run93).
- **Q35. The start-up mode and the UEM ADC.**
  - [V] variant +0xD0 caches the mode at +0x14 (8 = undecided).
  - It's decided from halfword 0x0C150000 bits 0x0E (method +0x16C) or else method +0x170,
    which asks `cbus.dll` (variant+4, method +0x20 = its field +0xA0).
  - `cbus.dll` sets that field to 1 when an ADC reading (dev 1 reg 0x19 start, reg 0x1A
    result) is within 380..590.
  - With 1, the mode comes from the RTC object (1/2), or 4 if dev 1 reg 0x15 has bit 4, else
    9. Mode 9 with the reading at 0 made `ecust`'s item-5 DFC reset the phone.
  - Board: reg 0x1A = 480 [I] (the bootstrap's own ADC limits: power off above 0x3DD, skip
    its delay for 5-93). Open: which channel and real value (battery voltage? battery size
    indicator?), and whether the power-key reason should set reg 0x15 bit 4 (mode 4).

- **Q31. Kernel timeouts never fired.**
  - [V] The kernel's one-shot timer queue (delta queue, head 0x80006628; insert via export
    428, expiry handler 0x50328d00) runs on hardware timer 1 (0x0C120A20, source 1;
    object 0x80000558).
  - The board modelled only timer 0, so every kernel timeout hung. The MMC socket's power-up
    timeout (2,079,960 ticks) left the FileServer's mount of drive 0 waiting forever (Q26,
    Q29).
  - Timer 1's methods [V]: Start = mode 1, count 0x7FFFFFFF, status 0, enable;
    ReadCount = +10 (live); AddDelta = +10 := count + delta (minimum 256); ISR = +08 := 1,
    then the queue handler.
  - Clocks: timer 0 runs at 34.67 MHz [V]: its period 0x843E2 is the kernel's 15625 us tick
    (0x3D09 in export 466). That corrects the earlier 13 MHz guess (Q13/Q17). Timer 1 runs at
    34.67 MHz / 15 [I]: the ms-to-ticks factor is 2311, and a ×15 appears next to it.
  - Board: six timers (0x0C120A00 + 0x20·i, sources 0-5). With them the MMC socket times
    out as "no card", EFile mounts, and EStart starts. The MMC controller model (choice (a)
    in Q29) wasn't needed for this.

- **Q28. The PPM flash file (RH2941.026).**
  - [V] It programs the PPM tree at flash 0x02330000 (0xD0000 bytes, virtual 0x51230000) and
    a data image at 0x00F00000 (0x72000 bytes, virtual 0x51300000). It also rewrites the boot
    block: ROM header version/time/checksum (0x84-0x8B, 0xA8), 8 bytes at 0x180, and the MD5
    entry at 0x8F00 (0x51230000-0x51371FFF).
  - [V] That entry's digest matches the MD5 of tree + data. So the bootstrap check is plain
    MD5 over flash, and Q12's mismatch on the MCU entry must come from MCU bytes that differ
    from flash, i.e. the scrambled NOS (Q20).
  - Board: `-M ngage,ppm=eka1/extracted/RH2941.026` (the runner's default) loads the
    segments: the boot block before the patches, the rest into the chips.

- **Q22. The power model.** [V] The kernel init (0x5032b924) calls the power model at
  0x80006BC4 (vtable +0x0C) and faults when it's NULL. It's set by `Power::SetPowerModel`
  = ekern export 394 (0x50336A08; faults 34 if set twice). No ROM module imports it, so the
  NOS provides it.
  - `AddPowerHandler` (export 5) enqueues into the model's queue at +16. That's why the
    stand-in builds its object with `DPowerModel`'s constructor (export 493), in the NOS's
    zeroed bss (0x8000C378+0x100), before setting its own vtable.
  - Calls seen: methods 3 and 11.
- **Q24. The variant's NOS status word.** [V] variant +0x90 (getter vtable +0x12C, setter
  +0x128; no Symbian module calls the setter).
  - `ecust` waits for bit 1, then faults with 12 unless bit 0 is set.
  - `rtc.dll` and `medint.pdd` test bits 0 and 1.
  - The stand-in sets 3.

- **Q1. Can the bootstrap be entered cold at the reset vector?**
  - [V] Yes. 0x0 is `b 0x32c`; 0x32c checks `ip == ~fp` (handoff flags from a boot loader)
    and clears the flags otherwise.
  - No internal boot ROM is needed (rengage loaded 8 KB at 0; this ROM does not rely on it).
- **Q2. What sits at physical 0x02000000?**
  - [V] A second CFI flash chip, 16-bit. Probe at 0x97c: reset 0xFF, 0x90 to 0xAAAA,
    manufacturer at +0, device at +2.
  - Accepted IDs: Intel 0x89/0x8874, then Samsung 0xEC/0x22F2/0x22FA, AMD 0x01/0x277E and
    ST 0x20/0xA4 with 0xAA/0x55 unlocks. The result picks the bank table at 0x638 or 0x744.
  - Board: QEMU `cfi.pflash01`, needs `device-width=2` (the legacy ID mode packs both bytes
    into one word: run1 looped in the AMD retry).
- **Q3. Where is SDRAM, and how big?**
  - [V] At 0x0A000000. The bootstrap pattern-tests 0x0A000000.. (0xFFFFFFFF/0xAAAAAAAA/
    0x5555 per word), sizes by aliasing writes at 0x0A000000 + 2^n up to +8 MB, and keeps its
    stack at 0x0AFFF3xx (top of a 16 MB bank).
  - [I] 16 MB (16 MB is also the RH-29 spec).
- **Q4. Why does run3 halt at 0x5dac?**
  - [V] On purpose. The flash-chip loop (0x5d44) identifies chips 0..2 at `n << 25`: first
    an Intel ID read (0x599c, stub copied to internal SRAM because it queries the chip the
    code runs from), then the AMD/ST/Samsung ID read (0x5a44).
  - If chip 0 or 1 is unknown (code 20), it sets bit 3 of byte 0x0C0047FD and spins.
  - Chip 0 is the boot flash at 0, so the ROM must answer an ID query: it becomes an Intel
    CFI01 device holding ROM.bin.
  - Accepted Intel devices: 0x8874 -> 8, 0x8876 -> 9, 0x8872 -> 10. The AMD branch takes
    0x01 with 0x227E plus extended IDs 0x2200/0x2400/0x2600/0x2700, 0x7E/0x16, 0x22D1. ST
    takes 0xA4/0xA5, and Samsung 0x22C0+0x30..0x3A.
- **Q8. Is 0x0C004000 internal SRAM?**
  - [V] Code is copied to 0x0C004748.. and executed there (run2/run3 show instruction-fetch
    reads served by the catch-all).
  - Size and base still [I]: the bootstrap's MMIO list names 0x0C004000. Model it as RAM once
    its extent is known (Q9).

- **Q9. 0x0C120004 / 0x0C120020 (clock block).**
  - [V] The bootstrap writes 0x0C to +04 and waits for bit 26 of +20 (0x3784). It writes 3 and
    waits for bit 27 (0x382c). Between the two it sets bits in the EMIF register 0x08000004.
  - [I] PLL enable and lock bits. The board forces bits 26/27 on in reads (`ngage_known`).
  - Sources tried: web (no UPP register map public), nokia-recomp (DCT4 recompiler, no
    MMIO map yet), rengage (none).
- **Q10. 0x0C149000.** [V] Written 0x2210; the bootstrap waits for bit 0 (0x37fc). [I] A
  ready/ack bit; forced on in reads.
- **Q11. The serial bus at 0x0C150420 is CBus.**
  - [V] The register interface, from the bootstrap (0x35ac/0x3634) and `cbus.dll` (driver,
    "CBus" fault category), which reaches it through ekern's 16-bit accessor (0x5031e708,
    `0x58000000 + offset`):
    - +0x20: transfer format (0x0F = 16-bit, 0x07 = 8-bit);
    - +0x22: device << 5 | register, 0x100 = read, done = bit 10, polled up to 0x800 times;
    - +0x24/+0x26: data high/low;
    - a write commits on the +0x26 store.
  - [V] Device 1 is the power ASIC (power-on reason, power-off). Device 4 has an ASIC id at
    register 0.
  - The 3-bit device / 5-bit register split is the Nokia CBus format (Linux `drivers/cbus`,
    Retu/Tahvo), from common knowledge.
  - Open: what the 0x200 flag in +0x22 and the high bits of +0x20 mean.
- **Q13. The bootstrap timer at 0x0C120A00 and its interrupt.**
  - [V] When the ADC check (dev 1 reg 0x19 = 0x6880 start, reg 0x1A result, 10 bits) is not
    within 5..93, `0x500031a0` writes +04 = 2 (run), then period +18 = +10 = 0x016B28C0.
    It clears +08, writes 0x0C120108 = 1 and unmasks IRQs.
  - [V] After the MD5 check it waits for +08 != 0. The IRQ vector (0x418) calls the handler
    0x3030 for any IRQ in this phase. The handler reads dev 1 reg 0x1F, sets +08 = 1, stops
    the timer and writes 0x0C12010C = 1.
  - Board: one-shot timer armed by the period write, IRQ = pending && 0x0C120108 bit 0,
    cleared by writing 1 to 0x0C12010C. [I] 13 MHz tick (GSM reference clock): 1.83 s.
- **Q14. The power-on reason.**
  - [V] `0x50002e6c` stays on if halfword 0x0C150000 has a bit in 0x000E, or if dev 1
    reg 0x15 has bit 3 (or bit 4). Otherwise `0x50002f00` powers off: dev 1 reg 0x12 = 0,
    then spin.
  - The board declares dev 1 reg 0x15 = 0x0008 as a read-only status. [I] Bit 3 is the power
    key. Writes to it (0x0200 at 0x50002f54) are commands, not stored state.
- **Q15. The device 4 identity.** [V] `cbus.dll` 0x5037e008 reads dev 4 reg 0 and requires
  `(value & 0xFF00) == 0x0400`, else -2 and fault "CBus 25". It then pattern-tests dev 4
  reg 0x0D (0x55/0xAA). The board declares dev 4 reg 0 = 0x0400.
  - [I] An ASIC id/revision register (as Retu's register 0). Which chip device 4 is stays open.
- **Kernel reached (P1 exit).** [V] run15: PC in `ekern.exe` (0x5031xxxx), SP in kernel data
  (0x804xxxxx), 216 SVCs taken. A gdb breakpoint at the fault routine (0x5031e340) showed
  the first kernel-side fault, "CBus 25", which became Q15.

- **Q16. The interrupt controller at 0x0C120100.**
  - [V] Four banks of 32 sources, 0x40 apart: +08 set enable, +10 pending, +14 the source
    number to service (0 = none).
  - Evidence: ekern 0x5031e874 enable, 0x5031e8cc, 0x5031e798 pending, 0x5031e868 number.
    The IRQ dispatcher 0x5031c87c reads +14 of banks 0-2; the FIQ path reads 0x0C1201D4
    (bank 3).
  - [I] +0C disables (write-1-to-clear on the enable mask). Read as an acknowledge, the
    bootstrap's leftover timer status kills the kernel. The +14 value is 1-based, sources
    are level-sensitive, and bank 3 drives FIQ.
  - Run19's IRQ storm (1.7 M IRQs) was the board answering +14 = 0.
- **Q17. The timer at 0x0C120A00 = interrupt source 0.**
  - [V] Both the bootstrap and the kernel enable bank 0 bit 0 right after arming it. The
    kernel's helpers: 0x5031ea58 (mode, written and read back), eac4/eae8/eafc
    (+18/+10/+14), eb14 (+08).
  - [V] +08 is the interrupt status: the kernel clears it before enabling and in its tick
    handler 0x5031c5b0.
  - [I] Mode 2 is periodic, reloading from +18. The kernel tick is 0x843E2 = 13 MHz / 24.
- **Q18. Kernel debug output.**
  - [V] ekern 0x5031e4a8 prints when debug-mask bit 30 is set. The mask is superpage +0xD4,
    copied from the ROM header's `iTraceMask` (offset 0xD8) = 0xC0008003, so it's on in the
    shipped ROM.
  - [V] Each message: 0xC0 at virtual 0x59000000 (physical 0x0DE00000), the characters at
    successive byte addresses, then 0xE0 to virtual 0x591D0000 (physical 0x0DFD0000) on
    '\n'. A write-only trace port [I].
  - Board: lines go to serial 0. The runner writes `build/ngage/<run>.serial`.
- **Q19. Where is the NOS?**
  - [V] `nosstarter.ext` (148 bytes) copies 0x5488 bytes from virtual 0x5170B054 to
    0x80006EF0, zeroes 0xC1ED8 bytes at 0x8000C378, and calls the NOS entry 0x5149E4C4 with
    r0 = -1.
  - [V] The page tables map virtual 0x51400000-0x517FFFFF to physical 0x00000000-0x003FFFFF
    (supervisor-only sections). So the NOS is inside ROM.bin (entry at offset 0x9E4C4, data
    at 0x30B054), aliased.
  - The jump to 0x4F77ED04 (prefetch abort) was the first scrambled word at the entry
    (`bcc`); see Q20.
- **Fault reporting.** `ekern`'s fatal-fault routine is entered at 0x5031e340. Break there
  (`ngage_run.py --break 0x5031e340`) to read the faulting module from LR and its category
  text from r0.

## Decisions for the owner

- **NOS: descramble or replace (Q20).** Running the real NOS needs the MCU code descrambled,
  and the only implementation at hand is GPL (TriX). Options:
  - run TriX yourself to produce a descrambled image locally (a tool, not copied code; the
    bytes stay out of git);
  - find a non-GPL description and implement it independently;
  - stay with the stand-in (P3): register the NOS interfaces (variant slot 3 first) and answer
    ISI. This is what runs now (H-nos-standin, Q50-Q53): the phone reaches its idle screen.

## Board changes by question

| change | question |
|---|---|
| ROM at 0, Intel CFI01 flash at 0x02000000 (`device-width=2`) | Q1, Q2 |
| chip 0 = Intel CFI01 0x8874 with ROM.bin | Q4 |
| SDRAM 16 MB at 0x0A000000 | Q3 |
| `ngage_known`: 0x0C120020 bits 26/27, 0x0C149000 bit 0 | Q9, Q10 |
| CBus master at 0x0C150420 (register file, status table, log) | Q11, Q14, Q15 |
| ROM.bin split: offsets 0..0xEFFFFF on chip 0, the rest on chip 1 | Q12 |
| H-ngage-md5 (digest of the served image) | Q12 |
| timers 0-5 at 0x0C120A00 (live count, one-shot/periodic, 34.67 MHz and /15) | Q13, Q17, Q31 |
| interrupt controller 0x0C120100 (4 banks, level, +0C = disable) | Q16 |
| kernel debug output 0x0DE00000 -> serial 0 | Q18 |
| guest PC/LR in board log lines (TCG unwind; first 32 per register since Q56) | tooling |
| PPM segments (`ppm=`), flash blocks 64 KB | Q27, Q28 |
| LCD interface ready bit (0x0C121204 bit 2) | Q37 |
| ~~H-nos-none~~ replaced by H-nos-standin: stub over the NOS entry + device 0x0C1F0000 | Q19-Q25 |
| NOS item 12 = 105 (format a blank C:), slots 6/7 for medint | Q44, Q45 |
| timer armed by a count write; bank 3 = FIQ mirror of bank 0 | Q46 |
| LCD panel model + console; H-ngage-fb (screen chunk until lcd.ldd sends frames) | Q37, Q47 |
| CFI01 read-while-write partitions (`patches/qemu-ngage/`) | Q48 |
| stub: RTC hand-over (rtc.dll request 1) | Q49 |
| ISI over NOSDRIVER: `ngage_isi.c`, `isi=` reply table, the stub's delivery DFC | Q50, Q52 |
| H-ngage-startup (reset source + start-up code 1: normal list) | Q51 |
| keypad matrix (`ngage_keys.c`, CTSI irq 25) | Q53 |
| timer 3 at 34.67 MHz | Q54 |
| persistent flash (`flash=`), item 12 = 100 once kept | Q55 |
| hot-path logging limited; idle hook answered by the stub from RAM | Q56 |
