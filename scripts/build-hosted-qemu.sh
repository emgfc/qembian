#!/usr/bin/env bash
# Build a stock qemu-system-aarch64 (same pinned QEMU 11.1.3 tarball as the syborg build, no syborg board)
# for the hosted-EKA2 virt machine (docs/hosted-eka2/qemu-setup.md).
#   scripts/build-hosted-qemu.sh [clean]
# Output: build/hosted/qemu/install/bin/qemu-system-aarch64
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
VER=11.1.3; B=build/hosted/qemu
[ -f downloads/qemu-$VER.tar.xz ] || scripts/fetch.sh
[ "${1:-}" = clean ] && rm -rf "$B"
mkdir -p "$B" build/logs
[ -d "$B/qemu-$VER" ] || tar -C "$B" -xJf downloads/qemu-$VER.tar.xz
mkdir -p "$B/build"; cd "$B/build"
[ -f build.ninja ] || ../qemu-$VER/configure --prefix="$ROOT/$B/install" --target-list=aarch64-softmmu \
  --disable-docs --disable-werror --enable-slirp > "$ROOT/build/logs/hosted-qemu-configure.log" 2>&1
ninja > "$ROOT/build/logs/hosted-qemu-build.log" 2>&1 || { tail -20 "$ROOT/build/logs/hosted-qemu-build.log"; exit 1; }
ninja install > /dev/null
ls -l "$ROOT/$B/install/bin/qemu-system-aarch64"
