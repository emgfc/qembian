#!/usr/bin/env bash
# Append the ARM EABI integer-division helpers to the CSL 2009q3 arm-none-symbianelf libgcc.a
# (both multilibs). Runs inside the container; called by setup-epocroot.sh. Idempotent.
#
# Why: GCC's config/arm/t-symbian leaves the EABI integer helpers (__aeabi_{i,ui}div{,mod},
# __aeabi_{u,}ldivmod, __aeabi_{llsl,llsr,lasr,lmul,lcmp,ulcmp}) out of libgcc because on
# Symbian the OS runtime normally provides them (RVCT's libraries, re-exported by EKERN for
# kernel-side code via ekernu.def). The GCC_SURGE kernel
# build links kernel-side code with -lgcc and expects them there (the guide's CSL 2010q1
# toolchain had them; CSL 2012.03 does too). We build them from the GCC 4.4.1 sources that
# match this compiler (gcc/config/arm/lib1funcs.asm, bpabi.S, bpabi.c, gcc/libgcc2.c).
set -euo pipefail
T="${1:-/work/build/toolchains/arm-2009q3}"
SRC=/work/downloads/gcc-4.4.1-arm
P="$T/bin/arm-none-symbianelf-"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

while read -r dir flags; do
  lib="$("${P}gcc" $flags -print-libgcc-file-name)"
  [ -f "$lib.orig" ] || cp "$lib" "$lib.orig"
  cp "$lib.orig" "$lib"   # always rebuild from the pristine CSL archive
  if "${P}nm" "$lib" 2>/dev/null | grep -q ' T __aeabi_uldivmod$'; then
    echo "   $(basename "$(dirname "$lib")")/libgcc.a already has EABI division helpers"; continue
  fi
  o="$W/${dir//\//_}"; mkdir -p "$o"
  # No _dvmd_tls: Symbian provides __aeabi_idiv0/__aeabi_ldiv0 itself (kernel: common/arm/gccehlp.cpp,
  # user side: drtaeabi.dll), and a second definition breaks the ekern link.
  for L in udivsi3 divsi3 umodsi3 modsi3; do
    "${P}gcc" $flags -O2 -c -x assembler-with-cpp -I"$SRC" -DL_$L "$SRC/lib1funcs.asm" -o "$o/_$L.o"
  done
  "${P}gcc" $flags -c /work/scripts/libgcc-shim/div0.S -o "$o/_symbian_div0.o"
  # lib1funcs.asm only provides bpabi.S (64-bit divmod/cmp) and the dword shifts when
  # !__symbian__ (GCC inlines them for Symbian, but ekernu.def still exports them).
  for L in aeabi_ldivmod aeabi_uldivmod aeabi_lcmp aeabi_ulcmp lshrdi3 ashrdi3 ashldi3; do
    "${P}gcc" $flags -O2 -c -x assembler-with-cpp -U__symbian__ -I"$SRC" -DL_$L "$SRC/lib1funcs.asm" -o "$o/_$L.o"
  done
  "${P}gcc" $flags -O2 -fomit-frame-pointer -c "$SRC/bpabi.c" -o "$o/bpabi.o"
  # bpabi.c's helpers need the generic 64-bit routines, which CSL's symbianelf libgcc also
  # leaves empty (_divdi3.o/_udivdi3.o have no symbols). Build them from libgcc2.c with a
  # minimal target-config shim (scripts/libgcc-shim; checked by libgcc-shim/test/divtest.c).
  for L in udivmoddi4 divdi3 udivdi3 muldi3; do
    "${P}gcc" $flags -O2 -fomit-frame-pointer -DIN_LIBGCC2 -I/work/scripts/libgcc-shim -I"$SRC" \
        -DL_$L -c "$SRC/libgcc2.c" -o "$o/_$L.o"
  done
  "${P}ar" rs "$lib" "$o"/*.o
  echo "   patched $lib:"
  "${P}nm" "$lib" | grep -E ' [TA] (__aeabi_[a-z0-9]+|__u?divdi3|__udivmoddi4|__div0)$' | sed 's/^/     /'
done < <("${P}gcc" -print-multi-lib | tr ';@' '  ' | sed 's/  */ -/g; s/ -$//')
