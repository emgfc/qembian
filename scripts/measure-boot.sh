#!/usr/bin/env bash
# Boot-speed benchmark: compose the hybrid ROM (panic-only trace), run it headless and print the
# wall-clock time at which each milestone appears on the kernel debug port.
#   scripts/measure-boot.sh [--timeout SECS] [--label NAME] [--qemu legacy|modern] [--skip-compose]
# Milestones are lines our stand-ins always print (independent of the trace mask). Results are
# appended to build/logs/boot-times.tsv.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
TIMEOUT=1500; LABEL=run; QEMU_KIND=legacy; COMPOSE=1
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT="$2"; shift ;;
    --label) LABEL="$2"; shift ;;
    --qemu) QEMU_KIND="$2"; shift ;;
    --skip-compose) COMPOSE=0 ;;
    *) echo "unknown arg $1" >&2; exit 2 ;;
  esac; shift
done
mkdir -p build/logs
[ $COMPOSE = 1 ] && scripts/hybrid-boot.sh --compose-only --trace 0x80000000 > build/logs/measure-compose.txt 2>&1
if [ "$QEMU_KIND" = modern ]; then
  run=(scripts/run-modern.sh --timeout "$TIMEOUT" --dtb "$(python3 -I tools/device.py paths dtb)" "$(python3 -I tools/device.py paths rom)")
else
  run=(env SYBORG_DTB="/work/$(python3 -I tools/device.py paths dtb | sed "s|^$PWD/||")" scripts/run.sh --timeout "$TIMEOUT" "$(python3 -I tools/device.py paths rom | sed "s|^$PWD/||")")
fi
"${run[@]}" 2>/dev/null \
  | python3 -u tools/boot_milestones.py --label "$LABEL" --log build/logs/measure-serial.log \
      --tsv build/logs/boot-times.tsv
