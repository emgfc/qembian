# Personality symbian3: Symbian^3 user sides on our EPL kernel

Used by devices with `platform.personality = "symbian3"`. First device: `models/nokia-n8-rm596`
(RM-596 011.012). Registry number 4 (`personalities/registry.toml`). Kernel side:
`patches/0015-symbian3-personality.patch`.

| part | what | evidence |
|---|---|---|
| exec numbering | ours: `exec.map` is the identity of `kernel/eka/kernel/execs.txt` (fast 44, slow 270; the N8 EUSER uses slow numbers up to 265) | [V] `probe.py stubs`: every kernel call identified by export has our number |
| argument conventions | `RMutex::Wait()` (ord 922) is `ldr r0,[r0]; blx MutexWait`: no timeout, `r1` is whatever the caller left. Our kernel reads `r1` as the timeout (< -1 returns `KErrArgument` without taking the mutex), so S11 goes through the 9.2 shim `MutexWait92` (timeout 0). There is no `RMutex::Wait(TInt)` export. The other 9.2-era conventions are gone: `RTimer::At` passes the time by value, `RHandleBase::Duplicate` passes `this`, `User::SafeInc` uses atomics | [V] N8 EUser exports 922, 926, 121, 658 disassembled against the 5800's |
| loader structures | ours (`TProcessCreateInfo` with `iFlags`) | [V] processes start with the native layout |

Symptom without it: `tzserver.exe::Main` KERN-EXEC 1 (`EAccessDenied` from `ExecHandler::MutexSignal`:
the thread signals `TZ_GlobalMutex`, whose Wait returned `KErrArgument` because of a garbage `r1`), then
sysstatemgr CleSrv STDLIBS-INIT 1 and a start-up loop. Found with a `sydb catch add --at` rule on the
access-denied branch of `ExecHandler::MutexSignal`.
