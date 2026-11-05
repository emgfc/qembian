#!/usr/bin/env bash
# Run the N95 hybrid ROM interactively in an SDL window (no rebuild).
#   scripts/hybrid-run.sh [--trace MASK] [--serial FILE]
# Composes build/n95/hybrid-run.img from the last builds (panic-only kernel trace by default, so
# the boot is not slowed down by serial output) on the 240x320 N95 board config.
# Serial output (kernel debug, ISCPEER/N95STUB lines) goes to build/run/hybrid-run-serial.log;
# follow it with:  tail -f build/run/hybrid-run-serial.log
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
TRACE=0x80000000; SERIAL=build/run/hybrid-run-serial.log
while [ $# -gt 0 ]; do
  case "$1" in --trace) TRACE="$2"; shift ;; --serial) SERIAL="$2"; shift ;; *) echo "unknown arg $1" >&2; exit 2 ;; esac; shift
done
python3 -I tools/romcompose.py --ours build/rom-hybrid/syborg_hybrid_kernel_ARMV5_udeb.img \
  --n95 build/n95/core-flat.img -o build/n95/hybrid-run.img --exec92 --trace "$TRACE" \
  --file Sys/Data/ESTARTCOMP.TXT=rom/hybrid/ESTARTCOMP.TXT \
  --rofs build/n95/rofs1.img --rofs build/n95/rofs2.img --patches rom/hybrid/code-patches.txt > /dev/null
python3 -I tools/dtb_set.py external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dtb build/n95/syborg-n95.dtb \
  framebuffer:width=240 framebuffer:height=320 > /dev/null
mkdir -p build/run
echo "serial log: $SERIAL   (close the QEMU window or Ctrl-C to stop)"
SYBORG_DTB=/work/build/n95/syborg-n95.dtb exec scripts/run.sh --gui --serial-log "/work/$SERIAL" build/n95/hybrid-run.img
