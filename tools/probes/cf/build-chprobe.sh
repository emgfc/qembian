#!/usr/bin/env bash
# Builds build/n95apps/chprobe.exe against the S60 3rd FP1 (Symbian 9.2) SDK, so import
# ordinals match the N95's 9.2 DLLs. Host tools only: GCC 14.2 symbianelf + elf2e32_next.
# Usage: src/n95apps/chprobe/build.sh [--dump]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
LIBDIR="$HERE/libs"
mkdir -p "$LIBDIR"
for d in "$SDK/release/armv5/lib/"*.dso; do ln -sf "$d" "$LIBDIR/"; done
for d in ${SYMBIAN3_SDK:-$HOME/work/Nokia_Symbian3_SDK_v1.0}/epoc32/release/armv5/lib/sensrv*.dso; do ln -sf "$d" "$LIBDIR/"; done
OUT="$ROOT/build/n95apps"
OBJ="$OUT/obj-chprobe"
mkdir -p "$OUT" "$OBJ"

UID3=0xE0001DBD
# the SDK preinclude uses a DOS path (#include "..\symcpp.h"); use a fixed copy
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
CXXFLAGS=(
  -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft -O2 -std=gnu++98
  -nostdinc -fexceptions -fno-rtti -fno-threadsafe-statics -fshort-wchar
  -fno-strict-aliasing -fno-builtin -Wno-attributes -Wno-narrowing -Wno-return-local-addr
  -D__SYMBIAN32__ -DWGP_BUILD=\"${WGP_BUILD:-dev}\" -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__
  -D__EXE__ -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os.hrh\""
  -include "$OBJ/gcce_fixed.h"
  -I"$SDK/include" -I"${SYMBIAN3_SDK:-$HOME/work/Nokia_Symbian3_SDK_v1.0}/epoc32/include" -I"$SDK/include/variant" -I"$SDK/include/gcce"
)

echo ">> compile"
"$GCC-g++" "${CXXFLAGS[@]}" -o "$OBJ/chprobe.o" "$HERE/chprobe.cpp"
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/startup.s"

LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
DSOS=(sensrvclient sensrvutil euser scppnwdl drtaeabi dfpaeabi)
DSOPATHS=(); for d in "${DSOS[@]}"; do DSOPATHS+=("$LIBDIR/$d.dso"); done

echo ">> link"
"$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 \
  --default-symver --entry _E32Startup -u _E32Startup \
  -soname "chprobe{000a0000}[e0001dbd].exe" \
  "$OBJ/startup.o" "$OBJ/chprobe.o" "${DSOPATHS[@]}" "$LIBGCC" "$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-file-name=libsupc++.a)" "$LIBGCC" \
  -Map "$OBJ/chprobe.map" -o "$OBJ/chprobe.elf"

echo ">> elf2e32"
"$E2E" --elfinput="$OBJ/chprobe.elf" --output="$OUT/chprobe.exe" \
  --targettype=EXE --linkas="chprobe{000a0000}[e0001dbd].exe" \
  --uid1=0x1000007a --uid2=0x00000000 --uid3=$UID3 --sid=$UID3 --vid=0x0 \
  --capability=All-TCB --fpu=softvfp --heap=0x1000,0x100000 --stack=0x5000 \
  --libpath="$LIBDIR" --compressionmethod=none

ls -la "$OUT/chprobe.exe"
if [ "${1:-}" = "--dump" ]; then
  "$E2E" --dump=hsi --e32input="$OUT/chprobe.exe"
fi
