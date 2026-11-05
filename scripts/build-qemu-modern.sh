#!/usr/bin/env bash
# Build modern QEMU (pinned in sources.lock) with the Syborg board on the host.
#   scripts/build-qemu-modern.sh [clean]
# Source: downloads/qemu-<ver>.tar.xz (signature checked when fetched); our board lives in
# src/qemu-syborg/ and is copied into the tree, with Kconfig/meson entries added (idempotent).
# Output: build/qemu-modern/install/bin/qemu-system-arm.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
VER=11.1.3
B=build/qemu-modern
SRC=$B/qemu-$VER
INST=$ROOT/$B/install
mkdir -p "$B" build/logs

if [ "${1:-}" = clean ] || [ ! -d "$SRC" ]; then
  rm -rf "$SRC" "$B/build"
  tar -C "$B" -xJf "downloads/qemu-$VER.tar.xz"
fi

for p in patches/qemu-modern/*.patch; do
  [ -e "$p" ] || continue
  grep -qxF "$(basename "$p")" "$SRC/.syborg-patches" 2>/dev/null && continue
  echo ">> patch $(basename "$p")"; patch -d "$SRC" -p1 --forward < "$p"
  basename "$p" >> "$SRC/.syborg-patches"
done
cp src/qemu-syborg/hw/arm/syborg*.{c,h} "$SRC/hw/arm/"
grep -q 'config SYBORG' "$SRC/hw/arm/Kconfig" || cat >> "$SRC/hw/arm/Kconfig" <<'EOF'

config SYBORG
    bool
    default y
    depends on TCG && ARM
    select PTIMER
EOF
# (re)write the meson line so that new board files are picked up
sed -i "/CONFIG_SYBORG/d" "$SRC/hw/arm/meson.build"
FILES=$(cd src/qemu-syborg/hw/arm && ls syborg*.c | sed "s/.*/'&'/" | paste -sd, -)
sed -i "1a arm_common_ss.add(when: 'CONFIG_SYBORG', if_true: files($FILES))" "$SRC/hw/arm/meson.build"

mkdir -p "$B/build"
cd "$B/build"
if [ ! -f build.ninja ]; then
  "../qemu-$VER/configure" --prefix="$INST" --target-list=arm-softmmu --disable-docs \
    --disable-werror 2>&1 | tee "$ROOT/build/logs/qemu-modern-configure.log"
fi
ninja 2>&1 | tee "$ROOT/build/logs/qemu-modern-build.log" | grep -E 'error|warning: .*syborg|FAILED' || true
ninja install > "$ROOT/build/logs/qemu-modern-install.log" 2>&1
ls -l "$INST/bin/qemu-system-arm"
