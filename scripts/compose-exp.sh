#!/usr/bin/env bash
# Compose the experimental audio ROM build/n95/hybrid-exp.img: the working ROM without the
# H-keysound-nommf and H-startuptone-nommf byte patches (MMF works through the DSP stand-in, so the
# startup jingle plays; docs/roadmap.md "Audio"). Boot it with ROM=build/n95/hybrid-exp.img scripts/run-gui.sh.
# A running QEMU is not affected (the ROM is read once at start).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
P=$(mktemp); trap 'rm -f "$P"' EXIT
grep -v -E '^(82f0e366|82f0e9c8|82f0ea94|82f0eac8|833a93c4)\b' rom/hybrid/code-patches.txt > "$P"
HYBRID_IMG=build/n95/hybrid-exp.img HYBRID_PATCHES="$P" scripts/hybrid-boot.sh --compose-only "$@"
