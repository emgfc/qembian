#!/usr/bin/env bash
# Builds playfile.exe (src/n95apps/playfile/playfile.cpp) into build/n95apps/ against the S60 3rd FP1 (9.2) SDK; the
# MMF client API is binary compatible on later phones. Install: sydb push build/n95apps/playfile.exe 'c:\sys\bin\playfile.exe'
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
LIBDIR="$SDK/release/armv5/lib"
OUT="$ROOT/build/n95apps"; OBJ="$OUT/obj-playfile"
mkdir -p "$OUT" "$OBJ"
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
# the SDK's MMF headers include with backslashes and mixed case (<Mmf\Common\MmfBase.h>): compile, link each header
# the compiler misses under its literal name to the real file (case-insensitive), repeat
SHIM="$OBJ/bs"; mkdir -p "$SHIM"
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/../devagent/startup.s"
compile() { "$GCC-g++" -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft -O2 -std=gnu++98 \
  -nostdinc -fexceptions -fno-rtti -Wno-return-local-addr -fno-threadsafe-statics -fshort-wchar -fno-strict-aliasing -fno-builtin \
  -Wno-attributes -Wno-narrowing -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__ \
  -D__EXE__ -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL \
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os_v9.2.hrh\"" -include "$OBJ/gcce_fixed.h" \
  -I"$SDK/include" -I"$SDK/include/variant" -I"$SDK/include/gcce" -I"$SHIM" -o "$OBJ/playfile.o" "$HERE/playfile.cpp"; }
for i in $(seq 60); do
  if compile 2> "$OBJ/cc.err"; then break; fi
  miss=$(grep -oE 'fatal error: [^:]+: No such file' "$OBJ/cc.err" | sed -E 's/fatal error: (.*): No such file/\1/' | head -1)
  [ -n "$miss" ] || { cat "$OBJ/cc.err"; exit 1; }
  real=$(python3 -I -c 'import os,sys
root,want=sys.argv[1],sys.argv[2].replace(chr(92),"/").lower()
for d,_,fs in os.walk(root):
    for f in fs:
        p=os.path.join(d,f)
        if os.path.relpath(p,root).lower()==want: print(p); sys.exit()' "$SDK/include" "$miss")
  [ -n "$real" ] || { cat "$OBJ/cc.err"; exit 1; }
  ln -sf "$real" "$SHIM/$miss"
done
LIBS="$OBJ/lib"; rm -rf "$LIBS"; mkdir -p "$LIBS"; ln -s "$LIBDIR"/* "$LIBS/"
LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
LINK="playfile{000a0000}[e0001dc6].exe"
"$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 --default-symver \
  --entry _E32Startup -u _E32Startup -soname "$LINK" "$OBJ/startup.o" "$OBJ/playfile.o" \
  "$LIBDIR/euser.dso" "$LIBDIR/mediaclientaudio.dso" "$LIBDIR/scppnwdl.dso" "$LIBDIR/drtaeabi.dso" "$LIBDIR/dfpaeabi.dso" \
  "$(dirname "$GCC")/../arm-none-symbianelf/lib/libsupc++.a" "$LIBGCC" -o "$OBJ/playfile.elf"
"$E2E" --elfinput="$OBJ/playfile.elf" --output="$OUT/playfile.exe" --targettype=EXE --linkas="$LINK" \
  --uid1=0x1000007a --uid2=0x00000000 --uid3=0xe0001dc6 --sid=0xe0001dc6 --vid=0x0 --capability=MultimediaDD+UserEnvironment+ReadUserData \
  --fpu=softvfp --heap=0x1000,0x200000 --stack=0x8000 --libpath="$LIBS" --compressionmethod=none >/dev/null
ls -la "$OUT/playfile.exe"
