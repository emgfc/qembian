# N95 unprivileged IPC: what a self-signed app can actually reach

Status: first pass over the stock RM-320 (N95 8GB, Symbian 9.2 / S60 3rd FP1) binaries.
Goal: find a capability-boundary crossing that lets an ordinary self-signed SIS app write
`c:\sys\bin\ssl.dll` (the TLS-modernisation plan), not just crash a server.

Everything below was measured from the extracted stock core
(`build/devices/nokia-n95-8gb-rm320/core-flat.img` + `core.json`) with
`tools/xip2elf.py`; ELF copies and disassembly are in `tmp/n95api/` (not committed).
Tags: **[verified]** = read from the binary/source in this repo, **[inferred]** = hypothesis
that still needs a probe.

## 1. Threat model

A self-signed SIS installed by the user gets the user-grantable capability set only
(typically `NetworkServices`, `LocalServices`, `ReadUserData`, `WriteUserData`,
`UserEnvironment`; no `AllFiles`, `TCB`, `DiskAdmin`, `TrustedUI`, `ProtServ`).
It **can** connect to any server whose `CPolicyServer` connect policy passes, and then
call the opcodes whose per-op policy passes. A server that has `AllFiles` and accepts
unprivileged requests is the target.

Do not equate "the server binary has capability X" with "a client can ask it to do X".
The check is per session/opcode in the server's policy table. This note maps those tables.

## 2. The important finding: `!ABServer` is open

`sbengine.exe` (SID `0x10202d56`, caps `AllFiles`, `ProtServ`, `WriteDeviceData`,
`ReadUserData`) hosts two servers:

| service | connect policy | opcode policy |
|---|---|---|
| `!ABServer` | `EAlwaysPass` (0xfd) | opcodes `0..10` pass; `11+` `KErrNotSupported` |
| `!SBEServer` | element 0 (capability check) | opcodes `0..20` element 0; `21+` not supported |

**Evidence [verified]**

- `!ABServer` `CPolicyServer::TPolicy` at VA `0x822446fc` in `sbengine.exe`:
  `iOnConnect=0xfd` (`EAlwaysPass`), `iRangeCount=2`, `iRanges=[0, 11]`,
  `iElementsIndex=[0xfd, 0xfe]` (pass / not-supported).
- `!SBEServer` policy at `0x82244760`: `iOnConnect=0`, `iRanges=[0, 21]`,
  `iElementsIndex=[0, 0xfe]`; element 0 is a real security policy, so this one is gated.
- `abclient.dll` (ROM, `Sys/Bin/abclient.dll`) is the Active Backup client:
  service literal `!ABServer`, starts `sbengine.exe` with `RProcess::Create` when the
  session cannot be created. Its 12 exports (`abclient_ord1..12`) send opcodes 0..10
  through `RSessionBase::SendReceive`; it also links `TSecurityPolicy` and
  `CreateSession`.
- `sbeclient.dll` (`Sys/Bin/sbeclient.dll`) is the *secure* client and uses
  `!SBEServer`, not `!ABServer`.

Consequences:

- An ordinary app can start `sbengine.exe` and create a session to `!ABServer` with no
  capability check at all [verified by policy; start path inferred from `abclient.dll`].
- The 11 opcodes are the Active Backup state machine. The client has 12 exports because
  constructors/destructors and the 11 operations are separate ordinals.
  Opcode 0 takes two integer args; 1 and 2 take one integer; 3..6 are asynchronous;
  9/10 are cancel/close-style [inferred from `abclient` disassembly].
- Server-side strings show it handles `?:\sys\*`, `?:\system\*`, `?:\resource\*`,
  `?:\private\*`, `*\..\*`, `backup_registration*.xml`,
  `\private\10202D56\import\packages\`, `C:\system\temp\MIDletRestore\` [verified strings].
  So this engine is designed to walk and restore system-wide file sets with `AllFiles`.

This is the cheapest high-value target: no privileged capability at the client, but the
server that does the file I/O has `AllFiles`.

## 3. `!InstallServer` is not directly reachable

`installserver.exe` (SID `0x101f7295`, caps `TCB`, `ReadDeviceData`,
`WriteDeviceData`, `TrustedUI`, `ProtServ`, `AllFiles`, `ReadUserData`) uses a
`CPolicyServer` with **custom check on connect and on every opcode**.

**Evidence [verified]**

- Policy at `0x8225f8b8`: `iOnConnect=0xff` (`ECustomCheck`), one range `[0]`,
  one element index `0xff` (custom check), no static elements.
- The custom check at `0x82253dae` compares `RMessagePtr2::SecureId()` with
  `0x10202d56` (`sbengine.exe`); if not equal it calls
  `DoHasCapability(ECapabilityTrustedUI, "CInstallServer::CustomSecurityTest client
  process does not have Trusted UI Capability")`.
- `swis.exe` alone lacks `TrustedUI`/`AllFiles`, but it loads `swiseng.dll`
  (caps include `TCB`, `AllFiles`, `TrustedUI`) and the UI process therefore has the
  privileged set [inferred from process/DLL capability union; the string and DLL caps
  are verified].
- `src/n95apps/devagent/sisinst.cpp` already calls `Swi::Launcher::Install` through
  `sishelper.dll`, answering every installer dialog; it is built with `All-TCB` because
  the launcher requires `TrustedUI`.

So do **not** spend the first weeks fuzzing `!InstallServer` from a basic-cap app: the
session never reaches the parser. The reachable installer surface is the **client-side
SIS parser and UI process** (`swiseng.dll`, `siscontroller.dll`, `sbengine`'s import
path), which run with the privileged set and parse attacker-supplied SIS content. That
is a separate, high-value target (malformed SIS, file-entry paths, upgrade/downgrade,
rollback, partial install), not an IPC cap-check bug.

## 4. F32 file server: reachable, but per-op policy is the wall

`efile.exe` (`!FileServer`, SID `0x100039e3`, caps `TCB`, `CommDD`, `PowerMgmt`,
`ProtServ`, `DiskAdmin`, `AllFiles`) serves `RFs`. Session creation is normally allowed;
each operation checks the **parsed full path** with `PathCheck`.

The Symbian^3 file-server source in this repo gives the policy grammar
[verified source; opcode numbers must be checked against the 9.2 `efile.exe` dispatch]:

| operation | system path | private path | resource path |
|---|---|---|---|
| `EFsMkDir` | `TCB` | `AllFiles` | `TCB` |
| `EFsRmDir` / `EFsDelete` | `TCB` | `AllFiles` | `TCB` |
| `EFsRename` / `EFsReplace` | `TCB` | `AllFiles` | `TCB` |
| `EFsFileCreate` / `FileReplace` / `FileTemp` | `TCB` | `AllFiles` | `TCB` |
| `EFsFileOpen` write | `TCB` | `AllFiles` | `TCB` |
| `EFsFileOpen` read | `AllFiles` | `AllFiles` | - |
| `EFsSetSessionPath` | `AllFiles` | `AllFiles` | - |
| `EFsSetSubst` | `TCB` | `AllFiles` | `TCB` |
| `EFsSessionToPrivate` / `EFsPrivatePath` / `EFsCreatePrivatePath` | no path cap (creates/uses the caller SID's private dir) | | |
| `EFsReadFileSection`, `EFsEntry`, `EFsIsFileOpen`, `EFsDirOpen` | `AllFiles` | `AllFiles` | - |
| `EFsAddFileSystem`, `Mount`, `Format`, `RawDisk*`, `Plugin*`, `CheckDisk`, `ScanDrive`, `LockDrive` | `DiskAdmin`/`TCB` | | |

`PathCheck` (`external/.../sfile/sf_utl.cpp`) tests `ComparePrivate` first, then
`SIDCheck` (hex SID in `\private\<8 hex>\`), then `CompareSystem` / `CompareResource`.
The whole check is on the **already parsed** `FullName().Mid(2)`. Therefore the attack
surface is not "call a privileged opcode" but "make the parsed path look private while
the filesystem resolves it to `\sys\bin`", or race the check with a name/subst change.
Test classes:

- `..`/`.` collapse, repeated separators, trailing dots/spaces, case folding,
  short/long names, drive-relative and colon forms, `\\?\`-like forms, embedded NULs,
  descriptor length vs content, overlong components;
- `\private\<own SID>\..\sys\bin\ssl.dll` and variants where `SIDCheck` sees the right
  prefix but the parser leaves `..`;
- session path / default path / subst changes between check and use;
- create/open a file in the private dir, then rename/replace/temp into a system path;
- notification/`ReadFileSection`/`Entry` paths that are checked with a different flag
  set (`aExactMatchAllowed`);
- races with `EFsNotifyChange`, `FileTemp`, `FileRename`, `SetSessionPath`.

## 5. Other reachable servers (for completeness)

Service names read from the stock binaries [verified strings], with the server's own
caps (not the client requirement):

| service | binary | SID | server caps (9.2 order) | note |
|---|---|---|---|---|
| `!FileServer` | `efile.exe` | `100039e3` | TCB, CommDD, PowerMgmt, ProtServ, DiskAdmin, AllFiles | F32, per-op path checks |
| `!ABServer` | `sbengine.exe` | `10202d56` | WriteDeviceData, ProtServ, AllFiles, ReadUserData | **open connect + opcodes 0..10** |
| `!SBEServer` | `sbengine.exe` | `10202d56` | same | capped connect |
| `!SisRegistryServer` | `sisregistryserver.exe` | `10202dca` | TCB, ProtServ, AllFiles | policy has 7 ranges; connect `EAlwaysPass`, but most writes are gated |
| `!CentralRepository` | `centralrepositorysrv.exe` | `10202be9` | ProtServ | settings; not a file writer |
| `!ecomserver` | `EComServer.exe` | `10009d8f` | ProtServ | plugin instantiation |
| `!FeatMgrServer` | `FeatMgrServer.exe` | `10205054` | ReadDeviceData, WriteDeviceData, ProtServ, SwEvent, ... | feature flags |
| `!HWRMServer` | `HWRMServer.exe` | `101f7a02` | ReadDeviceData, WriteDeviceData, ProtServ, SwEvent, ... | hardware resources |
| `BackupSrv` | `eikbackupsrv.dll` in `EikSrvs.exe` | `10004904` / `10003a4a` | TrustedUI, ProtServ | separate S60 backup server |
| `!SisLauncherServer` | `sislauncherserver.exe` | `1020473f` | PowerMgmt, ProtServ | launcher |
| `!InstallServer` | `installserver.exe` | `101f7295` | TCB, AllFiles, TrustedUI, ProtServ, ... | **custom TrustedUI gate** |

`eikbackupsrv.dll` / `EikSrvs.exe` are the other backup path (`BackupSrv`); they are
capped (`TrustedUI`, `ProtServ`) and are a lower priority than the open `!ABServer`.

## 6. Approach

### Phase 0: a capability probe app (1 day)

Build `src/n95apps/capask` (same toolchain as `dsbtest`/`devagent`) with **no caps**
(or only `NetworkServices`/`ReadUserData`/`WriteUserData`), SID a fresh test UID.
It:

1. tries `RSessionBase::CreateSession` for each service above, logs the result;
2. for open services (`!ABServer`, `!FileServer`, `!SisRegistryServer`,
   `!CentralRepository`, ...) calls opcodes `0..N` with benign, then malformed,
   `TIpcArgs` (null/4-byte/descriptor args, wrong lengths), logs return code and
   whether the process panicked;
3. for `!ABServer`, also links/calls `abclient` ordinals if the loader allows the DLL;
   otherwise raw `SendReceive` to the same opcodes;
4. writes a `backup_registration.xml` into its own private dir and checks whether
   `!ABServer` reads it (log via `sydb strace` / file access tracing).

Run on QEMU first (`sydb` + `sydb install`/`fs_push`), then on the physical N95 with a
self-signed SIS. The QEMU kernel is adapted, so a pass/fail is a hint; the stock phone
is the oracle.

### Phase 1: reverse the open surface (2-3 days)

- `abclient.dll`: map each export to opcode and argument layout; the 12 ordinals are
  stripped, so use call sites and the `SendReceive` immediates.
- `sbengine.exe`: find the `!ABServer` session `ServiceL` (vtable / switch on
  `RMessage2::Function()`), map opcodes 0..10 to methods and side effects.
- `backup_registration.xml` parser: find the element names (`public_backup`,
  `include_file`, `include_directory`, `exclude`, `backup_registration`,
  `system_backup`), the path checks (if any), and whether the app's own registration
  file can name `\sys\bin\...`.
- `sbencrypt.dll`: is the backup archive authenticated? If not, a forged archive is the
  write primitive; if yes, the registration-file path is still interesting.

### Phase 2: exploit hypotheses, in order

1. **Registration-file injection.** An app writes its own
   `\private\<SID>\backup_registration.xml` with an `include_file`/`include_directory`
   entry for `\sys\bin\ssl.dll` (or a restore destination there), triggers a backup and
   a restore through `!ABServer`, and observes whether the engine reads/writes the
   system path with its `AllFiles` privilege. This is the first thing to test because
   the app already controls that file and the server already trusts it.
2. **Archive forgery / path traversal.** If the backup archive is a file the app can
   reach, put a `..\..\sys\bin\ssl.dll` entry in it and restore. Check `sbencrypt` and
   the archive header first.
3. **State-machine abuse.** Opcodes 0..10 are a sequence; try restore without a valid
   backup, backup/restore interleaving, cancel at the wrong point, repeated
   start/stop, and app termination mid-operation. The server may leave a temp file or
   restore a partial set.
4. **F32 path parsing** as the fallback (section 4), because it is the direct "write a
   file" API and the source is available.

### Phase 3: fuzzing, but only after the map

Blind IPC fuzzing is expensive and mostly finds `KErrArgument`. Do it with:

- **Grammar**: F32 opcode/arg table (`sf_ops.h`), SIS/backup file formats, and the
  `abclient` opcode sequence. Mutate valid traces, not random bytes.
- **Coverage**: QEMU TCG edge coverage per server process/DLL (a small plugin or
  `-d exec` post-processing), reset via `sydb snap`/`fork`.
- **Stateful**: keep a session and mutate sequences; security bugs here are state
  machines (session path, subst, open handle, backup phase).
- **Invariant, not crash**: after each case snapshot `C:`/`Z:` file hashes, process
  capabilities, loaded DLL list, and the target path; success is "wrote
  `c:\sys\bin\ssl.dll`" or "read a protected file", not a panic.
- **Differential**: run the same corpus on QEMU and the physical N95; only trust
  findings that reproduce on stock. The QEMU kernel/personality is not the stock one.

## 7. Why this ordering

- `!ABServer` is the only service found so far with **both** an unauthenticated connect
  and a privileged file-writing implementation. That combination is the bug class we
  want; it is worth more than a thousand random `!InstallServer` session attempts that
  die at `TrustedUI`.
- `!InstallServer` is protected by a deliberate `CustomSecurityTest`; treat it as a
  parser/UI target, not an IPC target.
- F32 is hardened but is the direct write API and has a large legacy parser; it is the
  best fallback if the backup path is a dead end.

## 8. Open questions

- Does `!ABServer` actually run on a stock N95 at boot, or only when `abclient` starts
  it? (The client starts `sbengine.exe` explicitly.)
- Can a basic-cap app load `abclient.dll` from ROM, or must it use raw IPC? Either way
  the server policy is the same.
- What exactly do `!ABServer` opcodes 0..10 do, and which of them take a path or a
  file list? (Phase 1.)
- Is `backup_registration.xml` trusted for arbitrary paths, and is it read from the
  app's private dir? (Phase 1/2.)
- Is the backup archive signed/encrypted such that an app cannot forge one?
- Does the loader prefer `c:\sys\bin\ssl.dll` over `z:` on 9.2? (Separate from this
  exploit; `docs/research/tls-modern.md`.)

## 9. Related files

- `docs/research/tls-modern.md` — why we want to replace `ssl.dll`.
- `docs/devices/nokia-n95-8gb-rm320/n95-firmware-inventory.md` — stock core provenance.
- `src/n95apps/devagent/sisinst.cpp` — installer UI harness (All-TCB).
- `src/n95apps/devagent/build.sh` — app build/elf2e32 recipe.
- `external/oss.FCL.sf.os.kernelhwsrv/userlibandfileserver/fileserver/` — F32 grammar and
  policy table (Symbian^3 source; use as reference, verify opcodes on 9.2).
- `tmp/n95api/` — extracted ELFs and disassembly used for the offsets above.
