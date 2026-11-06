#!/usr/bin/env bash
# Host tests of the portable sygl core (no phone needed). Needs build/n95gl/gen (src/n95gl/build.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/sygl-tests"; mkdir -p "$OUT"
CF=(-DSYGL_HOST -O1 -g -Wall -Wno-unused-function -I"$ROOT/build/n95gl/gen" -I"$ROOT/src/n95gl" -fsanitize=address,undefined)
for t in "$ROOT"/tests/sygl/*_test.c; do
  n=$(basename "$t" .c)
  gcc "${CF[@]}" -o "$OUT/$n" "$t" "$ROOT/src/n95gl/raster.c"
  echo "== $n"; "$OUT/$n"
done
