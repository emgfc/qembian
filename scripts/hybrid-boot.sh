#!/usr/bin/env bash
# Option 1 loop: (re)build our Syborg kernel side, compose it with the N95 user side
# (9.2 exec personality on), boot headless and summarise serial output.
#   scripts/hybrid-boot.sh [--no-build] [--secs N] [--trace MASK] [--out DIR] [--type CMD]...
# Needs build/n95/core-flat.img from scripts/n95-inventory.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
BUILD=1; SECS=60; TRACE=0x80000000; OUT=build/boot-test/hybrid; TYPE=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --secs) SECS="$2"; shift ;;
    --trace) TRACE="$2"; shift ;;
    --out) OUT="$2"; shift ;;
    --type) TYPE+=(--type "$2"); shift ;;
    *) echo "unknown arg $1" >&2; exit 2 ;;
  esac; shift
done
K=build/epocroot/sf/os/kernelhwsrv/kernel/eka
python3 -I tools/exec92/gen_kernel_map.py $K/kernel/execs.txt tools/exec92/exec92.map $K/kernel/exec92_map.h
if [ $BUILD = 1 ]; then
  scripts/build-syborg.sh udeb > build/logs/hybrid-syborg-build.txt 2>&1 || { tail -30 build/logs/hybrid-syborg-build.txt; exit 1; }
  grep -E ': error|undefined reference' build/logs/syborg-udeb.log | head -20 && { echo "BUILD ERRORS"; exit 1; } || true
fi
scripts/build-n95compat.sh udeb > build/logs/hybrid-n95compat-build.txt 2>&1 || { tail -30 build/logs/hybrid-n95compat-build.txt; exit 1; }
python3 -I tools/romcompose.py --ours build/rom-hybrid/syborg_hybrid_kernel_ARMV5_udeb.img \
  --n95 build/n95/core-flat.img -o build/n95/hybrid.img --manifest build/n95/hybrid-manifest.txt \
  --exec92 --trace "$TRACE" \
  --file Sys/Data/ESTARTCOMP.TXT=rom/hybrid/ESTARTCOMP.TXT \
  --rofs build/n95/rofs1.img --rofs build/n95/rofs2.img
python3 -I tools/dtb_set.py external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dtb build/n95/syborg-n95.dtb \
  framebuffer:width=240 framebuffer:height=320 > /dev/null
rm -rf "$OUT"
SYBORG_DTB=/work/build/n95/syborg-n95.dtb SYBORG_GDB=${SYBORG_GDB:-} scripts/boot-test.sh --secs "$SECS" --out "$OUT" "${TYPE[@]}" build/n95/hybrid.img > build/logs/hybrid-boot.txt 2>&1 || true
L="$OUT/serial0.log"
echo "== serial: $(wc -l < "$L") lines; EXEC92 lines:"; grep -a 'EXEC92' "$L" | sort | uniq -c | head -30 || true
echo "== processes created:"; grep -a -o 'AddThread [^ ]*::Main to [^ ]*' "$L" | awk '{print $4}' | tr '\n' ' '; echo
echo "== faults/panics:"; grep -a -n -i -E 'FAULT|panic|Thread .* Panic|KERN-EXEC|exception' "$L" | head -20 || true
echo "== ISC:"; grep -a "ISCSTUB\|N95COMPAT" "$L" | sort | uniq -c | sort -rn | head -30 || true
echo "== tail:"; tail -n 25 "$L"
