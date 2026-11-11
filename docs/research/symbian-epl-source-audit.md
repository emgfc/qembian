# EPL Symbian sources on this machine: what is available and what the installer guards look like

Research note, 2026-11-10. This is a source-level audit, not a claim about the N95 9.2
binaries. The source trees are Symbian^3-era EPL drops, while the phone is S60 3rd FP1
(Symbian 9.2). Treat them as architectural reference and as a list of guards to look for in
the 9.2 binaries. A guard present here may be absent or different in Nokia's 9.2 code.

## Where the trees are

`~/work/symbian` contains 55 EPL trees (~5.3 GB), including:

| Tree | Relevant contents |
|---|---|
| `oss.FCL.sf.mw.appinstall` | installer (`installationservices/swi`), SIS parser, SIS registry, launcher, backup/restore machine, SIF framework |
| `oss.FCL.sf.mw.securitysrv` | certificate stores, security dialogs, boot-time integrity, key store, WIM |
| `oss.FCL.sf.os.ossrv` | OS libraries and utility services; **not** the kernel/file server itself |
| `oss.FCL.sf.os.security` | security server components |
| `oss.FCL.sf.mw.accesssec` | WLAN/EAP access security |
| `oss.FCL.sf.mw.networkingsrv`, `netprotocols`, `websrv` | network service surfaces |
| `oss.FCL.sf.os.persistentdata` | Central Repository, P&S, DBMS |
| `oss.FCL.sf.os.devicesrv`, `cellularsrv`, `commsfw` | device and comms services |

The appinstall tree is the useful one for the current target. It is not a Nokia S60
tree; it is the Symbian Foundation implementation, with later S60/Qt additions.

## Installer guards visible in the EPL source

The source matches the observed 9.2 policy decision:

- `!InstallServer` uses a custom connect check: SID `0x10202D56` (SBE) passes; otherwise
  the client needs `TrustedUI`. `EListUnInstalledPkgs` additionally needs `ReadDeviceData`,
  and `ERemoveUnInstalledPkg` needs `WriteDeviceData`.
  - `installationservices/swi/source/swis/server/installserver.cpp:445-480`

The file-level installer is more interesting than the connect check:

- **Path shape checks** — `SecurityCheckUtil::CheckFileName()`
  (`securitycheckutil.cpp:154`):
  - full drive and path (`x:\...`), no `\\`, no `..\`
  - rejects the writable `x:\Resource\SwiCertstore` path
  - for paths containing `:\sys\bin`, rejects non-ASCII characters after the last `\`
  - this is lexical, not F32 canonicalisation
- **Protected-directory checks** — `CheckProtectedDirectoriesL()`
  (`securitycheckutil.cpp:220`):
  - `\private\<SID>\...` must be owned by one of the package SIDs, or be another
    package's `\private\<SID>\import\...`
  - under `\sys`, only `\sys\bin\` is writable, and only for operations other than
    `EOpNull` (delete)
  - under `\resource`, deletion (`EOpNull`) is rejected; writes are otherwise handled
    by the TCB requirement
- **Executable checks** — `ProcessFileL()` (`securitycheckutil.cpp:453`):
  - SIS header capabilities must exactly match the E32 header capabilities
    (`CompareHeaderAndExeCapabilities`, line 91)
  - VID/SID certificate constraints are checked
  - untrusted packages cannot install protected VIDs, or EXEs with protected SIDs
  - an EXE SID already owned by another package is rejected, except for a valid
    SU/system-upgrade package or a same-name replacement
  - non-EXE/non-DLL files are rejected from `\sys\bin`
- **ROM eclipsing** — `ValidEclipseL()` (`installationplanner.cpp:1747`) and
  `IsEclipsableL()` (`installationplanner.cpp:1707`):
  - the planner builds `iEclipsableRomFiles` only through
    `PrepareEclipsableFilesListL()` (`installationplanner.cpp:1444`)
  - that list is populated only when `IsValidEclipsingUpgradeL()`
    (`installationplanner.cpp:361`) passes
  - `IsValidEclipsingUpgradeL()` requires the **same package UID** as the installed
    registry entry, and `TrustStatus().ValidationStatus() >= EValidatedToAnchor`
  - therefore a normal self-signed SIS cannot eclipse a ROM file; a system-upgrade
    certificate is needed for that path
  - `ValidEclipseL()` also rejects installation if the target exists on `z:` and is
    not in the eclipsable list (`KErrInvalidEclipsing`)
- **Subst check** — `IsSubstedDriveL()` (`securitycheckutil.cpp:659`) is called by the
  planner and restore machine, so a substituted drive is rejected before the target is
  used.

This is consistent with the earlier 9.2 observation that an unsigned SIS requesting
system capabilities fails at install time.

## Source-level asymmetry worth following: restore vs install

The strongest source-level asymmetry is in the backup/restore path, not in ordinary
install:

- `CRestoreMachine::IsUniqueL()` (`restoremachine.cpp:279`) explicitly **skips the Z:
  drive**:
  - comment: "eclipsing allowed, checked later"
  - the target is then hash-checked against the restore controller description
- `CRestoreProcessor::DoStateVerifyPathsL()` (`restoreprocessor.cpp:691`) only calls
  `CheckProtectedDirectoriesL()`; it does not call `ValidEclipseL()`.
- `SecurityCheckUtil::ProcessFileL()` is also called for restored files, so the
  EXE/DLL capability and SID checks apply, but there is **no ROM-eclipsing
  authorization** in this path.
- `CRestoreController::FindFileDescriptionL()` (`restorecontroller.cpp:545`) matches the
  incoming file against a file description in the restore controller and checks the
  hash from that same controller.

So the restore path's security argument is: the restore controller is trusted, and the
file must match the controller's hash. If an attacker can supply both the restore
stream and its controller, the hash check is not an authorization check. This is
consistent with the SBE restore route already mapped: restore can write `\sys\bin`
and relies on the controller/hash, not on the installer's ROM-eclipsing rules.

This is **not** a no-capability path by itself. `!InstallServer` is protected by
TrustedUI/SBE SID, and `!SBEServer` by its capability policy. It becomes useful only if
some trusted client forwards attacker-controlled restore metadata to either service.

The obvious next question is therefore the same one as before, but now source-backed:
**who can call `CRestoreMachine`/`ERestoreFile` or the SBE restore stream with data an
ordinary app controls?**

## Follow-up results (same day)

### Lexical `..` in the file server: closed

The promising file-server asymmetry is not exploitable. `TDrive::FileOpenL()`
(`sfile/sf_drv.cpp`) calls `IsIllegalFullName(aRequest->Src())` before any create/open
work, and `TNameChecker::IsIllegalPath()` rejects `.`, `..`, double backslashes,
wildcards and trailing dots in every path component. The same validation is in
`TDrive::Rename()`, `TDrive::Replace()` and the directory/delete paths. The
`PathCheck()` calls in `sf_file.cpp` are therefore not a missing guard; they run before
`FileOpenL()`, but the later `IsIllegalFullName()` still rejects a canonicalisation
trick. The `TFsFileOpen` early return for `IsPluginRequest()`
(`sf_file.cpp:414`) is likewise not client-controlled: `iOwnerPlugin` is set only by
the plugin shim (`sf_plugin_shim.cpp:733`) when a file-server plugin originates the
request, not by an ordinary `RFs`/`RFile` client.

This leaves only the **installer-vs-F32** lexical gap, not a general `..` traversal.

### Installer ROM-stub upgrade: source says no for a self-signed SIS

The ROM-upgrade path is real in the code but is **not** reachable without trust:

- `IsValidEclipsingUpgradeL()` (`installationplanner.cpp:361`) first requires the SAME
  package UID as the registry entry and
  `CurrentController().TrustStatus().ValidationStatus() >= EValidatedToAnchor`.
  A self-signed controller fails this before the `isBaseInRom` exception is reached.
- `IsValidSaUpgradeL()` (`installationplanner.cpp:332`) would permit a non-SU SA with
  the RU flag over a ROM base, but it is only consulted after the eclipse-upgrade
  trust check above.
- `IsSufficientlyTrusted()` has an `EPackageInRom` exception, but it is a later check;
  it does not bypass the earlier `EValidatedToAnchor` requirement.
- Therefore `PrepareEclipsableFilesListL()` stays empty for an untrusted package and
  `ValidEclipseL()` rejects `z:\sys\bin\ssl.dll` with `KErrInvalidEclipsing`.

This matches the intended model: a ROM stub can be upgraded only by a controller
validated to the anchor, not merely by one that names the stub's package UID.

### N95 ROM-stub inventory

I extracted the 41 `system\Install\*.sis` ROM stubs from `rofs1.img` and the extra
ROFS2 stubs. Their SIS package UIDs are almost all in the protected range
(`0x100...`, `0x102...`, `0x200...`; bit 31 clear). The only unprotected stub is
`Series60SkinsSupport.SIS`, UID `0xa00000eb`, and it contains no `sys\bin` files at
all (it is a dependency/support stub). No ROM stub lists `ssl.dll`, and no stub
contains a wildcard under `sys\bin`. So even if the trust check were weaker, there is
no ROM-stub file set on this firmware that includes `ssl.dll`.

### SBE import registration

`CDataOwnerManager::FindImportPackagesL()` scans
`\private\10202D56\import\packages\` for `backup_registration*.xml`, and the installer's
`CheckProtectedDirectoriesL()` deliberately permits writes under any
`\private\<SID>\import\`. That means a signed SIS can plant a registration file in
SBE's import directory, but it is parsed only when `!SBEServer` is used; the policy for
that server still requires `ReadDeviceData + WriteDeviceData`. It is not a no-cap read
or write primitive by itself.

### `!ABServer` callbacks: no length overflow found

The apparent unchecked
`iMessage.GetDesLengthL(3) -> aBuffer.SetLength(bufLength) -> iMessage.ReadL(3, aBuffer)`
in `absession.cpp` is not a heap overflow in release code. `RMessagePtr2::Read()` uses
`aDes.MaxLength()` for the IPC copy (`us_ksvr.cpp:1974`) and then resets the length to
the number of bytes copied, so an over-long client descriptor is clamped. The path is
still worth a malformed-shape fuzz for panics/DoS, not for memory corruption. More
importantly, the file-I/O half of SBE (`CDataOwner`) is driven only while
`CDataOwnerManager::iBURType` is a backup/restore mode, and only `!SBEServer` sets that
mode.

### Installer trust on stock 9.2

An unsigned SIS is not a valid test of the self-signed threat model. On the stock RM-320
session, `sydb sis install fuzz/out/capask.sis` reaches the installer but ends with
`-10254` security error because the package has no signature. A real test needs a SIS
signed by a self-generated certificate (`signsis`/`makekeys`), which the SDK's Windows
binaries can produce but the Linux `makesis -c` path here does not. Until that baseline
is established, no installer result on this phone can be attributed to the package's
authorization versus the missing signature.

## Remaining hypotheses to test against 9.2

These are now narrower:

1. **Trailing-space canonicalisation** — installer `CheckFileName()` rejects `..\` and
   trailing components ending in `.`, but not a trailing space. F32 source-side
   `IsIllegalName()` also allows `name ` (only all-space names are rejected). If 9.2
   F32 or the loader collapses `ssl.dll ` to `ssl.dll`, a target that the installer
   sees as a different name could be loaded as `ssl.dll`. Test with a benign name
   first, then the real target.
2. **Private-SID prefix parsing** — `CheckProtectedDirectoriesL()` uses
   `TLex::Val(sid, EHex)` and does not visibly require the lexer to consume exactly
   eight characters. A path with a valid SID prefix plus a suffix may be treated as
   caller-owned. It cannot reach `\sys` without `..` (now closed), but it can affect
   which `\private` directory is allowed. Test on 9.2.
3. **DLL SID uniqueness** — `ProcessFileL()` enforces SID ownership only for EXEs; DLLs
   get capability/VID checks but no SID-uniqueness check. A new DLL under a name that
   does not collide with ROM is allowed; the loader's `CheckSecInfo()` then enforces the
   importing process's capability set. This is a design boundary, not a demonstrated
   `ssl.dll` route.
4. **Check/use gap in install** — `VerifyInstallPathL()` and the planner check precede
   `InstallFileL()`'s final `Rename`. The final rename is validated by F32's
   `IsIllegalFullName()`, but not re-checked against the installer's protected-directory
   or eclipse policy. Exploiting this needs a privileged actor to change the path or
   drive mapping between the two points; lower priority.

## Practical plan

1. Build and sign a self-signed baseline SIS with `signsis` (a normal EXE under
   `\sys\bin`, no system caps) and confirm 9.2 accepts it after the security warning.
   Only then test a DLL named `ssl.dll` and capture the exact planner error.
2. Test trailing-space and private-SID-prefix path forms with signed packages; compare
   the installer's decision with F32's actual file that results.
3. Compare the 9.2 binaries (`installserver.exe`, `SWInstSvrUI`, `siscontroller`,
   `swiseng`, `sisregistryserver`) for the source guards above; the 9.2 code is older
   than these EPL drops and may differ.
4. Keep the restore path queued: `CRestoreProcessor` skips `ValidEclipseL()`, but every
   known entry point is still capability-gated (`!InstallServer` TrustedUI/SBE SID,
   `!SBEServer` ReadDeviceData+WriteDeviceData). Finding a trusted forwarder is the
   remaining problem, not the hash/controller logic.
5. Treat every source result as a hypothesis until reproduced on the N95 firmware or a
   seeded QEMU session; the file server and loader are not in these EPL trees.
