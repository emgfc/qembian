#!/usr/bin/env bash
# Interactive N95 run with everything a post-mortem needs:
#   scripts/run-gui.sh            GUI window, serial log + QMP socket in build/runs/gui-<time>/
# After a hang (leave QEMU running or close it), run:
#   tools/syb_report.py build/runs/gui-<time>/serial.log     (processes, panics, blocked IPC)
#   python3 tools/qmp.py build/runs/gui-<time>/qmp.sock screendump build/runs/gui-<time>/hang.png
# Controls: docs/n95-controls.md (F5 left softkey / OK, F6 centre, F7 right softkey, arrows).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/build/runs/gui-$(date +%H%M%S)"
mkdir -p "$D"
ln -sfn "$D" "$ROOT/build/runs/gui-last"
echo "serial log: $D/serial.log   qmp: $D/qmp.sock   (also build/runs/gui-last)"
exec "$ROOT/scripts/run-modern.sh" --gui --qmp "$D/qmp.sock" --serial-log "$D/serial.log" \
  --dtb "$ROOT/build/n95/syborg-n95.dtb" "$ROOT/build/n95/hybrid.img" "$@"
