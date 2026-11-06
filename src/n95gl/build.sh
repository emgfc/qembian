#!/usr/bin/env bash
# Build the sygl blob (our GLES 1.1 + EGL behind libgles_cm.dll's entry points; docs/gpu-roadmap.md)
# and the libgles_cm patches that point at it.
#   src/n95gl/build.sh [--debug] [--trace]
# Output: build/n95gl/sygl.bin (raw, linked at SYGL_BASE), sygl.elf (symbols for gdb), sygl.map,
#         rom/hybrid/gles-patches.txt (regenerated; depends only on SYGL_BASE).
# scripts/hybrid-boot.sh puts the blob into the composed ROM when sygl.bin exists.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
SYGL_BASE=0x80320000          # free gap in our ROM window (dead user-side files), see romcompose --blob
OUT="$ROOT/build/n95gl"; GEN="$OUT/gen"
mkdir -p "$GEN"
OPT=(-O2)
for arg in "$@"; do
  case "$arg" in
    --debug) OPT=(-O0 -g) ;;
    --trace) TRACE=1 ;;		# SYGL: trace lines (first 700 EGL/GL calls per thread, draw boxes)
  esac
done
[ -n "${TRACE:-}" ] && OPT+=(-DSY_TRACE_MAX=700)

python3 -I "$ROOT/tools/gen-gles-glue.py" --blob-addr $SYGL_BASE \
  --patches "$ROOT/rom/hybrid/gles-patches.txt" --asm "$GEN/entry.S" --names "$GEN/names.c" \
  --imports "$GEN/n95imports.h" --imports-asm "$GEN/imports.S"

# portable core: C, soft-float (the Symbian ARMV5 ABI passes floats in core registers)
COMMON=(-c -march=armv5t -marm -mfloat-abi=soft "${OPT[@]}" -ffreestanding -fno-builtin -fno-common
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -fsingle-precision-constant
        -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter -I"$GEN" -I"$HERE")
CFLAGS=("${COMMON[@]}" -std=gnu99 -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)
# platform layer: C++ against the S60 3rd FP1 SDK headers (as src/n95apps/devagent does)
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$GEN/gcce_fixed.h"
CXXFLAGS=("${COMMON[@]}" -std=gnu++98 -nostdinc -fno-rtti -fno-threadsafe-statics -fshort-wchar
  -fno-strict-aliasing -Wno-attributes -Wno-narrowing -Wno-unused-variable
  -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__ -D__DLL__
  -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os_v9.2.hrh\""
  -include "$GEN/gcce_fixed.h" -I"$SDK/include" -I"$SDK/include/variant" -I"$SDK/include/gcce")

OBJS=()
for c in egl gl light tex raster libc; do
  "$GCC-gcc" "${CFLAGS[@]}" -o "$GEN/$c.o" "$HERE/$c.c"; OBJS+=("$GEN/$c.o")
done
"$GCC-gcc" "${CFLAGS[@]}" -o "$GEN/names.o" "$GEN/names.c"; OBJS+=("$GEN/names.o")
"$GCC-g++" "${CXXFLAGS[@]}" -o "$GEN/plat.o" "$HERE/plat.cpp"; OBJS+=("$GEN/plat.o")
"$GCC-as" -march=armv5t -o "$GEN/entry.o" "$GEN/entry.S"
"$GCC-as" -march=armv5t -o "$GEN/imports.o" "$GEN/imports.S"
LIBGCC="$("$GCC-gcc" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
"$GCC-ld" -nostdlib --no-undefined -T "$HERE/sygl.ld" --defsym SYGL_BASE=$SYGL_BASE --gc-sections \
  -u sygl_entry_table -Map "$OUT/sygl.map" -o "$OUT/sygl.elf" \
  "$GEN/entry.o" "${OBJS[@]}" "$GEN/imports.o" "$LIBGCC"
"$GCC-objcopy" -O binary "$OUT/sygl.elf" "$OUT/sygl.bin"
echo $SYGL_BASE > "$OUT/sygl.addr"
echo "sygl: $(stat -c %s "$OUT/sygl.bin") bytes at $SYGL_BASE -> $OUT/sygl.bin"
