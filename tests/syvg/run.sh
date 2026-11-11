#!/usr/bin/env bash
# Builds libsyvg (scripts/build-syvg.sh) and runs the host-only OpenVG test.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
scripts/build-syvg.sh >/dev/null
B=build/syvg
g++ -O1 -m64 -std=gnu++11 -D__OPENVGHEADERS_USE_VG_1_1 -I"$B/ri" -I"$B/ri/include" -I"$B/inc" -I"$B/inc/VG" -Isrc/syvg \
  -include src/syvg/syvg_shim.h -o "$B/syvg_test" tests/syvg/syvg_test.cpp \
  -Wl,--start-group "$B/libsyvg.a" -Wl,--end-group 2>&1 | grep -v "warning\|note:" | grep -E "error" || true
"$B/syvg_test"
