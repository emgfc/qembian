#!/usr/bin/env bash
# Build the arm64 Linux guest kernel for the hosted-EKA2 virt machine (docs/hosted-eka2/qemu-setup.md).
#   scripts/build-hosted-kernel.sh [LINUX_SRC]
# Source: downloads/linux-6.18.63.tar.xz (longterm, pinned in sources.lock) unpacked into build/hosted/,
# or a full kernel tree given as LINUX_SRC; built out of tree into build/hosted/linux;
# config = arm64 defconfig + src/hosted/linux/hosted.config.
# Needs an aarch64 cross gcc (CROSS_COMPILE, default aarch64-suse-linux- or aarch64-linux-gnu-).
# Output: build/hosted/linux/arch/arm64/boot/Image
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
KVER=6.18.63
SRC=${1:-$ROOT/build/hosted/linux-$KVER}
if [ -z "${1:-}" ] && [ ! -d "$SRC" ]; then
  [ -f downloads/linux-$KVER.tar.xz ] || scripts/fetch.sh
  mkdir -p build/hosted && tar -C build/hosted -xJf downloads/linux-$KVER.tar.xz
fi
[ -f "$SRC/arch/arm64/kernel/entry.S" ] || { echo "no full arm64 kernel source in $SRC"; exit 1; }
if [ -z "${CROSS_COMPILE:-}" ]; then
  for c in aarch64-suse-linux- aarch64-linux-gnu-; do command -v ${c}gcc >/dev/null && CROSS_COMPILE=$c && break; done
fi
O=$ROOT/build/hosted/linux
mkdir -p "$O"
K="make -C $SRC O=$O ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE"
if [ ! -f "$O/.config" ] || [ src/hosted/linux/hosted.config -nt "$O/.config" ]; then
  $K defconfig >/dev/null
  "$SRC/scripts/kconfig/merge_config.sh" -m -O "$O" "$O/.config" src/hosted/linux/hosted.config >/dev/null
  $K olddefconfig >/dev/null
  for o in COMPAT SWP_EMULATION CP15_BARRIER_EMULATION SECCOMP_FILTER MEMFD_CREATE; do
    grep -q "^CONFIG_$o=y" "$O/.config" || { echo "config: CONFIG_$o did not stick"; exit 1; }
  done
fi
$K -j"$(nproc)" Image 2>&1 | grep -E 'error|Error' || true
ls -l "$O/arch/arm64/boot/Image"
