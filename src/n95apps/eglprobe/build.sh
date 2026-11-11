#!/usr/bin/env bash
# Builds eglprobe.exe (no capabilities) and build/n95apps/eglprobe.sis (unsigned; makesis -c crashes), to be
# started from the application menu of a Symbian^3 phone (real or qembian). Toolchain as devagent (GCC 14.2
# symbianelf, elf2e32_next, S60 3rd FP1 headers); EGL/OpenVG/GLES import libraries from the Symbian^3 SDK.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
S3LIB="${S3SDK:-$HOME/work/Nokia_Symbian3_SDK_v1.0/epoc32}/release/armv5/lib"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
GNUPOC="${GNUPOC:-$HOME/work/gnupoc-package/tools}"
OUT="$ROOT/build/n95apps"; OBJ="$OUT/obj-eglprobe"
mkdir -p "$OUT" "$OBJ"
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/../devagent/startup.s"
"$GCC-g++" -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft -O2 -std=gnu++98 \
  -nostdinc -fno-exceptions -fno-rtti -fno-threadsafe-statics -fshort-wchar -fno-strict-aliasing -fno-builtin \
  -Wno-attributes -Wno-narrowing -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__ \
  -D__EXE__ -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL \
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os_v9.2.hrh\"" -include "$OBJ/gcce_fixed.h" \
  -I"$SDK/include" -I"$SDK/include/variant" -I"$SDK/include/gcce" -o "$OBJ/eglprobe.o" "$HERE/eglprobe.cpp"
LIBS="$OBJ/lib"; rm -rf "$LIBS"; mkdir -p "$LIBS"
ln -s "$SDK/release/armv5/lib"/* "$LIBS/"
# elf2e32 opens each import as <DLL name>{000a0000}.dso, in the DLL's own case (libEGL, libOpenVG, libGLESv1_CM)
for l in libegl:libEGL libopenvg:libOpenVG libglesv1_cm:libGLESv1_CM; do
  ln -sf "$S3LIB/${l%%:*}{000a0000}.dso" "$LIBS/${l#*:}{000a0000}.dso"; done
LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
LINK="eglprobe{000a0000}[e0001dc0].exe"
"$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 --default-symver \
  --entry _E32Startup -u _E32Startup -soname "$LINK" "$OBJ/startup.o" "$OBJ/eglprobe.o" \
  "$SDK/release/armv5/lib/euser.dso" "$SDK/release/armv5/lib/efsrv.dso" "$SDK/release/armv5/lib/scppnwdl.dso" \
  "$SDK/release/armv5/lib/drtaeabi.dso" "$SDK/release/armv5/lib/dfpaeabi.dso" \
  "$S3LIB/libegl{000a0000}.dso" "$S3LIB/libopenvg{000a0000}.dso" "$S3LIB/libglesv1_cm{000a0000}.dso" \
  "$LIBGCC" -o "$OBJ/eglprobe.elf"
"$E2E" --elfinput="$OBJ/eglprobe.elf" --output="$OUT/eglprobe.exe" --targettype=EXE --linkas="$LINK" \
  --uid1=0x1000007a --uid2=0x100039ce --uid3=0xE0001DC0 --sid=0xE0001DC0 --vid=0x0 --capability=None \
  --fpu=softvfp --heap=0x1000,0x400000 --stack=0x8000 --libpath="$LIBS" --compressionmethod=none | grep -i error && exit 1 || true
cpp -P -I"$SDK/include" "$HERE/eglprobe_reg.rss" -o "$OBJ/eglprobe_reg.rpp"
PATH="$PATH:$GNUPOC/petran-1.1.0/e32uid" "$GNUPOC/rcomp-7.0.1/src/rcomp" -o"$OUT/eglprobe_reg.rsc" \
  -s"$OBJ/eglprobe_reg.rpp" -u < /dev/null
sed "s#@OUT@#$OUT#g" "$HERE/eglprobe.pkg.in" > "$OBJ/eglprobe.pkg"
rm -f "$OUT/eglprobe.sis"     # makesis does not replace an existing file
(cd "$OBJ" && "$GNUPOC/makesis-4/src/makesis" eglprobe.pkg "$OUT/eglprobe.sis" >/dev/null)   # a leading / reads as an option
ls -la "$OUT/eglprobe.exe" "$OUT/eglprobe.sis"
