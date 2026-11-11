#!/usr/bin/env bash
# Build a separate qemu-system-arm with the N-Gage (EKA1) bring-up board, docs/eka1-ngage-roadmap.md P1.
#   scripts/build-qemu-ngage.sh [clean]
# Own tree (build/qemu-ngage/) from the pinned QEMU tarball, so the bring-up never rebuilds the
# shared Syborg binary (build/qemu-modern/). Only src/qemu-ngage/ is copied in; no Syborg files.
# Output: build/qemu-ngage/build/qemu-system-arm
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
VER=11.1.3
B=build/qemu-ngage
SRC=$B/qemu-$VER
mkdir -p "$B" build/logs
[ -f "downloads/qemu-$VER.tar.xz" ] || scripts/fetch.sh
if [ "${1:-}" = clean ] || [ ! -d "$SRC" ]; then
  rm -rf "$SRC" "$B/build"
  tar -C "$B" -xJf "downloads/qemu-$VER.tar.xz"
fi
cp src/qemu-ngage/hw/arm/ngage*.[ch] "$SRC/hw/arm/"
# ngage-only QEMU changes (patches/qemu-ngage/*.patch); each is applied once per tree
for p in patches/qemu-ngage/*.patch; do
  [ -e "$p" ] || continue
  m="$SRC/.applied-$(basename "$p")"
  [ -f "$m" ] && continue
  patch -d "$SRC" -p1 --forward -s < "$p" && touch "$m"
done
grep -q 'config NGAGE' "$SRC/hw/arm/Kconfig" || cat >> "$SRC/hw/arm/Kconfig" <<'K'

config NGAGE
    bool
    default y
    depends on TCG && ARM
    select PFLASH_CFI01
K
sed -i "/CONFIG_NGAGE/d" "$SRC/hw/arm/meson.build"
FILES=$(cd src/qemu-ngage/hw/arm && ls ngage*.c | sed "s/.*/'&'/" | paste -sd, -)
sed -i "1a arm_common_ss.add(when: 'CONFIG_NGAGE', if_true: files($FILES))" "$SRC/hw/arm/meson.build"
mkdir -p "$B/build"; cd "$B/build"
if [ ! -f build.ninja ]; then
  "../qemu-$VER/configure" --target-list=arm-softmmu --disable-docs --disable-werror \
    --disable-plugins --disable-opengl 2>&1 \
    | tee "$ROOT/build/logs/qemu-ngage-configure.log" | tail -3
fi
if ! ninja qemu-system-arm > "$ROOT/build/logs/qemu-ngage-build.log" 2>&1; then
  grep -E 'error|FAILED' "$ROOT/build/logs/qemu-ngage-build.log" | head -20
  echo "build failed: build/logs/qemu-ngage-build.log" >&2; exit 1
fi
grep -E 'ngage\.c.*warning' "$ROOT/build/logs/qemu-ngage-build.log" || true
ls -l qemu-system-arm
