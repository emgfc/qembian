#!/usr/bin/env bash
# Builds the sydb agent pair against the S60 3rd FP1 (Symbian 9.2) SDK (GCC 14.2 symbianelf +
# elf2e32_next, host only):
#   build/n95apps/devagent.exe  All-TCB, links WSERV/apparc/avkon (agent, serial unit 1)
#   build/n95apps/dafs.exe      All,     links EUSER/EFSRV only (\sys mover, agent self-update)
#   build/n95apps/boothook.exe  All-TCB, links EUSER only, SID = EStart's (boot hook, installed on C: as
#                               HALSettings.exe or LanguageSelection.exe, docs/agent-autostart.md)
#   build/n95apps/oracle.exe    All-TCB, real-phone boot recorder started by the hook (docs/real-phone-tracing.md)
#   build/n95apps/sisinst.exe   All-TCB, installs a .sis/.sisx answering and logging the installer's dialogs
#                               (`sydb sis install`); links sishelper.dll through sishelper.def
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
# elf2e32 finds each import as <name>{000a0000}.dso in one directory (a "a;b" list hangs elf2e32_next):
# the SDK's libraries plus the ones we make (sishelper)
LIBS="$OBJ/lib"
rm -rf "$LIBS"; mkdir -p "$LIBS"; ln -s "$LIBDIR"/* "$LIBS/"

build() {   # name uid3[:sid] caps dsos...
  local name=$1 uid=${2%%:*} sid=${2#*:} caps=$3; shift 3
  local dsopaths=(); for d in "$@"; do case $d in */*) dsopaths+=("$d");; *) dsopaths+=("$LIBDIR/$d.dso");; esac; done
  local link="$name{000a0000}[${uid#0x}].exe"
  "$GCC-g++" "${CXXFLAGS[@]}" -o "$OBJ/$name.o" "$HERE/$name.cpp"
  "$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 \
    --default-symver --entry _E32Startup -u _E32Startup -soname "$link" \
    "$OBJ/startup.o" "$OBJ/$name.o" "${dsopaths[@]}" "$LIBGCC" -Map "$OBJ/$name.map" -o "$OBJ/$name.elf"
  "$E2E" --elfinput="$OBJ/$name.elf" --output="$OUT/$name.exe" --targettype=EXE --linkas="$link" \
    --uid1=0x1000007a --uid2=0x00000000 --uid3=$uid --sid=$sid --vid=0x0 \
    --capability="$caps" --fpu=softvfp --heap=0x1000,0x400000 --stack=0x8000 \
    --libpath="$LIBS" --compressionmethod=none >/dev/null
  ls -la "$OUT/$name.exe"
}
build devagent 0xE0001DB7 "All-TCB" euser efsrv ws32 apgrfx apparc avkon scppnwdl drtaeabi dfpaeabi
build dafs 0xE0001DB8 "All" euser efsrv scppnwdl drtaeabi dfpaeabi
build boothook 0xE0001DB9:0x10272C04 "All-TCB" euser scppnwdl drtaeabi dfpaeabi
build oracle 0xE0001DBA "All-TCB" euser efsrv hal scppnwdl drtaeabi dfpaeabi
# sishelper.dll (UID3 0x101F7293) has no import library in the 9.2 SDK: make one from our two-entry DEF
"$E2E" --definput="$HERE/sishelper.def" --dso="$OBJ/sishelper.dso" --linkas="sishelper{000a0000}[101f7293].dll" >/dev/null
ln -sf "$OBJ/sishelper.dso" "$LIBS/sishelper{000a0000}.dso"; ln -sf "$OBJ/sishelper.dso" "$LIBS/sishelper.dso"
build sisinst 0xE0001DBB "All-TCB" euser efsrv "$OBJ/sishelper.dso" scppnwdl drtaeabi dfpaeabi
