#!/usr/bin/env bash
# Run a Syborg ROM on modern QEMU (host build, or the container build through scripts/qemu-container.sh).
#   scripts/run-modern.sh [--device ID] [--timeout N] [--gui] [--gdb|--gdb-wait] [--serial-log F]
#                         [--fb WxH] [--dtb FILE] [--qmp SOCK] [--nvmem FILE] [--nvmem2 FILE] [--name NAME]
#                         [--] ROM.img [extra qemu args]
# The board (machine, CPU, RAM, display, ROFS flash window, drive sizes) comes from the device profile
# (tools/device.py qemu-args; --device ID, else $SYBORG_DEVICE, else devices/default). --fb overrides
# the display size.
# Defaults: headless (-display none), serial0 on stdout, fresh 64 MB NVMemory image (C: drive)
# per run (--nvmem, default build/run-modern/nvmemory-<pid>.img, deleted on exit unless given),
# E: likewise (--nvmem2, 1 GB sparse). A FILE given explicitly is persistent: created when
# missing (C: 64 MB, E: 1 GB, or NVMEM_SIZE / NVMEM2_SIZE), never wiped; the phone formats it.
# framebuffer 240x320. ROM.rofs (next to the ROM) is loaded into the 0x60000000 flash window. --name sets the QEMU process name (tools use 'sybtool' and kill only those).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# QEMU: $SYBORG_QEMU (a path, or "container" for scripts/qemu-container.sh), else the host build,
# else the container build (scripts/build-qemu-modern.sh [--container])
QEMU="${SYBORG_QEMU:-$ROOT/build/qemu-modern/install/bin/qemu-system-arm}"
[ "$QEMU" = container ] && QEMU="$ROOT/scripts/qemu-container.sh"
if [ ! -x "$QEMU" ] && [ -x "$ROOT/build/qemu-modern-ct/install/bin/qemu-system-arm" ]; then
  QEMU="$ROOT/scripts/qemu-container.sh"
fi
TIMEOUT=""; DISPLAY_OPT=(-display none); GDB=(); SERIAL=(-serial stdio); FB=""
DTB=""; QMP=""; ROM=""; NVMEM=""; NVMEM2=""; NAME=""; DEVICE="${SYBORG_DEVICE:-}"
while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE="$2"; shift ;;
    --timeout) TIMEOUT="$2"; shift ;;
    --gui) DISPLAY_OPT=() ;;
    --gdb) GDB=(-s) ;;
    --gdb-wait) GDB=(-s -S) ;;
    --serial-log) SERIAL=(-serial "file:$2"); shift ;;
    --fb) FB="$2"; shift ;;
    --dtb) DTB="$2"; shift ;;
    --qmp) QMP="$2"; shift ;;
    --nvmem) NVMEM="$2"; shift ;;
    --nvmem2) NVMEM2="$2"; shift ;;
    --name) NAME="$2"; shift ;;
    --) shift; ROM="$1"; shift; break ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) ROM="$1"; shift; break ;;
  esac
  shift
done
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
RUN="$ROOT/build/run-modern"; mkdir -p "$RUN"
TMP=()
if [ -z "$NVMEM" ]; then NVMEM="$RUN/nvmemory-$$.img"; TMP+=("$NVMEM"); rm -f "$NVMEM"; fi
if [ -z "$NVMEM2" ]; then NVMEM2="$RUN/nvmemory2-$$.img"; TMP+=("$NVMEM2"); rm -f "$NVMEM2"; fi
[ ${#TMP[@]} -gt 0 ] && trap 'rm -f "${TMP[@]}"' EXIT
# never two emulators on one drive image: the FAT would be written from two views
for img in "$NVMEM" "$NVMEM2"; do
  [ -e "$img" ] || continue
  real=$(realpath "$img")
  for pid in $(pgrep -f qemu-system-arm); do		# the files it has open, whatever path it was given
    if find "/proc/$pid/fd" -lname "$real" 2>/dev/null | grep -q .; then
      echo "$img is in use by QEMU pid $pid (a sydb session or run-gui): stop it first" >&2; exit 1
    fi
  done
done
DEV=(); [ -n "$DEVICE" ] && DEV=("$DEVICE")
DEVPY=(python3 -I "$ROOT/tools/device.py")
mapfile -t BOARD < <("${DEVPY[@]}" qemu-args "${DEV[@]}" --rom "$ROM")
[ ${#BOARD[@]} -gt 0 ] || { echo "no board arguments from tools/device.py" >&2; exit 1; }
size() { "${DEVPY[@]}" get "${DEV[@]}" board.drives | python3 -c "import json,sys;print([d['size'] for d in json.load(sys.stdin) if d.get('kind')=='$1'][0])"; }
[ -e "$NVMEM" ] || truncate -s "${NVMEM_SIZE:-$(size nvmem)}" "$NVMEM"
[ -e "$NVMEM2" ] || truncate -s "${NVMEM2_SIZE:-$(size nvmem2)}" "$NVMEM2"
# board from the device profile (with the ROM's ROFS partition in the flash window), then the run
args=("${BOARD[@]}" -kernel "$ROM" "${DISPLAY_OPT[@]}" "${SERIAL[@]}" -monitor none
      -drive "if=mtd,format=raw,file=$NVMEM" -drive "if=mtd,index=1,format=raw,file=$NVMEM2"
      "${GDB[@]}")
[ -n "$FB" ] && args+=(-global "syborg-fb.width=${FB%x*}" -global "syborg-fb.height=${FB#*x}")
[ -n "$DTB" ] && args+=(-dtb "$DTB")
[ -n "$QMP" ] && args+=(-qmp "unix:$QMP,server=on,wait=off")
[ -n "$NAME" ] && args+=(-name "$NAME")
args+=("$@")
echo ">> $QEMU ${args[*]}" >&2
if [ -n "$TIMEOUT" ]; then
  timeout -k 10 "$TIMEOUT" "$QEMU" "${args[@]}"
else
  "$QEMU" "${args[@]}"
fi
