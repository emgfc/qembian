#!/usr/bin/env bash
# Assemble the EPOCROOT (build/epocroot) and unpack the GCCE toolchain (build/toolchains).
#
# Layout (mirrors the Symbian Foundation "sf/" layout the bld.inf files expect):
#   build/epocroot/epoc32/               PDK 4.0a binaries_epoc + tools_epoc (headers, templates)
#   build/epocroot/linux_build/          oss.FCL.interim.SFTools.LinuxBuild (Raptor + host tools)
#   build/epocroot/sf/os/kernelhwsrv/    kernel source (copy of pinned checkout)
#   build/epocroot/sf/adapt/qemu/        Syborg baseport + QEMU source (copy of pinned checkout)
#   build/toolchains/arm-2009q3/         CodeSourcery GCC 4.4.1 arm-none-symbianelf
#
# Prebuilt ARMV5 target binaries from the PDK are deliberately NOT unpacked, so that the
# ROM can only contain what we built ourselves.
#
#   scripts/setup-epocroot.sh [--force]   (--force: wipe and recreate build/epocroot)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  "$ROOT/scripts/in-container.sh" /work/scripts/setup-epocroot.sh "$@"
  # derived baseport variants (one kernel build per memory model, docs/devices.md §4), regenerated
  # from the patched syborg baseport every time so they never drift from it (host python3)
  python3 -I "$ROOT/tools/baseport_variant.py" syborgmv --memmodel moving --cpu arm926
  python3 -I "$ROOT/tools/baseport_variant.py" syborgfx --memmodel flexible --cpu arm1176 --unaligned
  python3 -I "$ROOT/tools/baseport_variant.py" syborg6k --memmodel multiple --cpu arm1176
  exit 0
fi
cd /work
E=/work/build/epocroot
PDK=/work/cache/pdk4.0a

[ "${1:-}" = --force ] && rm -rf "$E"

# --- PDK outer zip -> cache/pdk4.0a
if [ ! -f "$PDK/release_metadata.xml" ]; then
  mkdir -p cache/pdk-unzip
  unzip -q -o downloads/PDK_4_0a.zip -d cache/pdk-unzip
  rm -rf "$PDK"; mv cache/pdk-unzip/pdk/PDK_4.0.a "$PDK"; rm -rf cache/pdk-unzip
fi
# Verify inner archives against the md5s in the release metadata.
python2.6 - "$PDK" <<'EOF'
import sys, re, os, hashlib
d = sys.argv[1]
meta = open(os.path.join(d, "release_metadata.xml")).read()
for name in ("binaries_epoc.7z.zip", "tools_epoc.7z.zip"):
    want = re.search(r'md5checksum="([0-9a-fA-F]+)" name="%s"' % re.escape(name), meta).group(1).lower()
    h = hashlib.md5(); f = open(os.path.join(d, name), "rb")
    for chunk in iter(lambda: f.read(1 << 20), ""): h.update(chunk)
    assert h.hexdigest() == want, "md5 mismatch for " + name
    print "   %s: md5 OK" % name
EOF

# --- reference fixture: prebuilt PDK 4.0a Syborg ROMs (RVCT-built), for validating QEMU
#     independently of our own kernel build. -> cache/pdk-rom/epoc32/rom/
if [ ! -f /work/cache/pdk-rom/epoc32/rom/syborg_tshell_ARMV5_udeb.img ]; then
  python2.6 - "$PDK" <<'PYEOF'
import sys, re, os, hashlib
d = sys.argv[1]; name = "binaries_rom_images.7z.zip"
want = re.search(r'md5checksum="([0-9a-fA-F]+)" name="%s"' % re.escape(name), open(os.path.join(d, "release_metadata.xml")).read()).group(1).lower()
assert hashlib.md5(open(os.path.join(d, name), "rb").read()).hexdigest() == want, "md5 mismatch for " + name
PYEOF
  7z x -y -bd -o/work/cache/pdk-rom "$PDK/binaries_rom_images.7z.zip" 'epoc32/rom/syborg*' >/dev/null
  echo ">> fixture ROMs: $(ls /work/cache/pdk-rom/epoc32/rom/*.img /work/cache/pdk-rom/epoc32/rom/*.IMG | xargs -n1 basename | tr '\n' ' ')"
fi

# --- epoc32 tree
if [ ! -d "$E/epoc32/include" ]; then
  mkdir -p "$E"
  for a in binaries_epoc tools_epoc; do
    echo ">> unpacking $a"
    7z x -y -bd -o"$E" "$PDK/$a.7z.zip" >/dev/null
  done
fi

# --- source packages (copies, so builds never dirty the pinned checkouts)
sync_src() {  # src dst
  if [ ! -d "$2" ]; then
    echo ">> copying $(basename "$1") -> ${2#/work/}"
    mkdir -p "$(dirname "$2")"
    cp -a "$1" "$2"; rm -rf "$2/.git"
  fi
}
sync_src external/oss.FCL.interim.SFTools.LinuxBuild "$E/linux_build"
sync_src external/oss.FCL.sf.os.kernelhwsrv          "$E/sf/os/kernelhwsrv"
sync_src external/oss.FCL.sf.adapt.Qemu              "$E/sf/adapt/qemu"
# graphics: only the hardware-independent surface manager driver (stand-in source for Symbian^3
# phones, src/standins/surfacemanagerdriver); the epocroot already has its headers
sync_src external/oss.FCL.sf.os.graphics/graphicshwdrivers/surfacemgr "$E/sf/os/graphics/graphicshwdrivers/surfacemgr"

# Local source patches (*.patch, applied once, -p1 relative to the package root). Several patch
# directories may feed one package; their patches are applied in file-name order across them.
apply_patches() {  # pkgdir patchdir...
  local pkg="$1"; shift
  local stamp="$pkg/.patches-applied"
  local p
  for p in $(for d in "$@"; do ls "$d"/*.patch 2>/dev/null; done | awk -F/ '{print $NF "\t" $0}' | sort | cut -f2); do
    grep -qxF "$(basename "$p")" "$stamp" 2>/dev/null && continue
    echo ">> patch $(basename "$p")"
    patch -d "$pkg" -p1 --forward < "$p"
    basename "$p" >> "$stamp"
  done
}
apply_patches "$E/linux_build"        /work/patches/linux_build
# the kernel: platform-wide patches, policy choices, and every personality's own patches
# (docs/devices.md: personalities/<sym9x>/patches)
apply_patches "$E/sf/os/kernelhwsrv"  /work/patches/kernelhwsrv/platform /work/patches/kernelhwsrv/policy \
                                       /work/personalities/*/patches
apply_patches "$E/sf/adapt/qemu"      /work/patches/qemu-baseport

# --- toolchain
T=/work/build/toolchains
if [ ! -x "$T/arm-2009q3/bin/arm-none-symbianelf-gcc" ]; then
  mkdir -p "$T"
  tar xjf downloads/arm-2009q3-63-arm-none-symbianelf-i686-pc-linux-gnu.tar.bz2 -C "$T"
fi
/work/scripts/fix-toolchain-libgcc.sh "$T/arm-2009q3"
"$T/arm-2009q3/bin/arm-none-symbianelf-gcc" --version | head -1
echo ">> EPOCROOT ready at ${E#/work/}"
