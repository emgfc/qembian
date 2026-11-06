#!/usr/bin/env bash
# Build the sygl blob (our GLES 1.1 + EGL behind libgles_cm.dll's entry points; docs/gpu-roadmap.md)
# and the libgles_cm patches that point at it.
#   src/n95gl/build.sh [--debug]
# Output: build/n95gl/sygl.bin (raw, linked at SYGL_BASE), sygl.elf (symbols for gdb), sygl.map,
#         rom/hybrid/gles-patches.txt (regenerated; depends only on SYGL_BASE).
# scripts/hybrid-boot.sh puts the blob into the composed ROM when sygl.bin exists.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
SYGL_BASE=0x80320000          # free gap in our ROM window (dead user-side files), see romcompose --blob
OUT="$ROOT/build/n95gl"; GEN="$OUT/gen"
mkdir -p "$GEN"
OPT=(-O2)
[ "${1:-}" = "--debug" ] && OPT=(-O0 -g)

python3 -I "$ROOT/tools/gen-gles-glue.py" --blob-addr $SYGL_BASE \
  --patches "$ROOT/rom/hybrid/gles-patches.txt" --asm "$GEN/entry.S" --names "$GEN/names.c" \
  --imports "$GEN/n95imports.h"

CFLAGS=(-c -march=armv5t -marm -mfloat-abi=soft "${OPT[@]}" -std=gnu99 -ffreestanding -fno-builtin
        -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions
        -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter -I"$GEN" -I"$HERE")
"$GCC-gcc" "${CFLAGS[@]}" -o "$GEN/sygl.o" "$HERE/sygl.c"
"$GCC-gcc" "${CFLAGS[@]}" -o "$GEN/names.o" "$GEN/names.c"
"$GCC-as" -march=armv5t -o "$GEN/entry.o" "$GEN/entry.S"
LIBGCC="$("$GCC-gcc" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
"$GCC-ld" -nostdlib --no-undefined -T "$HERE/sygl.ld" --defsym SYGL_BASE=$SYGL_BASE --gc-sections \
  -u sygl_entry_table -Map "$OUT/sygl.map" -o "$OUT/sygl.elf" \
  "$GEN/entry.o" "$GEN/sygl.o" "$GEN/names.o" "$LIBGCC"
"$GCC-objcopy" -O binary "$OUT/sygl.elf" "$OUT/sygl.bin"
echo $SYGL_BASE > "$OUT/sygl.addr"
echo "sygl: $(stat -c %s "$OUT/sygl.bin") bytes at $SYGL_BASE -> $OUT/sygl.bin"
