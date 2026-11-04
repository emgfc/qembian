#!/usr/bin/env bash
# Boot a Syborg ROM image in the Symbian QEMU (runs inside the legacy container).
#
#   scripts/run.sh [options] [ROM.img]
#     --fixture       boot the prebuilt PDK 4.0a syborg_tshell_ARMV5_udeb.img (reference ROM)
#     --timeout N     kill QEMU after N seconds (default: run until killed)
#     --gui           use SDL window (needs DISPLAY/X11 forwarding into the container)
#     --gdb           start QEMU's gdbstub on :1234 and wait for a debugger (-s -S)
#     --serial-log F  write serial0 (kernel debug port) to F instead of stdout
#   Default ROM: build/epocroot/epoc32/rom/syborg_tshell_ARMV5_udeb.img
#
# Run state (nvmemory drive image etc.) lives in build/run/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

TIMEOUT=""; GUI=0; GDB=0; ROM=""; SERIAL_LOG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --fixture) ROM=cache/pdk-rom/epoc32/rom/syborg_tshell_ARMV5_udeb.img ;;
    --timeout) TIMEOUT="$2"; shift ;;
    --gui) GUI=1 ;;
    --gdb) GDB=1 ;;
    --serial-log) SERIAL_LOG="$2"; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) ROM="$1" ;;
  esac
  shift
done
ROM="${ROM:-build/epocroot/epoc32/rom/syborg_tshell_ARMV5_udeb.img}"

if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  extra=()
  [ $GDB = 1 ] && extra+=(-p 127.0.0.1:1234:1234)
  if [ $GUI = 1 ]; then
    extra+=(-e DISPLAY="${DISPLAY:-:0}" -v /tmp/.X11-unix:/tmp/.X11-unix)
    [ -n "${XAUTHORITY:-}" ] && extra+=(-e XAUTHORITY=/xauth -v "$XAUTHORITY:/xauth:ro")
  fi
  args=("$ROM")
  [ -n "$TIMEOUT" ] && args=(--timeout "$TIMEOUT" "${args[@]}")
  [ $GUI = 1 ] && args=(--gui "${args[@]}")
  [ $GDB = 1 ] && args=(--gdb "${args[@]}")
  [ -n "$SERIAL_LOG" ] && args=(--serial-log "$SERIAL_LOG" "${args[@]}")
  CONTAINER_ARGS="${extra[*]+${extra[*]}}" exec "$ROOT/scripts/in-container.sh" /work/scripts/run.sh "${args[@]}"
fi

QI=/work/build/qemu/install
cd /work
ROM_ABS="$(readlink -f "$ROM")"
[ -f "$ROM_ABS" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
DTB=/work/external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dtb

mkdir -p /work/build/run && cd /work/build/run
export LD_LIBRARY_PATH="$QI/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

serial=stdio
[ -n "$SERIAL_LOG" ] && serial="file:$SERIAL_LOG"
qargs=(-M "$DTB" -kernel "$ROM_ABS" -L "$QI/share/qemu" -serial "$serial")
# Headless: no SDL window, monitor disabled.
if [ $GUI = 0 ]; then
  qargs+=(-nographic -monitor null)
  # This QEMU fork initialises SDL even with -nographic; give it dummy drivers.
  export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fi
[ $GDB = 1 ] && qargs+=(-s -S)

echo ">> qemu-system-arm ${qargs[*]}" >&2
if [ -n "$TIMEOUT" ]; then
  exec timeout -s KILL "$TIMEOUT" "$QI/bin/qemu-system-arm" "${qargs[@]}"
else
  exec "$QI/bin/qemu-system-arm" "${qargs[@]}"
fi
