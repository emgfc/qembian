# Starting the sydb agent at boot on every OS generation

Research note (2026-11-08, static analysis only: firmware listings in `build/roms/<id>/<fw>/`, strings of
the core ROMs, EPL sources in `external/` and `build/epocroot/sf/`, our tools). Claims are marked
[verified] (read in our sources / firmware images / session logs) or [inferred].

Question: the agent (`devagent.exe`, All-TCB, serial unit 1, plus the TCB helper `dafs.exe`) must start
at boot on 9.1–9.3 (done: Starter's `start.dat`), 9.4 (5800: never started), Symbian^3 (N8: nothing
yet) and the coming Anna/Belle N8 firmware. We compose the ROM and build the kernel, so anything from
"a record in the phone's own start-up list" to "our own EStart" is available.

## Decision (2026-11-08): the EStart hook (d1), one mechanism, not configurable

Implemented: d1 below, on every phone, with no profile key (the `[debug] agent` switch of 4.1 and the
`ssm` resource of 4.4 were not built; one mechanism is less to maintain). What changed from the
design:

- The hook is its own executable, `src/n95apps/devagent/boothook.cpp` (`build/n95apps/boothook.exe`,
  EUSER only). `sydb install` / `start --seed` / `update-agent` put it on C: under the name of a program
  the firmware's EStart starts by bare name, read from the EStart image (`tools/device.py`
  `boot_hook_name`): `HALSettings.exe` on 9.2 and later, `LanguageSelection.exe` on 9.1 (the N73's EStart
  has no HALSettings at all). The hook runs `z:\sys\bin\<its own name>` with the same command line.
  `dafs.exe` stays the `\sys` mover only.
- The real HALSettings accepts `INITIALISE` only when its creator's SID is EStart's (0x10272C04, EPL
  `halsettings.cpp` `HALSettingsManager()`); a hook with its own SID made it exit with
  KErrPermissionDenied (-46) [verified on the N8]. `boothook.exe` therefore carries SID 0x10272C04
  (`build.sh`: `uid3:sid`), and the real z: HALSettings now returns 0 [verified on the N8]. Nothing
  else checks the SID of a C: executable on our phones.
- HALSettings runs again later in boot (`PERSIST`; N8: at 1.6-3.3 s); the hook starts the agent only
  when no `devagent*` process is alive (log `BOOTHOOK: agent -11`).
- Log lines: `BOOTHOOK: agent R, z:\sys\bin\NAME R` and `BOOTHOOK: exit R`.

Why Starter's dynamic list (a1, used until 2026-11-08) was dropped, for whoever wants it back:
- 9.4 (5800): Starter never read `start.dat` in any boot with our fake modem (section 1, three
  candidate causes, none checked); the agent never started there.
- Symbian^3 and later have no Starter list at all (SSM + DSC store, a3/a4), so a second mechanism
  would be needed anyway.
- It started the agent only after the UI (~21 s on the N95), so boot problems before that were out of
  reach, and every agent rebuild had to re-register its MD5 (`sydb startup add`).
- It is the only mechanism that reproduces an installed app's start-up timing. The code stays:
  `tools/sydb/startlist.py` and `sydb startup add|rm|check|list` (live, StarterServer requests).
  `sydb install` removes an older install's agent record from `start.dat` so the agent does not start
  twice.

Sections 1-5 below are the research note as written before the decision.

## 1. What each generation actually does at boot (facts)

| | 9.1–9.2 (N95, N73) | 9.3 (N79, 5730) | 9.4 (5800) | Symbian^3 (N8 011.012) |
|---|---|---|---|---|
| Start-up framework | `Starter.exe`, SID 0x100059C9 [verified] | `sysstart.exe` **is** the Starter: SID 0x100059C9, holds the `StarterServer` name and the `c:\private\100059C9\start.dat` / `start.tmp` / `bckupstart.dat` literals [verified] | same as 9.3 [verified: strings + SID of `sysstart.exe`; the 5800 IPC log shows the `sysstart.exe::StarterServer` thread] | `sysstatemgr.exe` (SSM), SID 0x2000D75B [verified]; `starterclient.dll` still in the ROM but no `start.dat` literal anywhere in the core [verified] |
| Dynamic list (what a SIS install registers) | `start.dat` (DES, `tools/sydb/startlist.py`) [verified] | `start.dat`, same reader code family [verified strings; works on the N79 per docs] | `start.dat` literal present, but **no boot ever read it**: the 5800 IPC trace has no `start.dat` open at all [verified, `build/sydb-5800ipc`] | DSC store: `dscstore.dll` opens a DBMS secure shared database `%c:DBS_DSC.db`, format `secure[2000836D]`, tables `DscItem` (ItemId, FileName, ArgList, StartMethod, Timeout, NoOfRetries, Monitored, StartupType, Viewless, StartInBackground) and a DSC index table [verified strings]; policy file `private/100012a5/policy/2000836D.spd` is in the core [verified]. Read by `amastarter.exe` (started by the SSM command `SSM_START_AMA_STARTER`, `dsc_id = 0x2000836D`) [verified rss/rh] |
| Static lists (ROM) | Starter's own resources | `z:\resource\Starter_Arm.rsc`, `starter_non_critical_1.rsc` (ROFS1) [verified] | + `starter_ui_seq.rsc`, `starter_background_apps.rsc` [verified] | `z:\private\2000D75B\startup\0\*.rsc` (UID2 0x2000D764), one `SSM_COMMAND_LIST_ROOT` per file, lists merged per `command_list_id` across files (`noncriticalcmdlist_ext.rsc`, `_hw.rsc` are exactly such add-on files; `SYMBIAN_SSM_FLEXIBLE_MERGE` gives every command a `priority`) [verified: ROM listing + EPL `ssmcmdlists/data/*.rss`, `ssmcmd.rh`] |
| `c:\private\101f875a\import\[uid].rsc` | SID 0x101F875A is `SWInstSvrUI.exe` (the installer); the only ROM binary with that path literal is `SWInstStartupTask.dll` on all three generations [verified]. The STARTUP_ITEM_INFO files are read **at SIS install time** and turned into a Starter / DSC record. Nothing scans the directory at boot [inferred from the above; the N8's ROM copy `[20012BE9].rsc` is 64 bytes of dictionary-compressed resource]. Not a boot mechanism. |
| EStart | Nokia's `TH2FSStartup` (firmware, kept in our hybrid ROM; "N95" column = firmware in `hybrid-manifest.txt`) [verified]. All three contain the bare literals `HALSettings.exe` + `INITIALISE` [verified], i.e. the EPL `TFSStartup::InitialiseHAL()`: `RProcess::Create(_L("HALSettings.exe"), _L("INITIALISE"))`, Logon, Resume, wait for exit, exit reason returned [verified EPL `estart.cpp`]. The loader resolves a bare name over `\sys\bin` of **Y:..A: first, then Z:** [verified EPL comment in `estart.cpp`, and empirically: hack H-sydb-autostart worked on the N95]. |
| Serial for the agent | our `ECOMM.LDD` + `EUART1.PDD` (baseport) in every hybrid ROM; on the N8 the firmware's `ecomm.ldd` is a declared-missing driver and ours takes the name [verified `hybrid-manifest.txt`, `kernel-drivers.txt`] |

Why `start.dat` did nothing on the 5800 [inferred, three candidates, cheapest first]:
1. The dynamic list is started at a global system state the 5800 boot never reaches with our modem: the
   5800 ends in `101f8766/41 = 110` (N79: 100..104 visible), and Starter runs the list only from a
   "normal" state (RF on/off). Check: `sydb strace sysstart` for `start.dat` after the state lands, or a
   catch on the `start.dat` literal (`sydb dbg`), on a session where `sydb install` was run.
2. A feature flag (Starter checks FeatureManager feature 0x421 on the N95) that the 5800's
   `features.dat` does not set.
3. First-boot RFS: `sysstart.exe::StarterServer` runs `Z:\private\100059C9\ScriptInit.txt` and the
   `plg_script*.txt` through RfsServer at 4.3 s [verified log]; the `start.dat` written offline survived in
   `build/sydb-5800/drives` (56 bytes) [verified], so (3) is unlikely unless the seed was pre-RFS.
Not worth chasing if the recommendation below is taken: it covers 9.4 without Starter.

## 2. Mechanisms compared

| # | Mechanism | How | What we write (file, format) | Covers | Boot order / risks | Effort |
|---|---|---|---|---|---|---|
| a1 | Starter dynamic list `start.dat` (today) | Starter starts type 1/7/8 records whose EXE MD5 matches, after the global state trigger (~21 s on the N95, after the UI) | `c:\private\100059C9\start.dat`, DES-ECB records (`startlist.py`) | 9.1–9.3 [verified]; 9.4 nominally, broken in practice | starts after the UI; MD5 re-registration on every agent rebuild; feature-flag/state dependent; useless on Symbian^3 | done |
| a2 | Starter static lists | rebuild `z:\resource\Starter_Arm.rsc` / `starter_non_critical_1.rsc` with one more item | `[[rom.rofs_replace]]` of the ROFS1 resource with a recompiled one; needs Nokia's starter `.rh` (not in the SDKs; only `starter.hrh` item types 100/102 [verified]) and a resource compiler | 9.3–9.4 only | reverse-engineer Nokia's resource layout per firmware; `rcomp.exe` only (no Linux rcomp, no wine on this host [verified]) | high, per firmware |
| a3 | SSM command list add-on file | drop `devagent.rsc` into `z:\private\2000D75B\startup\0\`: `SSM_COMMAND_LIST_ROOT` → mapping `command_list_id = <non-critical list id>` → `SSM_COMMAND_LIST` with one `SSM_START_PROCESS_INFO { name = "c:\sys\bin\devagent.exe"; execution_behaviour = ESsmFireAndForget; priority = ... }` | new core-ROM file via `[rom].files` (composite Z: merges core and ROFS1 directories [inferred, standard composite FS]) or appended to our ROFS1 copy; needs a resource writer: plain (uncompressed) `.rsc` from Python, constants from `ssmcmd.hrh` (UID2 0x2000D764, `ESsmCmdStartProcess`, version `ECmdStartProcessVersionWithPriority`) | Symbian^3, Anna, Belle [inferred: same SSM] | starts in the non-critical phase (after UI services, before/around the home screen); the list id `ESsmStateNonCritical` is Nokia's (`ssmsubstateext.hrh`, not in our sparse `external/`; read it from the SF package or decode it from the N8's `noncriticalcmdlist_ext.rsc`); per start-up mode directory (`startup\0` only at first) | medium: resource writer (~150 lines) + `device.py build` hook |
| a4 | DSC store + AMA starter (what a SIS install does on Symbian^3) | a `DscItem` row with `FileName = c:\sys\bin\devagent.exe` | Symbian DBMS permanent-file-store database `c:\private\100012a5\DBS_2000836D_*DSC.db` [inferred name] — there is no Python writer for DBMS files and the format (stream store + DBMS tables + index) is not worth one | Symbian^3+ | would also need the AMA command to be present in the list (it is: `r_cmd_amastart` in `noncriticalcmdlist_ext`) and a normal-boot condition; writing it from the phone (an `RDscStore` helper run once by the agent) is a chicken-and-egg | high offline; low once an agent already runs (a `sydb startup add` back-end for Symbian^3) |
| a5 | `c:\private\101f875a\import\[E0001DB7].rsc` | — | STARTUP_ITEM_INFO (`startupitem.rh`, in all three SDKs) | none at boot: install-time only [verified SID owner] | — | — |
| b1 | ROM-level variants of a1–a3 (`[rom].files`, `[[rom.rofs_replace]]`, `[[rom.rofs_patches]]`) | the start-up configuration lives in the composed ROM instead of C: | as above | per generation | `rofs_replace` only repoints existing entries (no new ROFS directory entries) [verified `device.py rofs_patched`]; `[rom].files` adds any path to the core ROM [verified `--file`] | see a2/a3 |
| c1 | Our own EStart (EPL `estart` sources we already build for the plain Syborg ROM; `TSyborgFSStartup` creates a process before `StartSystem()` [verified]) | replace the firmware's `EStart.exe` in the core ROM | custom `TFSStartup` subclass | all, in principle | Nokia's `TH2FSStartup` does much more than the EPL one: `custom.ldd` / `RIscApi`, SecLdd, `LanguageSelection.exe`, `sospmmhandler.exe`, `EInfoServer`, `IntegrityCheck`, Mobile Crash Starter, modem AT controller, `SYSSTATEMGR.EXE` [verified strings]; re-implementing it per generation is the opposite of universal | high, fragile |
| c2 | Code patch of the firmware EStart | retarget its `HALSettings.exe` literal (15 chars) at our launcher | `code-patches.txt` entry per firmware | all | the replacement path must fit 15 UTF-16 chars (`\sys\bin\sa.exe` fits, drive-less path search [inferred]); one patch per firmware image; also loses the real HALSettings unless our launcher chains | medium, per firmware |
| c3 | Kernel / kernel extension / EFile | — | — | — | the kernel creates exactly one user process (the file server) without the loader; everything else needs the loader inside `efile.exe` (firmware's). A kernel extension cannot create user processes. Dead end. | — |
| **d1** | **EStart hook: `c:\sys\bin\HALSettings.exe` launcher** (the retired H-sydb-autostart, done properly) | EStart runs `HALSettings.exe` by bare name right after mounting the drives and before SysStart/SSM/the window server [verified EPL + strings]; the loader finds C: before Z: [verified]; our launcher spawns `devagent.exe` and then **chains to `z:\sys\bin\HALSettings.exe` with the same command line, waits, returns its exit reason** (the retired version returned −12 and skipped the real one) | one more copy of `dafs.exe` on C: named `HALSettings.exe`; no ROM change, no resource compiler, no record format | 9.2, 9.4, Symbian^3 [verified literals in all three EStarts]; Anna/Belle [inferred: same EStart family; `device.py check` can assert the literal] | earliest possible user-side start: file server up, C: mounted, serial LDD/PDD loadable (ROM, ours), **no window server yet** (the agent already connects WS lazily, `CAgent::Ws()` [verified]; anything else it does eagerly must tolerate missing servers). Depends on the EStart literal and the loader search order; the agent's own crash would not block boot (fire-and-forget), the chained HALSettings keeps HAL persistence | low: ~15 lines in `dafs.cpp`, ~10 in `sydb install` |
| d2 | Same launcher in the ROM | shadow Z:'s HALSettings | impossible without renaming the firmware file inside the core image (we keep the firmware user side as a blob) | — | — | — |

## 3. Recommendation

**Use d1 everywhere (`estart-hook`) as the universal default; keep a1 (`starter`) as the faithful
alternative on 9.1–9.3; add a3 (`ssm`) later as the faithful alternative for Symbian^3+ if a boot-order
closer to a real installed app is ever needed.**

Reasons: d1 is the only mechanism whose ingredients are identical on every generation we have (the EPL
`InitialiseHAL()` code and the loader's drive order), it needs no format reverse-engineering, no
resource compiler, no ROM rebuild, and it already worked once on the N95. It also starts the agent
before the UI, which is what debugging the boot itself needs. Its two dependencies are checkable
offline for every new firmware (the `HALSettings.exe` literal in `EStart.exe`; the hook file on C:).
The DSC route (a4) is not worth a DBMS writer; the SSM resource route (a3) is a reasonable second step
because the resource is a plain file in our ROM and SSM merges add-on files by design.

Fidelity note: with d1 the phone's own start-up lists stay untouched; the only visible change is one
extra process early in boot. H-sydb-autostart returns to `docs/hacks.md` (un-retired, "chains to the
real HALSettings").

## 4. Design

### 4.1 Profile

```toml
[debug]
agent = "auto"        # auto | estart-hook | starter | ssm | none
```

- `auto` (default when the table is absent): `estart-hook` on every generation. The N95 and the other
  9.1–9.3 devices move over after one smoke test each (`tests/device-smoke.sh`); until then their
  profiles may say `agent = "starter"` explicitly.
- `starter`: today's `start.dat` record (9.1–9.4 only; `device.py check` refuses it on `os_version =
  "symbian3"`).
- `ssm`: the SSM add-on resource (Symbian^3+ only), generated at `device.py build` time.
- `none`: install the EXEs only.

Resolution lives in one place, `tools/device.py` (`get debug.agent` plus a helper
`agent_mechanism(prof)` that maps `auto` by `os_version`), so `sydb` never carries an OS literal.

### 4.2 `sydb install` / `uninstall` (session stopped, C: image offline)

```
mechanism = device.agent_mechanism(prof)
put devagent.exe, dafs.exe -> C:\sys\bin            (all mechanisms, as today)
estart-hook: put dafs.exe again as C:\sys\bin\HALSettings.exe (drive_put name=...); remove any start.dat record
starter:     write the start.dat record (startlist.add); remove C:\sys\bin\HALSettings.exe
ssm:         nothing on C: (the resource is in the ROM); remove hook file and start.dat record
uninstall:   remove all three artefacts whatever the mechanism
```

`sydb update-agent` (live) is unchanged: it replaces `devagent.exe` through `dafs upgrade`; with
`estart-hook` there is no MD5 to re-register, so the `startup add` step becomes conditional on the
mechanism. `sydb start` prints the mechanism in its status line and warns when the hook file (or the
record) is missing from the session's C: so an "agent never came up" is diagnosed before the boot.

### 4.3 `dafs.cpp` (the launcher side)

```
if (own file name is "HALSettings"):
    spawn c:\sys\bin\devagent.exe (Create + Resume, ignore errors, log "DAFS: hook: agent -> r")
    RProcess real; r = real.Create(_L("z:\\sys\\bin\\HALSettings.exe"), User::CommandLine())
    if r == KErrNone: Logon; Resume; WaitForRequest; return (ExitType()==EExitPanic ? KErrGeneral : status.Int())
    return r          // EStart ignores the value except for its log; the retired hook returned -12
```

Nothing else changes: `dafs.exe` stays ALL-capability, EUSER/EFSRV only, so the loader accepts it as
`HALSettings.exe` on every generation (the loader checks the file's own capabilities, not its name).

### 4.4 `device.py build` / `check`

- `check`: with `estart-hook`, assert the UTF-16 literal `HALSettings.exe` and `INITIALISE` in the
  firmware's `Sys/Bin/EStart.exe` (core.json address + `core-flat.img`), so a new firmware (Anna, Belle)
  is validated before the first boot. With `ssm`, assert `Private/2000D75B/startup/0/` in ROFS1 and
  `sysstatemgr.exe` in the core.
- `build`: no change for `estart-hook`/`starter`. For `ssm`: a new `tools/sydb/ssmrsc.py` writes
  `build/roms/<id>/<fw>/gen/devagent.rsc` (plain resource file: 16-byte UID header with UID2 0x2000D764,
  resources `root`, `mapping`, `command list`, `start process info`, index at the end; constants from
  `ssmcmd.hrh`) and `device.py` adds `--file Private/2000D75B/startup/0/devagent.rsc=<gen>` exactly like
  `[rom].files`. The command names `c:\sys\bin\devagent.exe` so `sydb update-agent` keeps working.
  The list id (`ESsmStateNonCritical`) is taken from the SF `ssmsubstateext.hrh` or decoded once from the
  firmware's `noncriticalcmdlist_ext.rsc` and stored in the family file as `ssm.noncritical_list_id`.

### 4.5 Step plan

N8 first (`estart-hook`):
1. `dafs.cpp`: the hook branch above; `build.sh` unchanged for the first try (9.2 SDK binaries).
2. `sydb.py`: `[debug] agent` resolution via `device.py`; `install`/`uninstall`/`update-agent` per 4.2.
3. `SYBORG_DEVICE=nokia-n8-rm596 sydb --session build/sydb-n8t install`, boot the test session; expect
   `DAFS: hook: agent -> 0`, then the agent's `serial unit 1 open -> 0`, then `sydb proc list` shows
   `devagent.exe` before `Startup` reaches `101f8766/46 = 104`. If `devagent.exe` fails to load (−5/−6,
   missing import), go to 4.6 and rebuild against the Symbian^3 SDK.
4. `tests/device-smoke.sh nokia-n8-rm596`; move `[debug] agent` to `auto` in the N8 profile; note the
   hack in `docs/hacks.md` and the mechanism in `docs/sydb.md` "Autostart".
5. Anna/Belle variants: `device.py check` (4.4) is the gate; nothing else should change.

Then the 5800 (`estart-hook`, same steps; the open item "sydb agent on 9.4 start-up" closes). Starter's
`start.dat` on 9.4 is then only of academic interest (section 1 lists the checks if wanted).

Then the 9.1–9.3 devices: switch `auto` on, smoke test each, retire `starter` as the default but keep
the code (it is the only mechanism that reproduces an installed app's start-up timing).

### 4.6 The agent binary on Symbian^3 (and 9.4)

- **ABI / SDK**: `devagent.exe` is linked against the S60 3rd FP1 (9.2) import libraries
  (`build.sh`); the profiles already name the matching SDK per device (`sdk_lib`: S60 5th, Symbian^3)
  [verified]. EUSER/EFSRV/WS32/APGRFX/APPARC ordinals are kept binary compatible from 9.2 to Symbian^3;
  AVKON's less so [inferred]. First try the 9.2 build; if the loader rejects it (missing export), add a
  `--sdk` switch to `build.sh` producing `build/n95apps/<ui>/devagent.exe` and let `sydb install` pick the
  build for the profile's `ui`. Better long-term: drop the AVKON link from `devagent.exe` (keep only
  WS32/APGRFX for window groups and key events) so one binary serves every generation.
- **Capabilities / SID**: All-TCB in `c:\sys\bin` is accepted by the loader on every generation; SID
  0xE0001DB7 (unprotected range) is fine as the agent registers with no server by SID. The hook copy
  of `dafs.exe` runs as `HALSettings.exe` but with its own SID, so it cannot read HALSettings' private
  dir — irrelevant, it only chains.
- **Early start**: with d1 the agent runs before the window server, the font/bitmap server, apparc and
  P&S categories that the UI defines. `devagent.cpp` must only open `RFs` and the serial port at
  start (it already retries the LDD/PDD load and connects WS lazily [verified]); every command that
  needs a server must return the connect error instead of panicking. Check the one eager
  `StarterServer` session (`Connect()` at line 188) stays on demand.
- **Serial**: unit 1 on our `ECOMM.LDD`/`EUART1.PDD`, present in the N8 hybrid ROM [verified];
  the firmware's `ecomm.ldd` is absent and ours carries the name, so `LoadLogicalDevice(_L("ECOMM"))`
  resolves to ours. The QEMU side (chardev for unit 1 → `agent.sock`) comes from `device.py qemu-args`
  and is device-independent [verified: the daemon reads serial1 for every session].
- **Paging**: Symbian^3 demand-pages ROM code; our E32 images from `elf2e32_next` are unpaged RAM
  executables on C:, loaded whole. No change.
- **`dafs upgrade`** stamps a module version per deploy to defeat code-segment reuse; unchanged.

## 5. Open points

- `ESsmStateNonCritical` value and the exact DSC file name: only needed for `ssm`/`a4`, both deferred.
- Whether Nokia's EStart on Anna/Belle still calls `HALSettings.exe` by bare name: assumed; the
  `device.py check` assertion makes it a build-time fact.
- The retired hook returned −12 instead of chaining; the new one chains, so HAL persistence (display,
  contrast, `hal.dat`) now behaves as on the stock ROM — verify once on the N95 that nothing changes at
  the first boot after install.
