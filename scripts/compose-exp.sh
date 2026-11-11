#!/usr/bin/env bash
# Compose the audio-exp variant of the device ROM (build/roms/<id>/<fw>/hybrid-exp.img): the working ROM
# without the [H-keysound-nommf] and [H-startuptone-nommf] byte patches (MMF works through the DSP
# stand-in, so the startup jingle plays). The variant is declared in the profile:
# [rom.variants.audio-exp]. Boot it with ROM=build/roms/<id>/<fw>/hybrid-exp.img scripts/run-gui.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -I tools/device.py build --variant audio-exp --no-standins "$@"
