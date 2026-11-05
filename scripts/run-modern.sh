#!/usr/bin/env bash
# Run a Syborg ROM on modern QEMU (host-native, no container).
#   scripts/run-modern.sh [--timeout N] [--gui] [--gdb|--gdb-wait] [--serial-log F]
#                         [--fb WxH] [--dtb FILE] [--qmp SOCK] [--] ROM.img [extra qemu args]
# Defaults: headless (-display none), serial0 on stdout, fresh 64 MB NVMemory image in
# build/run-modern/, framebuffer 240x320.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="$ROOT/build/qemu-modern/install/bin/qemu-system-arm"
TIMEOUT=""; DISPLAY_OPT=(-display none); GDB=(); SERIAL=(-serial stdio); FB=240x320
DTB=""; QMP=""; ROM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT="$2"; shift ;;
    --gui) DISPLAY_OPT=() ;;
    --gdb) GDB=(-s) ;;
    --gdb-wait) GDB=(-s -S) ;;
    --serial-log) SERIAL=(-serial "file:$2"); shift ;;
    --fb) FB="$2"; shift ;;
    --dtb) DTB="$2"; shift ;;
    --qmp) QMP="$2"; shift ;;
    --) shift; ROM="$1"; shift; break ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) ROM="$1"; shift; break ;;
  esac
  shift
done
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
RUN="$ROOT/build/run-modern"; mkdir -p "$RUN"
rm -f "$RUN/nvmemory.img"; truncate -s 64M "$RUN/nvmemory.img"
args=(-M syborg -kernel "$ROM" "${DISPLAY_OPT[@]}" "${SERIAL[@]}" -monitor none
      -drive "if=mtd,format=raw,file=$RUN/nvmemory.img"
      -global "syborg-fb.width=${FB%x*}" -global "syborg-fb.height=${FB#*x}"
      "${GDB[@]}")
[ -n "$DTB" ] && args+=(-dtb "$DTB")
[ -n "$QMP" ] && args+=(-qmp "unix:$QMP,server=on,wait=off")
args+=("$@")
echo ">> $QEMU ${args[*]}" >&2
if [ -n "$TIMEOUT" ]; then
  exec timeout -s KILL "$TIMEOUT" "$QEMU" "${args[@]}"
fi
exec "$QEMU" "${args[@]}"
