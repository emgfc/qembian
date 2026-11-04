#!/usr/bin/env bash
# Headless boot test: run a Syborg ROM under Xvfb, capture serial0 and framebuffer screenshots,
# optionally type commands into the text shell.
#
#   scripts/boot-test.sh [--fixture] [--secs N] [--type "cmd"]... [--out DIR] [ROM.img]
#
# Outputs in DIR (default build/boot-test/<rom-name>/):
#   serial0.log      kernel debug port (Kern::Printf / RDebug)
#   qemu.log         QEMU + Python device-model stdout/stderr
#   screen-NN.png    framebuffer snapshots
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  exec "$ROOT/scripts/in-container.sh" /work/scripts/boot-test.sh "$@"
fi

SECS=60; ROM=""; OUT=""; TYPE=()
while [ $# -gt 0 ]; do
  case "$1" in
    --fixture) ROM=cache/pdk-rom/epoc32/rom/syborg_tshell_ARMV5_udeb.img ;;
    --secs) SECS="$2"; shift ;;
    --type) TYPE+=("$2"); shift ;;
    --out) OUT="$2"; shift ;;
    *) ROM="$1" ;;
  esac
  shift
done
cd /work
ROM="${ROM:-build/epocroot/epoc32/rom/syborg_tshell_ARMV5_udeb.img}"
ROM_ABS="$(readlink -f "$ROM")"; [ -f "$ROM_ABS" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
OUT="${OUT:-build/boot-test/$(basename "$ROM" .img)}"
rm -rf "$OUT"; mkdir -p "$OUT"; OUT="$(readlink -f "$OUT")"

QI=/work/build/qemu/install
DTB=/work/external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dtb
export LD_LIBRARY_PATH="$QI/lib" SDL_AUDIODRIVER=dummy DISPLAY=:99

Xvfb :99 -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!
trap 'kill $QPID $XPID 2>/dev/null || true' EXIT
sleep 1

# Fresh nvmemory (C:) drive for every test run.
RUNDIR="$OUT/run"; mkdir -p "$RUNDIR"; cd "$RUNDIR"
qargs=(-M "$DTB" -kernel "$ROM_ABS" -L "$QI/share/qemu" -serial "file:$OUT/serial0.log" -monitor null)
echo ">> qemu-system-arm ${qargs[*]}" | tee "$OUT/qemu.log"
"$QI/bin/qemu-system-arm" "${qargs[@]}" >>"$OUT/qemu.log" 2>&1 &
QPID=$!

shot() { xwd -root -silent | convert xwd:- "$OUT/screen-$1.png" 2>/dev/null || true; }

# Screenshot every 10 s; type commands (if any) at the half-way point.
n=0; t=0
while [ $t -lt "$SECS" ] && kill -0 $QPID 2>/dev/null; do
  sleep 5; t=$((t+5))
  if [ ${#TYPE[@]} -gt 0 ] && [ $t -eq $(( (SECS/2) / 5 * 5 )) ]; then
    for c in "${TYPE[@]}"; do
      xdotool search --name . windowfocus 2>/dev/null || true
      xdotool type --delay 120 "$c"; xdotool key Return; sleep 3
    done
  fi
  if [ $((t % 10)) -eq 0 ]; then shot "$(printf %02d $n)"; n=$((n+1)); fi
done
shot final
kill -0 $QPID 2>/dev/null && echo ">> QEMU still running after ${SECS}s (killed)" | tee -a "$OUT/qemu.log" \
  || echo ">> QEMU exited early" | tee -a "$OUT/qemu.log"
echo ">> results in ${OUT#/work/}"; ls "$OUT"
