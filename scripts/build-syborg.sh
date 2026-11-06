#!/usr/bin/env bash
# Build the Syborg baseport (sf/adapt/qemu/baseport/syborg) and, via its base/rom
# extension, the text-shell ROM epoc32/rom/syborg_tshell_ARMV5_<var>.img.
#
#   scripts/build-syborg.sh [udeb|urel] [extra sbs args...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  # host side: the keyboard table of the device (tools/device.py gen keymap-header; $SYBORG_DEVICE or
  # devices/default), included by syborg_keyboard.h (patch qemu-baseport 0005)
  python3 -I "$ROOT/tools/device.py" gen keymap-header > \
    "$ROOT/build/epocroot/sf/adapt/qemu/baseport/syborg/keyboard/syborg_keyboard_map.h"
  exec "$ROOT/scripts/in-container.sh" /work/scripts/build-syborg.sh "$@"
fi
source /work/scripts/env.sh
VAR="${1:-udeb}"; shift || true
CFG="arm.v5.${VAR}.gcce4_4_1.surge"

echo ">> syborg baseport + ROM [$CFG]"
( cd "${EPOCROOT}sf/adapt/qemu/baseport/syborg" && sbs -c "$CFG" -j "$(nproc)" -f "$LOGS/syborg-$VAR.log" "$@" ) || true
grep -E "^sbs : (errors|warnings)" "$LOGS/syborg-$VAR.log" 2>/dev/null || true
IMG="${EPOCROOT}epoc32/rom/syborg_tshell_ARMV5_${VAR}.img"
if [ -f "$IMG" ]; then ls -la "$IMG"; else echo "ROM image NOT produced: $IMG"; exit 1; fi
