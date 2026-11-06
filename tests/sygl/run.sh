#!/usr/bin/env bash
# Host tests of the portable sygl core (no phone needed). Needs the device's sygl gen dir (src/sygl/build.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/sygl-tests"; mkdir -p "$OUT"
CF=(-DSYGL_HOST -O1 -g -Wall -Wno-unused-function -I"$(python3 -I "$ROOT/tools/device.py" paths dir)/sygl/gen" -I"$ROOT/src/sygl" -fsanitize=address,undefined)
for t in "$ROOT"/tests/sygl/*_test.c; do
  n=$(basename "$t" .c)
  gcc "${CF[@]}" -o "$OUT/$n" "$t" "$ROOT/src/sygl/raster.c"
  echo "== $n"; "$OUT/$n"
done
