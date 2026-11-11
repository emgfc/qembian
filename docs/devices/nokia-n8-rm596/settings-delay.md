# N8 (Delight 6.7): seconds-long delay when opening Settings items (investigation, 2026-11-09)

**Follow-up, 2026-11-09:** the Settings-entry stall has been causally isolated on
the 5800 to a wrong SIM service byte in our modem response, making synchronous
`GetAlsBlocked` time out after about ten seconds. The existing Delight log has
the same malformed exchange. See [the A/B/A investigation](../nokia-5800-rm356/settings-delay.md).
The compositor explanation below remains an earlier hypothesis, not an
established cause. In particular, opcode 65545 is redraw-ready in the available
WSERV headers, not synchronous command-buffer flush. Every N8 session until 2026-11-10 still
got the wrong `2a 35 24` (only the 5800 ROM had been rebuilt with the fix).

N8 Delight with the fix (2026-11-10, copy of the owner's drives, `build/sydb-delight-alsfix`):
`29 37 00` -> `2a 37 24`, IPC 1000012 completes in 2 ms (-1). Timed screenshots from the tap:
Settings -> Phone (tapped 1.5 s after launch, list not yet drawn): highlight 0.4 s, Phone title
0.9 s, its list drawn 2.3 s; Phone management fully drawn 1.2 s (8.4 s before); Display 1.4 s
(3.7 s before). Whatever delay remains on the N8 is about a second per view and is not this bug;
the ~1 s blank page before a sub-list draws is the next thing to measure, against real-phone timing.

Symptom (owner; also seen since the 5800): tapping an item in Settings (Profiles, Phone, its sub-items)
takes seconds to tens of seconds to react.

## How it was measured (no reboot, no trace build)

- Session `build/sydb-delight` (Delight ROM, agent up), Settings = `GSApp.exe` 0x100058ec (`sydb launch`).
- `sydb strace zzz --secs 60 --max 100000000` only to keep the kernel's KIPC bit on: every IPC message of
  every process goes to the session's serial.log while it is on (the strace event list is a filtered view;
  its old default cap of 200 events is what cut the earlier attempt short).
- `tools/ipc_latency.py build/sydb-delight/serial.log --since T --min 0.3 [--match gsapp]`: pairs each
  request with its completion, lists the slow ones and the ones still open.
- `tools/pcsample_hmp.py build/sydb-delight 8`: where the CPU is during the stall.

## Review of the earlier conclusions (2026-11-09)

The investigation did collect useful traces. The failure was treating the trace
summary and observations about idle threads as a proven dependency chain. The
5800 A/B/A experiment isolates the entry stall; it does not retroactively explain
every N8 transition delay. Preserve that distinction when reading the history below.

| Earlier assumption/conclusion | Why it was unsafe or wrong | Required check |
|---|---|---|
| “fn 4 with fn 65545” is a synchronous flush waiting for seconds | These are different operations. In the available headers 4 is SyncMsgBuf; 65545 is async RedrawReady. An outstanding notification can be healthy. | Decode each opcode and validate its matching completion in raw logs; verify firmware-specific numbering. |
| `WaitForRequest` means nobody is in a synchronous wait | Synchronous client wrappers can issue a request and then wait on the request semaphore. This state also occurs in idle active schedulers. | Inspect the user call site/stack and outstanding request, not just the nanokernel wait object. |
| “Not the cause: the modem” | The old Delight log already contains `29 37 00` → `2a 35 24` at 181.341 s. A delivered packet is not necessarily an accepted response. | Check resource, transaction ID, service discriminator, status, length, and eventual ETel completion. |
| Long open IPC entries identify the blocking server | Notifications intentionally stay open; the latency tool lacks message/session IDs and can mispair calls. | Establish which request is synchronous on the affected UI thread. Follow that request to its completion/error. |
| Filter Settings by `gsapp` throughout | In the 5800 trace its name changes from `GSApp.exe::GSApp` to `Settings::Settings`. A name filter can hide the relevant request. This has not been established as the specific N8 failure. | Track identity/name changes and inspect unfiltered raw logs around reproduction. |
| Four-second alf gaps and loaded effects prove a transition dependency | Correlation does not establish what the app is waiting on. The notes explicitly marked this inferred, but subsequent work treated it as the main explanation. | Identify the actual pending operation; intervene on one cause and reverse the change. |

### Approach that isolated the Settings-entry bug

1. Reproduce the owner's exact sequence: main menu → Settings → immediate tap.
   Opening Settings with `launch`, waiting, and testing arbitrary subpages is not
   the same experiment. Capture timed screenshots to establish when the list is
   visible and whether the tap is delayed rather than lost.
2. Enable KIPC before the sequence and keep it on beyond the stall. Inspect the
   affected thread's last requests in the raw log, including its renamed identity.
   The decisive request was custom ETel IPC 1000012; async 1000016 beside it is a
   notification, not the synchronous query.
3. Follow the query across the boundary: ETel → modem request → our response →
   ETel completion. Here the response arrived immediately but carried the wrong
   service, and ETel completed about ten seconds later with -33.
4. Change only the service byte in the running test VM. Original/corrected/restored
   gave **9.843 s / 0.001 s / 10.187 s**. The immediate tap worked with the corrected
   response. This is causal evidence; CPU-idle percentages and visual correlations
   are not substitutes for it.
5. Verify statically in the original firmware: the 5800 NOKIATSY handler reads
   byte 10, accepts 0x37/0x38, ignores 0x35, converts the negative status, and
   completes IPC 1000012. Addresses and reproduction are in the linked 5800 note.
6. Implement the protocol correction, rebuild the actual affected artifact (guest
   stand-in/ROM), and reproduce without debugger edits. Record compatibility
   limits: N8 intervention and N73/N95 regression tests remain outstanding.

## Historical findings (original claims, subject to review above)

- Settings itself waits on the window server: its synchronous command-buffer flush (`!Windowserver` fn 4 with
  fn 65545) took 3.7 s and 8.4 s on "Display" and "Phone management" (11.3 s on another run).
- During the stall the phone is idle: 87% of PC samples in the kernel idle loop; the host GPU is not slow
  (899 kicks, 240 ms in total); the window server has no IPC request open (nothing to wait for in IPC).
- alf (thread `alfredserver` in EwSrv) runs in bursts separated by ~4.0 s gaps (304.2-304.4, 308.4, 312.6
  in one run): before each gap alf submits a surface update (`SURFACE_UPDATE_SERVER` fn 6, which gcebe
  completes at once with consumed/displayed/displayed-x-times) and the window server pokes alf's streamer
  (fn 11). A ~4 s timeout in the firmware ends each wait [I].
- Every tap starts a view-switch transition: alf's streamer loads `z:\resource\effects\viewswitch_left.fxml`
  / `viewswitch_right.fxml` at each tap. The same transitions show as black frames (menu -> Games: one black
  frame, then the new view). So the delay and the missing transitions are very likely one cause: the
  transition effect waits for something our platform never delivers and gives up after ~4 s [I].
- Not the cause: feature 0x666 (KFeatureIdUiTransitionEffects) is on in Delight's features.dat; clearing it
  (test ROM, both ROFS copies) did not stop the effects being loaded nor the stall, so alf does not gate its
  transitions on that feature.
- gsapp's request to `alfstreamerserver` fn 3 stays open 4-17 s and completes 66 or -3 (KErrCancel): the
  app side of the transition (GfxTransEffect begin/end) [I].

## Historical second pass (2026-11-09, halted)

Original recorded claims (the wait-state interpretation and modem exclusion are
not established facts; see review above):
- During a stall every thread of the window server (Wserv, alfredserver, the surface update server, alf's
  streamer) and gsapp waits in User::WaitForRequest (nanokernel wait = the thread's own request semaphore,
  `tools/sydb/gdb/threads-blocked.gdb`): nobody is blocked in a synchronous kernel wait; the window server
  holds gsapp's flush until an event arrives.
- alf's thread renders frames on a 15 ms (sometimes 1 ms) high-resolution RTimer; during a stall it stops
  arming it. It is woken again from DfcThread1.
- No off-screen composition is requested during the taps (gcebe now logs it; only one at boot), so the
  missing off-screen composition in gcebe is not the cause.
- Not the cause either: the feature flag 0x666, the surface update notifications (gcebe completes them at
  once), the modem, the host GPU (kicks are synchronous and fast), the BASE_UPDATE interrupt (masked).

Owner's verdict: timers are not the direction; do not continue that line.

Pitfalls met (for the next attempt):
- The kernel .sym must match the ROM's kernel: after a kernel rebuild, recompose the ROM before using symbols
  (here the Delight ROM was rebuilt with the current kernel, 0023 included). With a stale .sym, breakpoints
  land on other code and data addresses are off.
- Kernel data addresses: runtime = 0xC8000000 + (.bss/.data address in the .sym - 0x400000)
  (K::Containers 0xC800188C, TheScheduler 0xC8000AF8, current thread pointer at +304).
- `sydb dbg cmd` returns about the last 55 lines; gdb convenience variables persist between commands.
- `sydb catch add --thread` does not match kernel-side breakpoints (thread shows as '?'): use a gdb
  condition on the current thread pointer instead.

## Historical next steps (superseded for the Settings-entry stall)

1. Find the ~4 s wait: with the IPC trace on, catch `ExecHandler::TimerAfter` / `SemaphoreWait` in EwSrv only
   (the catch thread filter does not resolve kernel-side thread names: filter the hits by the current
   thread in the dump instead), or read alf's effect code path from `alfstreamerserver` fn 3 and fn 11.
2. Likely suspects for what the effect waits for: a screen capture of the old view (OpenVG / EGL readback of
   the window surface, or a surface-update "displayed" for a surface gcebe does not compose), or a frame
   callback from the composition (gcebe's Commit completes at once; check whether alf expects a later one).
3. A switch to turn alf's transitions off (the Themes "Theme effects" CenRep value) would confirm the cause
   and could serve as a workaround until the effect works.
