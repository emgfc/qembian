#!/usr/bin/env bash
# Builds the sydb agent pair against the S60 3rd FP1 (Symbian 9.2) SDK (GCC 14.2 symbianelf +
# elf2e32_next, host only):
#   build/n95apps/devagent.exe  All-TCB, links WSERV/apparc/avkon (agent, serial unit 1)
#   build/n95apps/dafs.exe      All,     links EUSER/EFSRV only (autostart launcher + \sys mover)
# Usage: src/n95apps/devagent/build.sh [--debug]   (--debug: -O0 -g, keeps the ELF for gdb)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
LIBDIR="$SDK/release/armv5/lib"
OUT="$ROOT/build/n95apps"
OBJ="$OUT/obj-devagent"
mkdir -p "$OUT" "$OBJ"
OPT=(-O2)
[ "${1:-}" = "--debug" ] && OPT=(-O0 -g)
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
CXXFLAGS=(
  -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft "${OPT[@]}" -std=gnu++98
  -nostdinc -fno-exceptions -fno-rtti -fno-threadsafe-statics -fshort-wchar
  -fno-strict-aliasing -fno-builtin -Wno-attributes -Wno-narrowing -Wno-return-local-addr
  -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__
  -D__EXE__ -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os_v9.2.hrh\""
  -include "$OBJ/gcce_fixed.h"
  -I"$SDK/include" -I"$SDK/include/variant" -I"$SDK/include/gcce"
)
LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/startup.s"

build() {   # name uid3 caps dsos...
  local name=$1 uid=$2 caps=$3; shift 3
  local dsopaths=(); for d in "$@"; do dsopaths+=("$LIBDIR/$d.dso"); done
  local link="$name{000a0000}[${uid#0x}].exe"
  "$GCC-g++" "${CXXFLAGS[@]}" -o "$OBJ/$name.o" "$HERE/$name.cpp"
  "$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 \
    --default-symver --entry _E32Startup -u _E32Startup -soname "$link" \
    "$OBJ/startup.o" "$OBJ/$name.o" "${dsopaths[@]}" "$LIBGCC" -Map "$OBJ/$name.map" -o "$OBJ/$name.elf"
  "$E2E" --elfinput="$OBJ/$name.elf" --output="$OUT/$name.exe" --targettype=EXE --linkas="$link" \
    --uid1=0x1000007a --uid2=0x00000000 --uid3=$uid --sid=$uid --vid=0x0 \
    --capability="$caps" --fpu=softvfp --heap=0x1000,0x400000 --stack=0x8000 \
    --libpath="$LIBDIR" --compressionmethod=none >/dev/null
  ls -la "$OUT/$name.exe"
}
build devagent 0xE0001DB7 "All-TCB" euser efsrv ws32 apgrfx apparc avkon scppnwdl drtaeabi dfpaeabi
build dafs 0xE0001DB8 "All" euser efsrv scppnwdl drtaeabi dfpaeabi
