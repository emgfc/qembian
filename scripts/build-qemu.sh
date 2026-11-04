#!/usr/bin/env bash
# Build the Symbian-modified QEMU 0.9.1 ("qemu-symbian-svp") inside the legacy container.
#
# Output (build/qemu/install):
#   bin/qemu-system-arm
#   lib/libnvmemmory.so          native helper for the nvmemory Python device
#   share/qemu/plugins/*.py      Python device models (QEMU looks in <bios_dir>/plugins)
#
#   scripts/build-qemu.sh [clean]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  exec "$ROOT/scripts/in-container.sh" /work/scripts/build-qemu.sh "$@"
fi

UP=/work/external/oss.FCL.sf.adapt.Qemu/symbian-qemu-0.9.1-12
B=/work/build/qemu
INST=$B/install
LOGS=/work/build/logs
mkdir -p "$B" "$LOGS"

# QEMU 0.9.1 builds in-tree, and model-libraries reaches into ../../../qemu-symbian-svp,
# so build on a private copy that keeps the upstream relative layout.
if [ "${1:-}" = clean ] || [ ! -f "$B/qemu-symbian-svp/configure" ]; then
  rm -rf "$B/qemu-symbian-svp" "$B/model-libraries" "$INST"
  cp -a "$UP/qemu-symbian-svp" "$UP/model-libraries" "$B/"
  for p in /work/patches/qemu/*.patch; do
    [ -e "$p" ] || continue
    echo ">> applying $(basename "$p")"
    patch -d "$B" -p1 --forward < "$p"
  done
fi

cd "$B/qemu-symbian-svp"
if [ ! -f config-host.mak ]; then
  # Note: "fdt support: no" in the summary is expected. It only probes for a system
  # -lfdt (used by the PPC device_tree.c); the bundled libfdt/ is always linked in and
  # is what the syborg .dtb machine loader (devtree.c) uses.
  sh ./configure --target-list=arm-softmmu --prefix="$INST" \
      --python-config=/usr/bin/python2.6-config --audio-drv-list=sdl --disable-kqemu \
      2>&1 | tee "$LOGS/qemu-configure.log"
fi
make -j"$(nproc)" 2>&1 | tee "$LOGS/qemu-make.log"
make install 2>&1 | tee "$LOGS/qemu-install.log"

# Python device plugins -> <bios_dir>/plugins (see python-plugin.c: qemu_python_init)
mkdir -p "$INST/share/qemu/plugins"
cp -a plugins/*.py "$INST/share/qemu/plugins/"

# libnvmemmory.so, loaded via ctypes by syborg_nvmemorydevice.py
make -C "$B/model-libraries" all 2>&1 | tee "$LOGS/nvmemmory-make.log"
mkdir -p "$INST/lib"
cp -a "$B/model-libraries/nvmemmory/Release/libnvmemmory.so.1.0" "$INST/lib/"
ln -sf libnvmemmory.so.1.0 "$INST/lib/libnvmemmory.so.1"
ln -sf libnvmemmory.so.1.0 "$INST/lib/libnvmemmory.so"

echo; echo ">> installed:"; find "$INST/bin" "$INST/lib" -maxdepth 1 | sort
