#!/usr/bin/env bash
# Option 1 loop: (re)build our Syborg kernel side, compose it with the N95 user side
# (9.2 exec personality on), boot headless and summarise serial output.
#   scripts/hybrid-boot.sh [--no-build|--compose-only] [--secs N] [--trace MASK] [--out DIR] [--type CMD]...
# Needs the device's core-flat.img (tools/fw/inventory.py); device: $SYBORG_DEVICE or the registry default.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
BUILD=1; COMPOSE_ONLY=0; SECS=60; TRACE=0x80000000; OUT=build/boot-test/hybrid; TYPE=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --compose-only) BUILD=0; COMPOSE_ONLY=1 ;;
    --secs) SECS="$2"; shift ;;
    --trace) TRACE="$2"; shift ;;
    --out) OUT="$2"; shift ;;
    --type) TYPE+=(--type "$2"); shift ;;
    *) echo "unknown arg $1" >&2; exit 2 ;;
  esac; shift
done
# the device ROM from its profile (tools/device.py build: exec map, stand-ins, romcompose, DTB);
# HYBRID_VARIANT=NAME composes a [rom.variants] entry instead (compose-exp.sh: audio-exp)
DB=(python3 -I tools/device.py build)
[ $BUILD = 1 ] && DB+=(--kernel)
[ -n "${HYBRID_VARIANT:-}" ] && DB+=(--variant "$HYBRID_VARIANT")
[ "$TRACE" != 0x80000000 ] && DB+=(--trace "$TRACE")
IMG=$("${DB[@]}" | tee /dev/stderr | sed -n 's/^ROM: //p')
DTB=$(python3 -I tools/device.py paths dtb)
[ $COMPOSE_ONLY = 1 ] && exit 0
rm -rf "$OUT"
SYBORG_DTB="/work/${DTB#$PWD/}" SYBORG_GDB=${SYBORG_GDB:-} scripts/boot-test.sh --secs "$SECS" --out "$OUT" "${TYPE[@]}" "$IMG" > build/logs/hybrid-boot.txt 2>&1 || true
L="$OUT/serial0.log"
echo "== serial: $(wc -l < "$L") lines; EXEC92 lines:"; grep -a 'EXEC92' "$L" | sort | uniq -c | head -30 || true
echo "== processes created:"; grep -a -o 'AddThread [^ ]*::Main to [^ ]*' "$L" | awk '{print $4}' | tr '\n' ' '; echo
echo "== faults/panics:"; grep -a -n -i -E 'FAULT|panic|Thread .* Panic|KERN-EXEC|exception' "$L" | head -20 || true
echo "== ISC:"; grep -a "ISCSTUB\|N95COMPAT" "$L" | sort | uniq -c | sort -rn | head -30 || true
echo "== tail:"; tail -n 25 "$L"
