# Personality sym92: Symbian OS 9.2 user sides and drivers on our EPL kernel

Used by devices with `platform.personality = "sym92"` (S60 3rd Edition FP1: N95, N95 8GB, N82, E51,
E65, 6120 classic...). Our kernel is built from the later EPL sources; this personality makes it look
like the 9.2 kernel to the phone's own binaries.

| part | file | what |
|---|---|---|
| exec numbering | `exec.map` | 9.2 SWI numbers -> our exec handlers (+ shims); compiled into the kernel by `tools/personality/gen_exec_map.py` (kernel/exec92_map.h). Derived with `align.py` / `map-draft.txt` from the device's EUSER (docs/n95-swi-map.md) |
| exec patches | `patches/0001`, `0005` | the 9.2 exec tables in the kernel, TimerAt by reference |
| driver ABI | `patches/0008`, `0009`, `kabi.toml` | DObject/DLogicalChannel vtables equal to 9.2's; no user memory guards (H-kernel-abi-92) |

Verify a device against it: `tools/personality/check.py --device <id>` (driver vtables from the
device's own ekern; exit 1 on any difference `kabi.toml` does not allow).

## Deriving another personality (sym93 for S60 3rd FP2 / 9.3, sym94 for 5th Edition, ...)

1. Exec numbering: take the device's EUSER from its firmware (`tools/fw` inventory), align its SWI
   stubs against ours (`personalities/sym92/align.py` is the 9.2 version; `tools/swi_map.py --compare`),
   write `personalities/sym9x/exec.map`, add shims where the ABI of a call changed.
2. Driver ABI: copy `kabi.toml`, run `tools/personality/kabi.py` against the device's ekern, and change
   our kernel until only listed differences remain (each change a patch in `personalities/sym9x/patches`).
3. Point the device profile at it (`platform.personality`), build, smoke (`tests/device-smoke.sh ID`).
A kernel build carries one personality's tables today (docs/devices.md, "personalities").
