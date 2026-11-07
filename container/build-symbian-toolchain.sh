#!/usr/bin/env bash
# Build the guest-side toolchain inside container/Containerfile.modern (stage "toolchain"):
#   /opt/symbianelf  GCC 14.2 + binutils 2.29.1 for arm-none-symbianelf (C/C++ compilers and libgcc
#                    only: our guest code is -nostdlib/-nostdinc and links libgcc and SDK .dso files)
#   /opt/elf2e32     elf2e32_next (ELF -> E32Image post-linker) with patches/elf2e32_next/*.patch
# Inputs (versions, URLs, sha256, commit) are passed by the Containerfile from sources.lock values.
#   build-symbian-toolchain.sh gcc | elf2e32     (two image layers: an elf2e32 change keeps GCC cached)
set -euo pipefail
J=$(nproc)
T=arm-none-symbianelf
P=/opt/symbianelf
W=/tmp/tc; mkdir -p "$W"; cd "$W"
if [ "${1:-}" = gcc ]; then
: "${BINUTILS_URL:?}" "${BINUTILS_SHA256:?}" "${GCC_URL:?}" "${GCC_SHA256:?}"

fetch() {	# url sha256
  curl -fsSL -o "$(basename "$1")" "$1"
  echo "$2  $(basename "$1")" | sha256sum -c -
}
fetch "$BINUTILS_URL" "$BINUTILS_SHA256"
fetch "$GCC_URL" "$GCC_SHA256"
tar xf "$(basename "$BINUTILS_URL")"; tar xf "$(basename "$GCC_URL")"
BU=$(basename "$BINUTILS_URL" | sed 's/\.tar\..*//'); GC=$(basename "$GCC_URL" | sed 's/\.tar\..*//')

# binutils 2.29.1: the last series that knows arm*-symbianelf well; 2017 code, so a modern host
# compiler needs its new errors turned back into warnings. gold is not needed.
mkdir b-bu && cd b-bu
CFLAGS="-O2 -std=gnu11 -Wno-error -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion" \
CXXFLAGS="-O2 -std=gnu++11 -Wno-error" \
  "../$BU/configure" --target=$T --prefix=$P --disable-nls --disable-shared --disable-werror \
    --enable-plugins --enable-lto --enable-multilib --disable-gold > ../binutils-configure.log
make -j"$J" > ../binutils-make.log 2>&1 || { tail -40 ../binutils-make.log; exit 1; }
make install-strip > /dev/null
cd ..

# GCC 14.2 knows arm*-*-symbianelf. Three source edits, the same facts the community GCC4Symbian
# recipe applies (written here independently):
#  1. predefine __SYMBIAN32__ next to __symbian__ (SDK headers test it);
#  2. GCC's own stdint.h for the target (newlib-stdint.h, use_gcc_stdint=provide): no C library
#     headers exist at build time;
#  3. no crtfastmath.o for the arm ELF targets in libgcc (Symbian provides its own start-up code).
S=$GC/gcc/config/arm/symbian.h
sed -i 's|^\(\s*\)builtin_define ("__symbian__");\(\s*\\\)$|&\n\1builtin_define ("__SYMBIAN32__");\2|' "$S"
grep -q '__SYMBIAN32__' "$S"
C=$GC/gcc/config.gcc
python3 - "$C" "$GC/libgcc/config.host" <<'EOF'
import re, sys
c, h = sys.argv[1], sys.argv[2]
t = open(c).read()
i = t.index('arm*-*-symbianelf*)\n\t  tm_file="${tm_file} arm/symbian.h"')
t = t[:i] + t[i:].replace('arm/symbian.h"', 'arm/symbian.h newlib-stdint.h"\n\t  use_gcc_stdint=provide', 1)
open(c, 'w').write(t)
t = open(h).read()
i = t.index('arm*-*-symbianelf*)\n\t  tmake_file="${tmake_file} arm/t-symbian')
j = t.index('extra_parts="$extra_parts crtfastmath.o"', i)
t = t[:j] + 'extra_parts="$extra_parts"' + t[j + len('extra_parts="$extra_parts crtfastmath.o"'):]
open(h, 'w').write(t)
EOF
mkdir b-gcc && cd b-gcc
export PATH=$P/bin:$PATH
"../$GC/configure" --target=$T --prefix=$P --without-headers --enable-languages=c,c++ \
  --enable-interwork --enable-multilib --enable-long-long --enable-wchar_t --with-dwarf2 \
  --disable-shared --disable-threads --disable-nls --disable-libssp --disable-libquadmath \
  --disable-libstdcxx --disable-hosted-libstdcxx --disable-libstdcxx-pch > ../gcc-configure.log
make -j"$J" all-gcc all-target-libgcc > ../gcc-make.log 2>&1 || { tail -40 ../gcc-make.log; exit 1; }
make install-strip-gcc install-strip-target-libgcc > /dev/null
cd ..
"$P/bin/$T-gcc" --version | head -1
echo 'int f(int a, int b) { return a / b; }' > t.c
"$P/bin/$T-gcc" -march=armv5t -mfloat-abi=soft -c t.c -o t.o
test -f "$("$P/bin/$T-gcc" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
cd /; rm -rf "$W"; exit 0
fi

: "${ELF2E32_URL:?}" "${ELF2E32_COMMIT:?}"

# elf2e32_next at the pinned commit + our patches; every .c/.cpp under lib/ and src/ (its Code::Blocks
# project lists a file that is not in the tree). 2020-era code: modern g++ needs <cstdint> and
# -fpermissive (two old idioms); output equals the build the project used (E32 header time aside).
git clone -q "$ELF2E32_URL" e2e && cd e2e && git checkout -q "$ELF2E32_COMMIT"
for p in /src/patches/elf2e32_next/*.patch; do patch -p1 -s < "$p"; done
SRCS=$(find lib src -name '*.c' -o -name '*.cpp' | sort)
mkdir -p "/opt/elf2e32/bin" obj
OBJS=()
for s in $SRCS; do
  o=obj/$(echo "$s" | tr / _).o
  case "$s" in *.c) cc -O2 -Iinclude -Isrc -Ilib/elf -Ilib/e32 -Ilib/getopt -c "$s" -o "$o" ;;
                *)  c++ -O2 -std=c++14 -include cstdint -fpermissive -w -Iinclude -Isrc -Ilib/elf -Ilib/e32 -Ilib/getopt -c "$s" -o "$o" ;; esac
  OBJS+=("$o")
done
c++ -o /opt/elf2e32/bin/elf2e32_next "${OBJS[@]}" -static-libstdc++ -static-libgcc
/opt/elf2e32/bin/elf2e32_next --help > /dev/null 2>&1 || true
ls -l /opt/elf2e32/bin/elf2e32_next
cd /; rm -rf "$W"
