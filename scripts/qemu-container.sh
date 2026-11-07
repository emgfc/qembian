#!/usr/bin/env bash
# Drop-in for qemu-system-arm that runs the container-built QEMU (scripts/build-qemu-modern.sh
# --container -> build/qemu-modern-ct/) inside the modern image, so a host needs no QEMU libraries.
# scripts/run-modern.sh uses it with SYBORG_QEMU=container, or by itself when only the container
# build exists. Host namespaces for network (gdb stub, ports), PIDs (tools find QEMU with pgrep and
# /proc/<pid>/fd) and IPC (/dev/shm framebuffer); the GUI, GPU and sound come from the host session
# (X11/Wayland sockets, /dev/dri, PipeWire/PulseAudio in $XDG_RUNTIME_DIR, /dev/snd).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="$ROOT/build/qemu-modern-ct/install/bin/qemu-system-arm"
[ -x "$Q" ] || { echo "no container QEMU: run scripts/build-qemu-modern.sh --container" >&2; exit 1; }
a=(--network host --pid host --ipc host --security-opt label=disable)
[ -n "${DISPLAY:-}" ] && a+=(-e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix)
[ -n "${XAUTHORITY:-}" ] && [ -e "$XAUTHORITY" ] && a+=(-e "XAUTHORITY=$XAUTHORITY" -v "$XAUTHORITY:$XAUTHORITY:ro")
if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$XDG_RUNTIME_DIR" ]; then
  a+=(-e XDG_RUNTIME_DIR -v "$XDG_RUNTIME_DIR:$XDG_RUNTIME_DIR")
  [ -n "${WAYLAND_DISPLAY:-}" ] && a+=(-e WAYLAND_DISPLAY)
fi
[ -e /dev/dri ] && a+=(--device /dev/dri)
[ -e /dev/snd ] && a+=(--device /dev/snd)
for v in AUDIO PIPEWIRE_RUNTIME_DIR PULSE_SERVER GDK_BACKEND SDL_VIDEODRIVER; do [ -n "${!v:-}" ] && a+=(-e "$v"); done
CONTAINER=modern CONTAINER_ARGS="${a[*]} ${CONTAINER_ARGS:-}" exec "$ROOT/scripts/in-container.sh" "$Q" "$@"
