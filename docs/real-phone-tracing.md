# Real phones as an oracle

Plan of 2026-11-08 (owner's idea): record what a real phone does (messages, states, deaths, timing) and
compare it with qembian running the same firmware. The first divergence is usually the bug. Phones: the
owner's rooted N95 8GB RM-320 (same firmware as `models/nokia-n95-8gb-rm320`) and a rooted N8 with a
custom ROM (the kernel is in ROM flash, so modders do not change it; the user side may differ from
stock).

## Safety rule (owner)

Every on-phone recorder starts only when its output file does not exist. It creates the file first, then
records, flushing often. If the file exists, it does nothing: either the work is done, or the previous
attempt crashed the phone and must not run again. Re-arm = copy the file off the phone, delete it.
Raw traces stay in `build/evidence/` (they contain IMEI, IMSI, numbers, messages); only formats, reply
tables and findings are committed.

## Stages

| # | What | Where | Gives | Effort | Risk |
|---|---|---|---|---|---|
| 1 | **oracle.exe MVP** (built and tested in qembian 2026-11-08, `src/n95apps/devagent/oracle.cpp`): started by the boot hook (`HALSettings.exe` on C:, as in qembian), logs P&S changes of the keys qembian publishes, process/thread starts and deaths (exit type, reason, category), timestamps; stops after a fixed time | user side | boot state sequence and deaths on the real phone, directly comparable with qembian's `SYBPROP`/`Thread ... Panic` lines | 1 day | low: a user process; worst case the boot hook misbehaves (it is tested in qembian on this firmware) |
| 2 | ISI queries from Linux over USB (`cdc_phonet`, PC Suite mode): send the requests qembian's fake modem leaves unanswered to the real modem, record the answers | PC | exact reply formats and values (no phone code) | 0.5 day | none |
| 3 | sydb agent on the real phone over USB/BT serial: the same sydb commands (keys, taps, ps, wg, screenshots, P&S) against a real phone; scenario driving | user side | identical scenarios on both sides; UI action timing | 1-2 days | low |
| 4 | BTrace client-server category, if Nokia's kernel was built with it (check the images first) | kernel, existing | every client-server message, no hooking | 0.5 day to check | none to check |
| 5 | **kernel message tap**: a probe LDD hooks the kernel's message send/accept/complete (shadow ROM pages, as debuggers and ROMPatcher do); same records as qembian's KIPC trace (MsgAc/MsgCo/IPCRD) into a RAM ring drained to a file | kernel | the general oracle: every service request, result and timing | ~1 week | medium: a bug crashes/reboots the phone (guard file prevents a boot loop) |
| 6 | device paths: modem link send/receive (unprompted modem messages), audio DSP control calls and ring progress | kernel | modem indications schedule, the audio protocol for a non-silent DSP model | days per path | medium |
| 7 | file server plugin: every file operation of every process | user side (TCB) | missing files / failed opens (ROM composition problems) | 1-2 days | low |

Then a diff tool: align the real and emulated sequences per server/function (or per P&S key, per
process), report requests answered differently or never, and timing outliers.

## Limits

- Nothing before the file system is up (the boot hook runs ~0.5 s in); no hardware internals (DSP, modem
  CPU, GPU), only their interfaces.
- Kernel hooks are per kernel build (the N95 matches our profile; the N8 needs its own images).
- A full message trace is tens of thousands of records per boot: RAM ring buffer, filters; logging
  slows the phone slightly (fine for sequences, not for races).
- The custom N8 ROM's user side may differ from stock Belle; the N95 is the cleaner oracle.

## Stage 1 on the phone (N95 RM-320)

Files (built by `src/n95apps/devagent/build.sh`): `build/n95apps/boothook.exe` and
`build/n95apps/oracle.exe`. On the rooted phone, with a file manager that can write `C:\sys\bin`:
1. copy `oracle.exe` to `C:\sys\bin\oracle.exe`;
2. copy `boothook.exe` to `C:\sys\bin\HALSettings.exe`;
3. make sure `E:\oracle\boot.log` does not exist; reboot; wait 2 minutes;
4. copy `E:\oracle\boot.log` to the PC (`build/evidence/oracle/`), delete it to record again;
5. `python3 tools/oracle.py compare build/evidence/oracle/boot.log <qembian serial.log>`.
Remove `C:\sys\bin\HALSettings.exe` to stop the hook (the real one on Z: runs again).

## Stage 1 status (2026-11-08)

Tested in qembian on the N95 8GB firmware (the same firmware as the owner's phone): the hook starts the
oracle at 0.48 s; it records 77 P&S keys (initial values, then every change), thread starts (polled every
200 ms) and every thread death through the kernel's undertaker (`RUndertaker`: exit type, reason,
category, also for threads that lived less than a poll), ~300 lines in 132 s; the second boot finds the
file and exits at once (-11), the file unchanged. `tools/oracle.py compare` on a qembian run against its
own serial log reports only the known blind spots:
- P&S values that change twice within a millisecond show only the last (subscriptions coalesce);
- deaths before the oracle starts (HALSettings at 0.5 s) and, in qembian, `mc_useragent`'s KERN-EXEC 0
  (killed by the kernel inside an exec call; not reported by the undertaker here, reason not checked);
- deaths in the few instructions between an undertaker completion and the next logon.
Notes: the file is open (KErrInUse) until `end`; an EXE built with our `startup.s` gets no static
constructors (no global TBufs: the first version panicked USER 23).

Next (owner runs it on the phone): `tools/oracle.py compare build/evidence/oracle/boot.log <qembian
serial.log>`, then stage 2 (ISI over USB) or more keys/records in stage 1 (window groups once the window
server is up, CenRep notifications, memory).

## First real recording (N95 8GB RM-320, 2026-11-08)

`build/evidence/oracle/n95-boot-1.log` (not committed), compared with the same recorder in qembian
(`build/evidence/oracle/qembian-n95-boot.log`) by `tools/oracle.py diff`. The oracle starts at 3.6 s on
the phone (0.5 s in qembian): the real boot is slower before the hook runs. Both run offline without a SIM
(the owner's phone too), so state differences are real; the phone also runs the owner's apps (RomPatcher+,
X-plore, menu use). A clean ROM asks for language, region, date and time: processes and keys of that
wizard differ between a fresh and a seeded boot and are normal.

**Abnormal exits: none on the phone; in qembian** (each one a qembian bug to fix):

| Thread | qembian | Probable area |
|---|---|---|
| centralrepositorysrv `!CentralRepository` | kill -11 at 0.6 s (restarts) | a second instance / startup order |
| SplashScreen `Main` | panic E32USER-CBase 46 at 7.6 s | stray signal: an AO completed twice (likely a stand-in completes a request twice) |
| WlmServerExe `WlanServer` | kill -5 at 8.2 s | WLAN driver missing (wlanldd/wlanpdd) |
| UsbWatcher, usbsvr `!usbman` | kill -1 at 12.5/13.6 s | USB stack (usbsvr runs on the phone) |
| sdpserver, FaxModem | kill -1 at ~13 s | Bluetooth (no BT stand-in; btaccserver runs on the phone only) |
| lbdtracker | kill -1 / -11 | landmarks/location files |
| mc_isiserver, mc_useragent | die before the oracle starts (killed -1, KERN-EXEC 0 in the serial log) | MobileCrash servers: both run on the phone |

**Processes on the phone only**: mc_isiserver, mc_useragent, usbsvr, btaccserver (and the owner's apps,
sconpcconnserver = PC Suite over USB). **In qembian only**: upnpnotifappserver, sscserver, ipwserver,
bthidstarter, ipvoicemailappengine, pocstarter, pacontroller: never started on the phone (neither a start
nor an exit recorded), so they are started conditionally (SIM state, first boot, settings) - to check.

**P&S** (both offline, no SIM, so these are real differences; meaning of the keys still to look up):
101f75b6/1020e34a = 159 on the phone, 1 in qembian (set by EStart/LanguageSelection); 100052da 1@9.0 vs
3@14.2; 100052db 1@16.2 on the phone, never in qembian; 100052e9/100052f0 pass through 1/3 in qembian
only; 1020762e 3 then 0 at 23.2 s on the phone only; 100052fb = 2 in qembian only.

### Fixes from the first recording (2026-11-08)

1. SplashScreen E32USER-CBase 46: our own hack H-splash-behind (retired). Its patched literal was shared
   with the splash's dismissal P&S key; see models/nokia-n95-8gb-rm320/hacks.md.
2. CentralRepository killed -11 at 0.6 s: a second server instance finding the name taken (the usual
   start race when two clients start a server); the phone recording starts at 3.6 s and cannot show it.
   Treated as not a bug until a recording covers the first second.
3. MobileCrash: `mobilecrashdriver.ldd` now has a stand-in (it installs the device "MINKDA");
   mc_useragent opens it instead of dying KERN-EXEC 0, then exits -1, and mc_isiserver exits on its own
   after reading phone info and closing its ISC channel (ctl 15 = Close). Both stay alive on the phone:
   open (next: what mc_isiserver checks before it gives up; mc_useragent's -1 after MINKDA ctl 1).

## Stage 1 on the N8 (Delight 6.7, 2026-11-08)

The same two files serve the N8; the boot hook name is also `HALSettings.exe` (Belle's EStart). The
recorder now reads its P&S key list from `E:\oracle\keys.txt` (one `CAT/KEY` in hex per line), else it
uses its built-in N95 list; `tools/oracle.py keys <qembian serial.log>` writes the list a qembian boot of
that firmware sets (91 keys on Delight). Tested in qembian on Delight: 287 lines, 137 threads, every
death including mc_useragent's KERN-EXEC 0 (missed on the 9.2 kernel's run), 0 P&S differences against its
own serial log.

Belle's kernel refuses `RThread::Open` from a find handle for threads that are not global (file server
drive threads, the audio server's DevSound threads: -46, a PlatSec diagnostic each time). The recorder
logs them once as `thread? NAME open -46` and does not try again; their deaths still come through the
undertaker.

On the phone (rooted, a file manager that writes `C:\sys\bin`):
1. `build/n95apps/oracle.exe` -> `C:\sys\bin\oracle.exe`; `build/n95apps/boothook.exe` ->
   `C:\sys\bin\HALSettings.exe`;
2. `build/evidence/oracle/keys-delight.txt` -> `E:\oracle\keys.txt` (mass memory);
3. no `E:\oracle\boot.log`; reboot; wait 2.5 minutes;
4. copy `E:\oracle\boot.log` to `build/evidence/oracle/n8-boot-1.log`, delete it on the phone;
5. `python3 tools/oracle.py diff build/evidence/oracle/n8-boot-1.log build/evidence/oracle/qembian-delight-boot.log`.
Remove `C:\sys\bin\HALSettings.exe` afterwards.

Found on the way: our serial LDD (ECOMM) queued its transmit-fill DFC with `TDfc::Add` from thread
context (Syborg's UART has no transmit interrupt, so the PDD calls `CheckTxBuffer` at the end of
`EnableTransmit`, which also runs in the LDD's DFC); the udeb kernel asserted ("Don't call from thread
with kernel unlocked", Exception 0x10000000) while the agent was busy. Fixed in
`patches/kernelhwsrv/platform/0021-ecomm-tx-fill-dfc-from-thread.patch` (Enque from a thread, Add
otherwise, as the LDD's own TxComplete does).


## eglprobe: EGL / OpenVG / OpenGL ES report (2026-11-10)

`src/n95apps/eglprobe/build.sh` -> `build/n95apps/eglprobe.sis` (unsigned, no capabilities, Symbian^3
product UID). Installed, it appears in the application menu as "eglprobe"; each start writes
`E:\eglprobe\<n>.txt` (C:\data\eglprobe\ without E:) and shows a note with the path: EGL version and
strings, every config with every EGL 1.4 config attribute, OpenVG strings and limits, a pbuffer from a
VGImage, OpenGL ES 1.1 strings. The same SIS runs in qembian (`sydb sis install`, `sydb launch
0xE0001DC0`), so the real phone's report and ours diff line by line. Raw reports from the phone go to
`build/evidence/`. qembian (Delight, 2026-11-10): EGL 1.2 from eglInitialize but EGL_VERSION "1.3", no
EGL extensions, no VG extensions.
