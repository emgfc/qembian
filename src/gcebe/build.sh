#!/usr/bin/env bash
# Build the gcebe blob (our composition backend for Symbian^3's gce.dll, src/gcebe/gcebe.c) and the
# patches that rebind gce's import of compositionbackend.dll ordinal 1 to it.
#   src/gcebe/build.sh [--device ID]        (device: $SYBORG_DEVICE / the registry default)
# Profile [composition]: impl = "gcebe", blob_addr, imports (list), patches (generated), rebind.
# Output: build/roms/<id>/<fw>/gcebe/{gcebe.bin, gcebe.elf, gcebe.map, gcebe.addr};
# models/<id>/<patches> (regenerated). tools/device.py build puts the blob into the composed ROM.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
DEV="${SYBORG_DEVICE:-}"
while [ $# -gt 0 ]; do case "$1" in --device) DEV="$2"; shift ;; esac; shift; done
export SYBORG_DEVICE="$DEV"
DPY=(python3 -I "$ROOT/tools/device.py")
DEVDIR="$("${DPY[@]}" paths dir)"
pf() { "${DPY[@]}" file "$1"; }    # a profile file: the firmware overlay's, else the model's
[ "$("${DPY[@]}" get composition.impl)" = gcebe ] || { echo "gcebe: profile has no [composition] impl = \"gcebe\""; exit 1; }
BASE=$(printf '0x%x' "$("${DPY[@]}" get composition.blob_addr)")
OUT="$DEVDIR/gcebe"; GEN="$OUT/gen"; mkdir -p "$GEN"
# a variant profile (extends) uses its parent's import list unless it has its own
IMPORTS="$(python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import device as d; p = d.load(sys.argv[2] or None); print(d.device_file(p, d.get(p, "composition.imports")))' "$ROOT/tools" "$DEV")"
CPATCH="$(pf "$("${DPY[@]}" get composition.patches)")"
mkdir -p "$(dirname "$CPATCH")"
python3 -I "$ROOT/tools/blob_glue.py" imports --list "$IMPORTS" \
  --asm "$GEN/imports.S" --header "$GEN/rom_imports.h"
"$GCC-gcc" -c -march=armv5t -marm -mfloat-abi=soft -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-common \
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter -I"$OUT" -o "$GEN/gcebe.o" "$HERE/gcebe.c"
"$GCC-as" -march=armv5t -o "$GEN/imports.o" "$GEN/imports.S"
LIBGCC="$("$GCC-gcc" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
"$GCC-ld" -nostdlib --no-undefined -T "$HERE/gcebe.ld" --defsym GCEBE_BASE=$BASE --gc-sections \
  -Map "$OUT/gcebe.map" -o "$OUT/gcebe.elf" "$GEN/gcebe.o" "$GEN/imports.o" "$LIBGCC"
"$GCC-objcopy" -O binary "$OUT/gcebe.elf" "$OUT/gcebe.bin"
echo $BASE > "$OUT/gcebe.addr"
# rebind: gce.dll's XIP thunk for compositionbackend.dll ordinal 1 -> gcebe_create
BINDS=$("${DPY[@]}" get composition.rebind | python3 -c "import json,sys; print(' '.join(json.load(sys.stdin)))")
python3 -I "$ROOT/tools/blob_glue.py" rebind --elf "$OUT/gcebe.elf" \
  --out "$CPATCH" $BINDS
echo "gcebe: $(stat -c %s "$OUT/gcebe.bin") bytes at $BASE -> $OUT/gcebe.bin"
