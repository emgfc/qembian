#!/usr/bin/env bash
# Rebuild the Syborg text-shell ROM with a different kernel trace mask, for diagnostics.
# Reuses the preprocessed .oby produced by build-syborg.sh and runs the same rombuild.
#
#   scripts/build-rom-trace.sh [udeb|urel] [MASK]
#     MASK default 0x80000003 = KPANIC|KBOOT|KHARDWARE (see kernel/eka/include/nkern/nk_trace.h:
#       bit0 KHARDWARE, bit1 KBOOT, bit5 KSCHED, bit6 KPROC, bit7 KEXEC, bit10 KDLL, bit31 KPANIC)
#     Kernel trace is only compiled into udeb kernels.
# Output: build/rom-trace/syborg_tshell_ARMV5_<var>_trace<MASK>.img
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  exec "$ROOT/scripts/in-container.sh" /work/scripts/build-rom-trace.sh "$@"
fi
source /work/scripts/env.sh
VAR="${1:-udeb}"; MASK="${2:-0x80000003}"
OBY="${EPOCROOT}epoc32/rom/syborg/syborg_tshell_ARMV5_${VAR}_rom.oby"
[ -f "$OBY" ] || { echo "run scripts/build-syborg.sh $VAR first ($OBY missing)" >&2; exit 1; }
OUT=/work/build/rom-trace; mkdir -p "$OUT"; cd "$OUT"
NAME="syborg_tshell_ARMV5_${VAR}_trace${MASK}"
sed -e "s/^kerneltrace .*/kerneltrace $MASK/" -e "s/^romname=.*/romname=$NAME.img/" "$OBY" > "$NAME.oby"
grep -q "^romname=" "$NAME.oby" || sed -i "1i romname=$NAME.img" "$NAME.oby"
rombuild "$NAME.oby" > "$NAME.rombuild.log" 2>&1 || { tail -20 "$NAME.rombuild.log"; exit 1; }
ls -la "$OUT/$NAME.img"
