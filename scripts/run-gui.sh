#!/usr/bin/env bash
# Interactive N95 run with everything a post-mortem needs:
#   scripts/run-gui.sh            GUI window, serial log + QMP socket in build/runs/gui-<time>/
#   ROM=build/n95/hybrid-exp.img scripts/run-gui.sh     same with the audio ROM (scripts/compose-exp.sh)
# After a hang (leave QEMU running or close it), run:
#   tools/syb_report.py build/runs/gui-<time>/serial.log     (processes, panics, blocked IPC)
#   python3 tools/qmp.py build/runs/gui-<time>/qmp.sock screendump build/runs/gui-<time>/hang.png
# Drives: C: build/n95/drive-c.img (64 MB) and E: build/n95/drive-e.img (1 GB), persistent across
# runs (DRIVE_C=/DRIVE_E= to use others); copy files in/out with scripts/n95-drive.sh while QEMU is off.
# Clock: host time plus the offset in build/n95/rtc-offset (RTC_OFFSET= for another file), which
# the RTC rewrites whenever the phone's date/time is changed; delete the file to go back to host time.
# Sound: host PipeWire by default (AUDIO=alsa or AUDIO=sdl to switch).
# GLES: the host GPU by default (syborg-gl); GL=soft for the guest's software rasteriser.
# Controls: docs/n95-controls.md (F5 left softkey / OK, F6 centre, F7 right softkey, arrows).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/build/runs/gui-$(date +%H%M%S)"
mkdir -p "$D"
ln -sfn "$D" "$ROOT/build/runs/gui-last"
echo "serial log: $D/serial.log   qmp: $D/qmp.sock   (also build/runs/gui-last)"
exec "$ROOT/scripts/run-modern.sh" --gui \
  --nvmem "${DRIVE_C:-$ROOT/build/n95/drive-c.img}" --nvmem2 "${DRIVE_E:-$ROOT/build/n95/drive-e.img}" --qmp "$D/qmp.sock" --serial-log "$D/serial.log" \
  --dtb "$ROOT/build/n95/syborg-n95.dtb" "${ROM:-$ROOT/build/n95/hybrid.img}" \
  -audiodev "${AUDIO:-pipewire},id=snd0" -machine audiodev=snd0 \
  -global "syborg-gl.backend=$([ "${GL:-host}" = soft ] && echo off || echo host)" \
  -global "syborg-rtc.offset-file=${RTC_OFFSET:-$ROOT/build/n95/rtc-offset}" "$@"
