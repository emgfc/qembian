#!/usr/bin/env bash
# Build modern QEMU (pinned in sources.lock) with the Syborg board on the host.
#   scripts/build-qemu-modern.sh [clean] [--container]
# Source: downloads/qemu-<ver>.tar.xz (scripts/fetch.sh, sha256 pinned in sources.lock); our board
# lives in src/qemu-syborg/ and is copied into the tree, with Kconfig/meson entries added (idempotent).
# Output: build/qemu-modern/install/bin/qemu-system-arm (host build: needs the host's dev packages,
# the list is container/Containerfile.modern), or with --container a build inside the modern image
# into build/qemu-modern-ct/ (podman/docker only; run through scripts/qemu-container.sh, which
# scripts/run-modern.sh picks by itself when there is no host build).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
ARGS=(); CT=0
for x in "$@"; do [ "$x" = --container ] && CT=1 || ARGS+=("$x"); done
if [ "$CT" = 1 ] && [ -z "${SYBORG_IN_MODERN:-}" ]; then
  [ -f downloads/qemu-11.1.3.tar.xz ] || scripts/fetch.sh
  CONTAINER=modern exec scripts/in-container.sh scripts/build-qemu-modern.sh "${ARGS[@]}"
fi
set -- "${ARGS[@]+"${ARGS[@]}"}"
VER=11.1.3
B=build/qemu-modern
[ -n "${SYBORG_IN_MODERN:-}" ] && B=build/qemu-modern-ct		# links against the image's libraries
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
# the host EGL/OpenVG (the Khronos RI built for the host, C++): a static library linked into the device
scripts/build-syvg.sh > build/logs/syvg-build.log 2>&1 || { tail -20 build/logs/syvg-build.log; exit 1; }
cp src/syvg/syvg.h src/syvg/vg2_fn.h "$SRC/hw/arm/"
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
# syborg-audio decodes MP3/AAC with FFmpeg (libavcodec, libswresample from the host);
# syborg-gl renders with the host GPU through libepoxy (EGL surfaceless + GLES 2)
sed -i "1a arm_common_ss.add(when: 'CONFIG_SYBORG', if_true: [files($FILES), dependency('libavcodec'), dependency('libavutil'), dependency('libswresample'), dependency('epoxy'), declare_dependency(link_args: ['$ROOT/build/syvg/libsyvg.a', '-lstdc++'])])" "$SRC/hw/arm/meson.build"

mkdir -p "$B/build"
cd "$B/build"
# link-time optimisation (cross-unit inlining in the TCG hot paths, a few percent); no TCG plugins
# (nothing here uses them; their hooks cost in slow paths). docs/perf.md
OPTS="--enable-lto --disable-plugins --enable-slirp"   # slirp: -nic user for syborg-net (libslirp-devel / libslirp-dev)
if [ ! -f build.ninja ]; then
  "../qemu-$VER/configure" --prefix="$INST" --target-list=arm-softmmu --disable-docs \
    --disable-werror $OPTS 2>&1 | tee "$ROOT/build/logs/qemu-modern-configure.log"
  echo "$OPTS" > .syborg-configure
elif [ "$(cat .syborg-configure 2>/dev/null)" != "$OPTS" ]; then
  # an existing tree configured with other options: change them in place
  pyvenv/bin/meson configure -Db_lto=true -Dplugins=false -Dslirp=enabled . 2>&1 | tail -3
  echo "$OPTS" > .syborg-configure
fi
ninja 2>&1 | tee "$ROOT/build/logs/qemu-modern-build.log" | grep -E 'error|warning: .*syborg|FAILED' || true
ninja install > "$ROOT/build/logs/qemu-modern-install.log" 2>&1
ls -l "$INST/bin/qemu-system-arm"
