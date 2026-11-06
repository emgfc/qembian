#!/usr/bin/env bash
# Copy files into / out of the N95's persistent drives (FAT images used by scripts/run-gui.sh).
#   scripts/n95-drive.sh ls  c|e [DIR]          list (e.g. ls e /  or  ls c /data/Installs)
#   scripts/n95-drive.sh put c|e FILE... [DIR]  copy host files into DIR (default: the root)
#   scripts/n95-drive.sh get c|e PATH [DEST]    copy a file out
#   scripts/n95-drive.sh mkdir c|e DIR
#   scripts/n95-drive.sh rm c|e PATH
# Images: build/n95/drive-c.img (C:, 64 MB FAT16) and build/n95/drive-e.img (E:, the memory card,
# 1 GB FAT32); override with DRIVE_C= / DRIVE_E=. The phone formats them on first boot, so boot
# once before copying. The emulator must not be running (two writers corrupt the FAT).
# To install: put the .sis/.sisx on E: (or C:\data\Installs), boot, open it in File manager.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
command -v mcopy > /dev/null || { echo "needs mtools (sudo zypper install mtools)" >&2; exit 1; }
cmd="${1:-}"; drv="${2:-}"; shift 2 || { sed -n '2,12p' "$0"; exit 2; }
case "$drv" in
  c|C|c:|C:) IMG="${DRIVE_C:-$ROOT/build/n95/drive-c.img}" ;;
  e|E|e:|E:) IMG="${DRIVE_E:-$ROOT/build/n95/drive-e.img}" ;;
  *) echo "drive must be c or e" >&2; exit 2 ;;
esac
[ -f "$IMG" ] || { echo "no image $IMG: boot once with scripts/run-gui.sh first" >&2; exit 1; }
if pgrep -f "qemu-system-arm.*$(realpath "$IMG")" > /dev/null; then   # full path: other sessions may run
  echo "$IMG is in use by a running QEMU: close the emulator first" >&2; exit 1
fi
export MTOOLS_SKIP_CHECK=1 MTOOLS_NO_VFAT=0
p() { local x="${1:-/}"; echo "::${x//\\//}"; }   # DIR or C:\style path -> mtools path
case "$cmd" in
  ls)    mdir -i "$IMG" "$(p "${1:-/}")" ;;
  put)   [ $# -ge 1 ] || { echo "put needs FILE... [DIR]" >&2; exit 2; }
         dst=/
         if [ $# -ge 2 ] && [ ! -e "${@: -1}" ]; then dst="${@: -1}"; set -- "${@:1:$#-1}"; fi
         mcopy -i "$IMG" -o "$@" "$(p "$dst")/" < /dev/null; mdir -i "$IMG" "$(p "$dst")" ;;
  get)   mcopy -i "$IMG" -n "$(p "$1")" "${2:-.}" ;;
  mkdir) mmd -D s -i "$IMG" "$(p "$1")" < /dev/null ;;   # -D s: an existing directory is not a question
  rm)    mdel -i "$IMG" "$(p "$1")" ;;
  *)     sed -n '2,12p' "$0"; exit 2 ;;
esac
