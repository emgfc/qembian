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
  CONTAINER_ARGS="${SYBORG_DTB:+-e SYBORG_DTB=$SYBORG_DTB}${SYBORG_GDB:+ -e SYBORG_GDB=$SYBORG_GDB -p 127.0.0.1:1234:1234}" exec "$ROOT/scripts/in-container.sh" /work/scripts/boot-test.sh "$@"
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
DTB=${SYBORG_DTB:-/work/external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dtb}	# SYBORG_DTB: path under /work
export LD_LIBRARY_PATH="$QI/lib" SDL_AUDIODRIVER=dummy DISPLAY=:99

Xvfb :99 -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!
trap 'kill $QPID $XPID 2>/dev/null || true' EXIT
sleep 1

# Fresh nvmemory (C:) drive for every test run.
RUNDIR="$OUT/run"; mkdir -p "$RUNDIR"; cd "$RUNDIR"
qargs=(-M "$DTB" -kernel "$ROM_ABS" -L "$QI/share/qemu" -serial "file:$OUT/serial0.log" -monitor null)
[ -n "${SYBORG_GDB:-}" ] && qargs+=(-s)   # SYBORG_GDB=1: gdbstub on 127.0.0.1:1234 (no wait)
[ "${SYBORG_GDB:-}" = wait ] && qargs+=(-S)   # SYBORG_GDB=wait: also halt until gdb continues
echo ">> qemu-system-arm ${qargs[*]}" | tee "$OUT/qemu.log"
"$QI/bin/qemu-system-arm" "${qargs[@]}" >>"$OUT/qemu.log" 2>&1 &
QPID=$!

# Type a line into the guest. The Syborg keymap is UK layout: PC scancode 0x2B (where X's US
# layout puts "\") is "#" there, and "\" is the ISO 102nd key (scancode 0x56 = X keycode 94,
# keysym "less" in Xvfb's default layout). So send each "\" as that key.
type_line() {
  local s="$1" seg
  while :; do
    seg="${s%%\\*}"
    [ -n "$seg" ] && xdotool type --delay 120 "$seg"
    [ "$seg" = "$s" ] && break
    xdotool key less; s="${s#*\\}"
  done
  xdotool key Return
}

shot() { xwd -root -silent | convert xwd:- "$OUT/screen-$1.png" 2>/dev/null || true; }

# Screenshot every 10 s; type commands (if any) at the half-way point.
n=0; t=0
while [ $t -lt "$SECS" ] && kill -0 $QPID 2>/dev/null; do
  sleep 5; t=$((t+5))
  if [ ${#TYPE[@]} -gt 0 ] && [ $t -eq $(( (SECS/2) / 5 * 5 )) ]; then
    for c in "${TYPE[@]}"; do
      xdotool search --name . windowfocus 2>/dev/null || true
      type_line "$c"; sleep 3
    done
  fi
  if [ $((t % 10)) -eq 0 ]; then shot "$(printf %02d $n)"; n=$((n+1)); fi
done
shot final
kill -0 $QPID 2>/dev/null && echo ">> QEMU still running after ${SECS}s (killed)" | tee -a "$OUT/qemu.log" \
  || echo ">> QEMU exited early" | tee -a "$OUT/qemu.log"
echo ">> results in ${OUT#/work/}"; ls "$OUT"
