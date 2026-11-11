# Personality sym101: Symbian OS 10.1 (Nokia Belle) user sides on our EPL kernel

Used by devices with `platform.personality = "sym101"`. First device: `models/nokia-n8-rm596/firmware/111.040.1511`
(RM-596 111.040.1511, Belle Refresh). Registry number 6 (`personalities/registry.toml`). Kernel side:
`patches/0020-sym101-personality.patch`.

| part | what | evidence |
|---|---|---|
| exec numbering | the Symbian^3 one (`personalities/symbian3/exec.map`, with its 9.2 `RMutex::Wait()` shim) up to S10a | [V] `tools/swi_map.py` report of the Belle EUser: every call identified by export has the Symbian^3 number |
| S10b..S10d | new calls on every process start and library load: S10b at the end of `User::InitProcess()` (no arguments, result ignored), S10c before each DLL's ProcessAttach entry call (r0 = entry point; non-zero skips the call), S10d first thing in `RLibrary::Load` (non-zero fails the load). Our kernel's own S10b..S10d are debug calls, so the numbers go to the shim `Ok101` (returns KErrNone) | [V] Belle EUser disassembled (`tools/xipdis.py`); meaning [I] |
| S10e..S114 | one export each (ords 2547..2553), purpose unknown: unmapped, logged (`EXEC92: unmapped ... slow exec`), KErrNotSupported | [V] numbers, [I] that nothing at boot needs them |
| loader structures | ours, as on Symbian^3 | [I] |
| user stack top | Belle's TLocalThreadData: one more word at +12, the thread's own ID (`RThread::Id()` of the current thread reads it, no kernel call); the DLL TLS array (now user side) at +20; 0x34 bytes | [V] EUser ords 1793, DllSetTls disassembled; the kernel reserves 0x38 and stores the ID |
| nested library loads | Belle's GLES forwarders load khclient_vc3.dll from a static constructor, i.e. inside another library's attach; our kernel would initialise the outer library's DLL data again | [V] KDLL trace of sysstatemgr; KernNestedAttachCountsAsLoaded |
