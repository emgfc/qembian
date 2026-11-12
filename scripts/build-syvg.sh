#!/usr/bin/env bash
# syvg: the Khronos OpenVG 1.1 RI front end as a host static library for QEMU's syborg-gl device
# (docs/devices/nokia-n8-rm596/egl-vg-host-v2.md). RI sources from the pinned graphics repo, host headers in
# src/syvg/inc (the Khronos VG headers are copied from the repo with VGHandle pointer-sized: the host is
# 64-bit; the guest's 32-bit handles are mapped by the device), patches in src/syvg/patches.
# Output: build/syvg/libsyvg.a (linked by scripts/build-qemu-modern.sh).
set -euo pipefail
# SYVG_CXX / SYVG_AR / SYVG_OUT: another compiler and output (android/native/build.sh: the NDK, build/android/syvg)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
G=external/oss.FCL.sf.os.graphics
RISRC=$G/openvg/openvgrefimplementation/sfopenvg/sfopenvg
B="${SYVG_OUT:-build/syvg}"; rm -rf "$B/ri" "$B/inc" "$B/obj"; mkdir -p "$B/obj" "$B/inc/VG/1.1" "$B/inc/VG/1.0"
cp -r "$RISRC" "$B/ri"
for p in src/syvg/patches/*.patch; do [ -e "$p" ] && patch -s -p1 -d "$B/ri" < "$p"; done
cp -r src/syvg/inc/* "$B/inc/"
cp $G/openvg/openvginterface/include/openvg.h $G/openvg/openvginterface/include/vgu.h "$B/inc/VG/"
cp $G/openvg/openvginterface/include/1.1/*.h "$B/inc/VG/1.1/"
cp $G/openvg/openvginterface/include/1.0/*.h "$B/inc/VG/1.0/"
mkdir -p "$B/inc/EGL"; cp $G/egl/eglinterface/include/1.4/egl.h $G/egl/eglinterface/include/1.4/eglext.h "$B/inc/EGL/"
sed -i 's/^typedef VGuint VGHandle;/#include <stdint.h>\ntypedef uintptr_t VGHandle;  \/* host build: pointer-sized *\//' "$B/inc/VG/1.1/openvg.h"
CXX=(${SYVG_CXX:-g++ -m64} -c -O2 -fPIC -std=gnu++11 -fno-exceptions -Wno-deprecated -D__OPENVGHEADERS_USE_VG_1_1 -DNDEBUG
     -I"$B/ri" -I"$B/ri/include" -I"$B/inc" -I"$B/inc/VG" -Isrc/syvg)
# the RI's own rasteriser is replaced by vgraster.cpp (fills on the GPU); vghost.cpp is the RI's drawables,
# images and targets on syborg-gl (both ported from the v1 guest code, src/khr)
# the OpenVG remoting (function table, host dispatch) from the Khronos headers
python3 -I tools/gen-vg2.py $G/openvg/openvginterface/include/1.1/openvg.h $G/openvg/openvginterface/include/1.1/vgu.h \
  "$B/guest" src/syvg >/dev/null
# MiniEGL's eglX become syvg_eglX: the QEMU process also loads the host's own EGL (epoxy)
RIEGL=($(grep -o 'EGLAPIENTRY egl[A-Za-z]*' "$B/inc/EGL/egl.h" | awk '{print $2}' | sort -u | sed 's/.*/-D&=syvg_&/'))
for f in riApi riContext riFont riImage riMath riPath riPixelPipe riVGU riMiniEGL; do
  "${CXX[@]}" "${RIEGL[@]}" -include src/syvg/syvg_shim.h -o "$B/obj/$f.o" "$B/ri/$f.cpp"
done
for f in vghost vgraster vg2 syvg_egl syvg_eglos; do
  "${CXX[@]}" "${RIEGL[@]}" -include src/syvg/syvg_shim.h -o "$B/obj/$f.o" src/syvg/$f.cpp
done
rm -f "$B/libsyvg.a"; ${SYVG_AR:-ar} rcs "$B/libsyvg.a" "$B"/obj/*.o
ls -l "$B/libsyvg.a"
