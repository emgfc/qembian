# glsec: hostile tests of the graphics boundary

`glsec.exe` is an ordinary app (no capabilities) that attacks the syborg-gl device the way a hostile process
could: user writes to the kernel's registers, read-backs into kernel memory and read-only code, command words
from unmapped and kernel pages, a forged owner (user kick, ctl 0x7f12, ctl 0x7f11), OWNER_FREE of another
process and of everyone, COMPOSE and SCANOUT_FREE without the compositor right, and (with a second instance
of itself) a present into a GCE surface id another process presented. Plan and findings:
`docs/gpu-boundary-roadmap.md`, `docs/gpu-boundary-review-2026-11-10.md`.

```
tools/probes/glsec/build.sh                                   # -> build/n95apps/glsec.exe
SYBORG_DEVICE=nokia-n8-rm596 python3 tools/sydb/sydb.py --session build/sydb-glsec start --seed build/seeds/nokia-n8-rm596
python3 tools/sydb/sydb.py --session build/sydb-glsec push build/n95apps/glsec.exe 'C:\sys\bin\glsec.exe'
python3 tools/sydb/sydb.py --session build/sydb-glsec run 'C:\sys\bin\glsec.exe' --wait 10
grep -a GLSEC build/sydb-glsec/serial.log          # one PASS/FAIL line per test, then "done, N failed"
grep -a refused build/sydb-glsec/qemu.err          # the device's reason for each refusal
```

The probe leaves one GCE scan-out slot claimed (its test surface id) until the machine resets.
