# Personality sym94: Symbian 9.4 (S60 5th Edition) user sides on our EPL kernel

Used by devices with `platform.personality = "sym94"`. First device: `models/nokia-5800-rm356`.
Registry number 3 (`personalities/registry.toml`). Kernel side: `patches/0014-sym94-personality.patch`.

| part | what | evidence |
|---|---|---|
| exec numbering | `exec.map`: sym92 through the N79 (9.3 = 9.2 numbering) with `tools/personality/derive_map.py`. 9.4 inserts **S45** (`TimeNowSecure`, used by `TTime::HomeTimeSecure`/`UniversalTimeSecure`) after `TimeNow`, so every later slow call is +1, and appends **SEE..SF2** (`SetWin32RuntimeHook`, `GetBTraceId`, `NotifyOnIdle`, `CancelMiscNotifier`, `NotifyObjectDestruction`), all of which our kernel has (ours S45, SF0..SF4) | [V] `probe.py stubs --vs n79`; the six by pairing the same EUSER exports with our own EUSER |
| argument conventions | the 9.2 shims stay: `RMutex::Wait()` passes no timeout, `RTimer::At` passes `const TTime&`, `RHandleBase::Duplicate` passes the handle by value, SafeInc32/SafeDec32 still used | [V] 5800 EUser exports 922, 926, 121 disassembled |
| loader structures | TCodeSegCreateInfo as 9.2 (= ours); TProcessCreateInfo ends at `iDestructStat` (9.4 kernel copies 0x208 bytes, 9.2/9.3 0x200; ours adds `iFlags`) | [V] the 5800 and N79 ekern ProcessCreate handlers |
| TChunkCreate | 0x18 bytes with `iClearByte`, as ours (9.2/9.3: 0x14) | [V] exec-sizes ChunkCreate |

How to repeat for the next version: `personalities/sym91/README.md` (method), plus `tools/fw/probe.py
--device ID stubs --vs <known>` and `exec-sizes --vs <known>`; read the copy-in immediates of the handlers
that differ in their kernel (the scan only understands RVCT code, so compare two firmware kernels, not ours).
