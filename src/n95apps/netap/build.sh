#!/usr/bin/env bash
# Builds netap.exe (src/n95apps/netap/netap.cpp) into build/n95apps/: the LAN access point for syborg-net and a
# connection test. Symbian^3 SDK headers and import libraries (CommDb, ESock); toolchain as eglprobe.
# Install on a running phone: sydb push build/n95apps/netap.exe 'c:\sys\bin\netap.exe'
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
S3="${S3SDK:-$HOME/work/Nokia_Symbian3_SDK_v1.0/epoc32}"
LIB="$S3/release/armv5/lib"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
OUT="$ROOT/build/n95apps"; OBJ="$OUT/obj-netap"
mkdir -p "$OUT" "$OBJ"
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$S3/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/../devagent/startup.s"
"$GCC-g++" -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft -O2 -std=gnu++98 \
  -nostdinc -fexceptions -fno-rtti -fno-threadsafe-statics -fshort-wchar -fno-strict-aliasing -fno-builtin \
  -Wno-attributes -Wno-narrowing -fpermissive -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__ \
  -D__EXE__ -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL \
  -D__PRODUCT_INCLUDE__="\"$S3/include/variant/symbian_os.hrh\"" -include "$OBJ/gcce_fixed.h" \
  -I"$S3/include" -I"$S3/include/platform" -I"$S3/include/mw" -I"$S3/include/platform/mw" -I"$S3/include/app" -I"$S3/include/variant" -I"$S3/include/gcce" -o "$OBJ/netap.o" "$HERE/netap.cpp"
# elf2e32 opens each import as <DLL name>{000a0000}.dso in the DLL's own case (ConnMon): a lib dir with those names
LIBS="$OBJ/lib"; rm -rf "$LIBS"; mkdir -p "$LIBS"; ln -s "$LIB"/* "$LIBS/"
ln -sf "$LIB/connmon{000a0000}.dso" "$LIBS/ConnMon{000a0000}.dso"
LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
# netap.exe (own SID) and netallow.exe: the same program with SysAp's SID 0x100058F3, the only writer of
# CenRep 0x101F876C key 1 (network connection allowed; docs/hacks.md H-net-offline-allowed): `netallow allow 1`
for v in netap:e0001dc4 netallow:100058f3; do
  NAME=${v%%:*}; SID=${v#*:}
  LINK="$NAME{000a0000}[$SID].exe"
  "$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 --default-symver \
    --entry _E32Startup -u _E32Startup -soname "$LINK" "$OBJ/startup.o" "$OBJ/netap.o" \
    "$LIB/euser.dso" "$LIB/esock.dso" "$LIB/insock.dso" "$LIB/commdb.dso" "$LIB/centralrepository.dso" "$LIB/cmmanager.dso" "$LIB/connmon.dso" "$LIB/etel.dso" "$LIB/etelmm.dso" "$LIB/etelpckt.dso" "$LIB/scppnwdl.dso" \
    "$LIB/drtaeabi.dso" "$LIB/dfpaeabi.dso" "$(dirname "$GCC")/../arm-none-symbianelf/lib/libsupc++.a" "$LIBGCC" -o "$OBJ/$NAME.elf"
  "$E2E" --elfinput="$OBJ/$NAME.elf" --output="$OUT/$NAME.exe" --targettype=EXE --linkas="$LINK" \
    --uid1=0x1000007a --uid2=0x100039ce --uid3=0x$SID --sid=0x$SID --vid=0x0 --capability=All-TCB \
    --fpu=softvfp --heap=0x1000,0x100000 --stack=0x8000 --libpath="$LIBS" --compressionmethod=none | grep -i error && exit 1 || true
done
ls -la "$OUT/netap.exe" "$OUT/netallow.exe"
