# Personality sym91: Symbian 9.1 user sides on our EPL kernel

Used by devices with `platform.personality = "sym91"` (S60 3rd Edition, no feature pack: N73, N80,
N93, E60, E61, 3250, 5500...). First device: `devices/nokia-n73-rm132`. These phones also use the
**moving memory model** on ARMv5 (ROM at 0xF8000000, fixed processes), so they run on the moving-model
kernel build (`kernel.baseport = "syborgmv"`, `tools/baseport_variant.py`), not on the N95's.
Registry number 5 (`personalities/registry.toml`).

| part | file | what | evidence |
|---|---|---|---|
| exec numbering | `exec.map` | 9.2's map (`personalities/sym92/exec.map`) cut to F00..F1A / S00..SE5: the ordered SWI stub sequences of the N73 and N95 EUser.dll are identical except for what 9.2 appended (F1B, SE6..SED) | [V] stub-order diff; export pairing (`tools/personality/derive_map.py`) agrees on 117 calls |
| TLS calls | `exec.map` S4D, S75 | `UserSvr::DllTls(handle)` and `UserSvr::DllSetTls(handle, ptr)` branch straight to the exec stubs; 9.2 inserts a DLL uid argument (0), ours needs it: shims `DllTls91`/`DllSetTls91` | [V] both EUser.dll disassembled; without the shims the RVCT runtime's exception globals were NULL and the first leave crashed |
| SWI return | `patches/0012` (vectors.cia) | 9.1 EUSER stubs are `mov ip, lr; swi n` with no return instruction; the N73's own kernel saves `{fp, ip}` and returns to user mode through a `bx ip` trampoline. For user-mode SWIs our kernel returns to ip (Thumb state if odd) | [V] N73 ekern SWI vector; 256 of 256 EUSER SWI sites set ip (253 stubs, 3 inline `mov ip, pc`) |
| loader structures | `patches/0012` (sexecpers.cpp, scodeseg.cpp) | TCodeSegCreateInfo 380 bytes (ends at iClientProcessHandle; no demand-paging fields), TProcessCreateInfo 416 (= 380 + 36): copied in and out field-group by field-group | [V] copy-in sizes in the N73 ekern's exec handlers (its slow exec table at 0xf8014a2c) |
| driver ABI | none | the phone's own kernel-side drivers are not used (stand-ins are built against our kernel) | |

The kernel carries every personality's tables (`tools/personality/gen_exec_map.py` writes
`kernel/execpers_map.h` with both) and picks one at boot from the ROM header (`Exec92Init`).

## How this was derived (repeat for the next version)

1. `tools/swi_map.py --rom-image core-flat.img --at <EUser.dll> --names <SDK euser.dso>` for the new
   and the known firmware; `derive_map.py NEW KNOWN personalities/KNOWN/exec.map` for a draft.
2. Diff the **ordered** stub sequences of the two EUser.dll (stubs are generated in table order) —
   the decisive check; export pairing is noisy where an export's SWI list is a call-graph closure.
3. Look at the stub *shape* too (`mov ip, lr; swi` vs `swi; bx lr`): it tells the return convention.
4. Find the phone's own slow exec table in its ekern (a word = count, then {flags, handler} pairs
   into ekern's code) and read the copy-in sizes of CodeSegCreate (0x8C), ProcessCreate (0x96) etc.
5. Compare argument use of exports that wrap exec calls directly (`b <stub>` without setting r1..r3).
