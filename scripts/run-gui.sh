#!/usr/bin/env bash
# Interactive run of a device instance in QEMU's own window, with everything a post-mortem needs (for
# the everyday phone window use `sydb phone NAME`, docs/sydb.md):
#   INSTANCE=NAME scripts/run-gui.sh   serial log + QMP socket in build/runs/gui-<time>/ (also build/runs/gui-last)
# Instance: INSTANCE, else the primary instance of $SYBORG_DEVICE / the default device (tools/instance.py
# primary). Its drives (build/instances/NAME/drives/: C: nvmemory.img, E: nvmemory2.img, rtc-offset) and
# ROM (its rom_variant) are used; DRIVE_C= / DRIVE_E= / ROM= / RTC_OFFSET= override them. The instance
# must not be running as a sydb session at the same time.
# After a hang (leave QEMU running or close it), run:
#   tools/syb_report.py build/runs/gui-<time>/serial.log     (processes, panics, blocked IPC)
#   python3 tools/qmp.py build/runs/gui-<time>/qmp.sock screendump build/runs/gui-<time>/hang.png
# Sound: host PipeWire by default (AUDIO=alsa or AUDIO=sdl to switch).
# GLES: the host GPU by default (syborg-gl); GL=soft for the guest's software rasteriser.
# Controls: docs/devices/nokia-n95-8gb-rm320/n95-controls.md (F5 left softkey / OK, F6 centre, F7 right softkey, arrows).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INST="${INSTANCE:-$(python3 -I "$ROOT/tools/instance.py" primary)}"
IDIR="$(python3 -I "$ROOT/tools/instance.py" get "$INST" dir)"
SYBORG_DEVICE="$(python3 -I "$ROOT/tools/instance.py" get "$INST" device)"
SYBORG_INSTANCE="$IDIR"
export SYBORG_DEVICE SYBORG_INSTANCE
if [ -f "$IDIR/daemon.pid" ] && kill -0 "$(cat "$IDIR/daemon.pid")" 2>/dev/null; then
  echo "instance $INST runs as a sydb session: sydb --instance $INST stop (or use sydb phone $INST)" >&2; exit 1
fi
DTB="$(python3 -I "$ROOT/tools/device.py" paths dtb)"
mkdir -p "$IDIR/drives"
D="$ROOT/build/runs/gui-$(date +%H%M%S)"
mkdir -p "$D"
ln -sfn "$D" "$ROOT/build/runs/gui-last"
echo "instance $INST ($SYBORG_DEVICE); serial log: $D/serial.log   qmp: $D/qmp.sock   (also build/runs/gui-last)"
exec "$ROOT/scripts/run-modern.sh" --gui \
  --nvmem "${DRIVE_C:-$IDIR/drives/nvmemory.img}" --nvmem2 "${DRIVE_E:-$IDIR/drives/nvmemory2.img}" --qmp "$D/qmp.sock" --serial-log "$D/serial.log" \
  --dtb "$DTB" "${ROM:-$(python3 -I "$ROOT/tools/instance.py" get "$INST" rom)}" \
  -audiodev "${AUDIO:-pipewire},id=snd0" -machine audiodev=snd0 \
  -global "syborg-gl.backend=$([ "${GL:-host}" = soft ] && echo off || echo host)" \
  -global "syborg-rtc.offset-file=${RTC_OFFSET:-$IDIR/drives/rtc-offset}" "$@"
