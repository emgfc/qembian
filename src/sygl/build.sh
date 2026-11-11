#!/usr/bin/env bash
# Build the sygl blob (our GLES 1.1 + EGL behind libgles_cm.dll's entry points; docs/gpu-roadmap.md)
# and the libgles_cm patches that point at it.
#   src/sygl/build.sh [--device ID] [--debug] [--trace]        (device: $SYBORG_DEVICE / the registry default)
# Inputs from the device profile ([gl], models/<id>/gl/): blob address, libgles_cm ordinals, the ROM
# exports the blob calls, the driver identity strings. Output: build/roms/<id>/<fw>/sygl/sygl.bin (raw,
# linked at the blob address), sygl.elf (symbols for gdb), sygl.map; models/<id>/gl/gles-patches.txt
# (regenerated). tools/device.py build puts the blob into the composed ROM when sygl.bin exists.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
OPT=(-O2); DEV="${SYBORG_DEVICE:-}"
while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEV="$2"; shift ;;
    --debug) OPT=(-O0 -g) ;;
    --trace) TRACE=1 ;;		# SYGL: trace lines (first 700 EGL/GL calls per thread, draw boxes)
  esac; shift
done
DPY=(python3 -I "$ROOT/tools/device.py")
export SYBORG_DEVICE="$DEV"
DEVDIR="$("${DPY[@]}" paths dir)"
pf() { "${DPY[@]}" file "$1"; }    # a profile file: the firmware overlay's, else the model's
SYGL_BASE=$(printf '0x%x' "$("${DPY[@]}" get gl.blob_addr)")   # free gap in the ROM window, see romcompose --blob
OUT="$DEVDIR/sygl"; GEN="$OUT/gen"
mkdir -p "$GEN"
# the ROM's export addresses (tools/syb_sym.py builds the index once, from the SDK's import libraries)
if [ ! -f "$DEVDIR/symindex.json" ]; then
  S60_SDK_LIB="${S60_SDK_LIB:-$SDK/release/armv5/lib}" python3 -I "$ROOT/tools/syb_sym.py" reindex
fi
# the device's driver identity (apps choose their GL path by these strings: H-gles-identity)
"${DPY[@]}" get gl.identity | python3 -c "
import json, sys
d = json.load(sys.stdin)
for k, m in (('vendor', 'GL_VENDOR'), ('renderer', 'GL_RENDERER'), ('version', 'GL_VERSION'),
             ('egl_vendor', 'EGL_VENDOR'), ('egl_version', 'EGL_VERSION')):
    print('#define SYGL_%s %s' % (m, json.dumps(d[k])))" > "$GEN/identity.h"
[ -n "${TRACE:-}" ] && OPT+=(-DSY_TRACE_MAX=700)

python3 -I "$ROOT/tools/gen-gles-glue.py" --blob-addr $SYGL_BASE \
  --ordinals "$(pf gl/libgles_cm.ordinals)" --imports-list "$(pf gl/n95imports.txt)" \
  --img "$DEVDIR/core-flat.img" --symindex "$DEVDIR/symindex.json" \
  --patches "$(pf gl/gles-patches.txt)" --asm "$GEN/entry.S" --names "$GEN/names.c" \
  --imports "$GEN/n95imports.h" --imports-asm "$GEN/imports.S"

# portable core: C; floats in core registers (Symbian ARMV5 ABI, softfp), arithmetic on the ARM1136 VFP
COMMON=(-c -march=armv5t -marm -mfloat-abi=softfp -mfpu=vfp "${OPT[@]}" -ffreestanding -fno-builtin -fno-common
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
for c in egl gl light tex raster hw libc; do
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
