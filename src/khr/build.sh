#!/usr/bin/env bash
# Build our Khronos libraries for Symbian^3 phones. EGL and OpenVG run on the host (QEMU, src/syvg:
# docs/devices/nokia-n8-rm596/egl-vg-host-v2.md); these are the guest halves:
#   libEGL.dll   EGL at its SDK ordinals: OpenVG's EGL side forwarded to the host (egl2.inl, with the window
#                glue in eglos.cpp), OpenGL ES 1.x's from sygl (eglmux.cpp routes); every OpenVG/VGU function
#                at private ordinals in its unused slots, as generated stubs (tools/gen-vg2.py, vg2call.cpp);
#                OpenGL ES 1.1 (sygl) and 2.0 (gl2.c, remoted) as before
#   libOpenVG.dll, libOpenVGU.dll, libGLES*.dll   forwarders into libEGL (as Broadcom's to khmangle)
#   src/khr/build.sh            output: build/roms/<id>/<fw>/khr/*.dll (SYBORG_DEVICE = the device)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEV="${SYBORG_DEVICE:-}"
while [ $# -gt 0 ]; do case "$1" in --device) DEV="$2"; shift ;; esac; shift; done
export SYBORG_DEVICE="$DEV"
DPY=(python3 -I "$ROOT/tools/device.py")
DEVDIR="$("${DPY[@]}" paths dir)"
SDK="${S3SDK:-$HOME/work/Nokia_Symbian3_SDK_v1.0/epoc32}"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
LIB="$SDK/release/armv5/lib"
OUT="$DEVDIR/khr"; OBJ="$OUT/obj"; mkdir -p "$OBJ"
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
# the SDK's headers include <VG/...>, <EGL/...>, <KHR/...>; its directories are lower case
mkdir -p "$OBJ/inc"; for d in VG:vg EGL:egl KHR:khr; do ln -sfn "$SDK/include/${d#*:}" "$OBJ/inc/${d%%:*}"; done

COMMON=(-c -march=armv6k -marm -mfloat-abi=softfp -mfpu=vfp -O2 -fno-common -fno-unwind-tables
  -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fshort-wchar -fno-strict-aliasing
  -Wno-attributes -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__ -D__DLL__
  -D_UNICODE -DNDEBUG -D_UREL -D__OPENVGHEADERS_USE_VG_1_1
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os.hrh\"")
SDKINC=(-I"$OBJ/inc" -I"$SDK/include" -I"$SDK/include/platform" -I"$SDK/include/variant" -I"$SDK/include/gcce"
  -I"$SDK/include/vg" -I"$SDK/include/egl" -I"$SDK/include/khr" -I"$SDK/include/graphics" -I"$SDK/include/platform/graphics")
CXX=("$GCC-g++" "${COMMON[@]}" -std=gnu++98 -nostdinc -nostdinc++ -fno-rtti -fno-exceptions -fno-threadsafe-statics
  -include "$OBJ/gcce_fixed.h" -I"$HERE/shim" "${SDKINC[@]}" -isystem "$("$GCC-gcc" -print-file-name=include)")

# --- OpenVG/VGU: generated stubs, every call to the host (tools/gen-vg2.py; the host side is src/syvg)
python3 -I "$ROOT/tools/gen-vg2.py" "$ROOT/external/oss.FCL.sf.os.graphics/openvg/openvginterface/include/1.1/openvg.h" \
  "$ROOT/external/oss.FCL.sf.os.graphics/openvg/openvginterface/include/1.1/vgu.h" "$OBJ" "$OBJ/vg2host" >/dev/null
"$GCC-gcc" -c -march=armv6k -marm -mfloat-abi=softfp -mfpu=vfp -O2 -std=gnu99 -ffreestanding -fno-builtin \
  -include "$OBJ/gcce_fixed.h" -D__OPENVGHEADERS_USE_VG_1_1 "${SDKINC[@]}" -I"$ROOT/src/syvg" -o "$OBJ/vg2_gen.o" "$OBJ/vg2_gen.c"
"${CXX[@]}" -D__OPENVGHEADERS_USE_VG_1_1 -o "$OBJ/vg2call.o" "$HERE/vg2call.cpp"
VGOBJS=("$OBJ/vg2_gen.o" "$OBJ/vg2call.o")


# --- generated export tables and forwarders
DEFS="$SDK/include/def/eabi"
# OpenGL ES 1.1 on the host (docs/devices/nokia-n8-rm596/gles1-host.md): its function table and generated
# marshalling, and which function each libGLES export goes to (host halves: committed in src/qemu-syborg)
mkdir -p "$OBJ/sygl"
python3 -I "$ROOT/tools/gen-gles.py" "$SDK/include" "$OBJ/sygl" "$OBJ/sygl"   # host halves: committed in src/qemu-syborg
python3 -I "$HERE/gen_khr.py" "$DEFS" "$OBJ" - "$OBJ/sygl/gles_alias.txt"
# headers for the SF surface update client (not in the SDK): <graphics/surfaceupdateclient.h>, <graphics/suerror.h>
SU="$ROOT/external/oss.FCL.sf.os.graphics/graphicscomposition/surfaceupdate"
mkdir -p "$OBJ/sfinc/graphics"; for h in surfaceupdateclient.h suerror.h; do ln -sfn "$SU/inc/$h" "$OBJ/sfinc/graphics/$h"; done
# TSurfaceConfiguration (REglWindowBase::SetBackgroundSurface; the SDK's w32std.h only declares it)
WS="$ROOT/external/oss.FCL.sf.os.graphics/windowing/windowserver"
ln -sfn "$WS/inc/Graphics/surfaceconfiguration.h" "$OBJ/sfinc/graphics/surfaceconfiguration.h"
ln -sfn "$ROOT/external/oss.FCL.sf.os.graphics/windowing/windowserver/inc/Graphics/openwfc/surfaceconfiguration.inl" "$OBJ/sfinc/graphics/surfaceconfiguration.inl"
# import library for the ROM's surfaceupdateclient.dll (UID3 from the ROM; the SDK has none), from SF's def
"$E2E" --definput="$SU/eabi/surfaceupdateclientU.def" --dso="$OBJ/surfaceupdateclient.dso" \
  --linkas="surfaceupdateclient{000a0000}[10282ff1].dll" --targettype=IMPLIB >/dev/null 2>&1 \
  || "$E2E" --definput="$SU/eabi/surfaceupdateclientU.def" --dso="$OBJ/surfaceupdateclient.dso" \
       --linkas="surfaceupdateclient{000a0000}[10282ff1].dll" >/dev/null

# --- ours
"${CXX[@]}" -I"$OBJ/sfinc" -Wno-deprecated -o "$OBJ/eglos.o" "$HERE/eglos.cpp"
"${CXX[@]}" -o "$OBJ/hostgl.o" "$HERE/hostgl.cpp"
"$GCC-gcc" -c -march=armv6k -marm -O2 -ffreestanding -fno-builtin -o "$OBJ/support.o" "$HERE/support.c"

# --- OpenGL ES 1.1 and 2.0: forwarding only, everything on the host (docs/devices/nokia-n8-rm596/gles-host.md)
SYGEN="$OBJ/sygl"; mkdir -p "$SYGEN"
SYC=("$GCC-gcc" -c -march=armv6k -marm -mfloat-abi=softfp -mfpu=vfp -O2 -std=gnu99 -ffreestanding -fno-builtin
     -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -fsingle-precision-constant -ffunction-sections
     -fdata-sections -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -I"$SYGEN")
SYOBJS=()
for f in gles gles_stubs gles_procs; do
  src="$HERE/$f.c"; [ -f "$SYGEN/$f.c" ] && src="$SYGEN/$f.c"
  "${SYC[@]}" -fvisibility=default -I"$HERE" -o "$OBJ/$f.o" "$src"; SYOBJS+=("$OBJ/$f.o")   # khr1_*/khr2_*
done
"$GCC-as" -march=armv6k -o "$OBJ/startup.o" "$HERE/startup.s"
"$GCC-as" -march=armv6k -o "$OBJ/khr_alias.o" "$OBJ/khr_alias.S"
LIBGCC="$("$GCC-gcc" -march=armv6k -mfloat-abi=softfp -mfpu=vfp -print-libgcc-file-name)"

# one import-library directory: elf2e32_next's --libpath loop never advances past the first entry
# (patches/elf2e32_next/0002-libpath-search.patch), so every .dso it may look up is linked into it
DSODIR="$OBJ/dso"; mkdir -p "$DSODIR"; ln -sfn "$OBJ/surfaceupdateclient.dso" "$DSODIR/"

dll() {   # name uid3 deffile objs... -- dsos...
  local name=$1 uid=$2 def=$3; shift 3
  local objs=() dsos=()
  while [ "$1" != -- ]; do objs+=("$1"); shift; done; shift
  for d in "$@"; do case "$d" in /*) dsos+=("$d") ;; *) dsos+=("$LIB/$d.dso") ;; esac; done
  for d in "${dsos[@]}"; do ln -sfn "$d" "$DSODIR/$(basename "$d")"; done
  for d in "$LIB"/*.dso; do [ -e "$DSODIR/$(basename "$d")" ] || ln -s "$d" "$DSODIR/"; done
  local link="$name{000a0000}[${uid#0x}].dll"
  # only the .def's names are dynamic: the RI's API functions are dllexport'ed by the SDK headers,
  # and extra dynamic symbols make elf2e32_next loop
  { echo "KHR { global:"; grep -o "^\s*[A-Za-z_][A-Za-z0-9_]* @" "$def" | grep -v absent_export | sed 's/ @/;/'; echo "local: *; };"; } > "$OBJ/$name.vers"
  "$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 \
    --version-script "$OBJ/$name.vers" --entry _E32Dll -u _E32Dll -soname "$link" \
    "${objs[@]}" "${dsos[@]}" "$LIBGCC" -Map "$OBJ/$name.map" -o "$OBJ/$name.elf"
  "$E2E" --elfinput="$OBJ/$name.elf" --output="$OUT/$name.dll" --targettype=DLL --linkas="$link" \
    --uid1=0x10000079 --uid2=0x1000008d --uid3=$uid --sid=$uid --vid=0x0 --capability="All-TCB" \
    --fpu=softvfp --definput="$def" --dso="$OUT/$name.dso" --dlldata \
    --libpath="$DSODIR" --compressionmethod=none >"$OBJ/$name.e2e.log" 2>&1 \
    || { cat "$OBJ/$name.e2e.log"; exit 1; }
  echo "khr: $OUT/$name.dll $(stat -c %s "$OUT/$name.dll") bytes"
}
dll libEGL 0x10281ab8 "$OBJ/libEGL.def" "$OBJ/startup.o" "${VGOBJS[@]}" "$OBJ/eglos.o" "$OBJ/hostgl.o" \
  "${SYOBJS[@]}" "$OBJ/support.o" "$OBJ/khr_alias.o" -- euser ws32 fbscli surfacemanager libm scppnwdl drtaeabi \
  "$OBJ/surfaceupdateclient.dso"
# forwarders: OpenVG, and OpenGL ES 1.x (libGLES_CM = S60 3rd edition's EGL + GL; libGLESv1_CM = GL)
for v in libOpenVG:0x10281ab9 libOpenVGU:0x10281c47 libGLES_CM:0x101fcabd libGLESv1_CM:0x102836be libGLESv2:0x10285a7a; do
  n=${v%%:*}
  "$GCC-as" -march=armv6k -o "$OBJ/$n.o" "$OBJ/$n.S"
  dll $n ${v#*:} "$OBJ/$n.def" "$OBJ/startup.o" "$OBJ/$n.o" "$OBJ/support.o" -- "$OUT/libEGL.dso" euser
done
