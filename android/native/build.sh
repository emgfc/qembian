#!/usr/bin/env bash
# The Android native part (android/README.md): QEMU 11.1.3 with our syborg board as libqemu-syborg.so for
# arm64-v8a, with static glib and pixman, our libsyvg and no FFmpeg (audio off on Android).
#   android/native/build.sh [deps|syvg|qemu|all]          (default all; each step skips what is done)
# Output: build/android/jniLibs/arm64-v8a/libqemu-syborg.so (the Gradle project picks it up).
# Inputs: android/native/sources.lock (downloads/android/), the QEMU tarball the desktop build uses,
# the NDK (ANDROID_NDK, default the SDK Manager's r28c in ~/Android/Sdk).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
STEP="${1:-all}"
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}"
API=30
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
[ -x "$TC/bin/aarch64-linux-android$API-clang" ] || { echo "no NDK at $NDK (set ANDROID_NDK)"; exit 1; }
B="$ROOT/build/android"; PREFIX="$B/prefix"; DL="$ROOT/downloads/android"
MESON="${MESON:-$(command -v meson || echo "$ROOT/build/qemu-modern/build/pyvenv/bin/meson")}"
mkdir -p "$B" "$PREFIX" "$DL" "$B/logs"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""

fetch() {   # name: download and check against sources.lock
    local line url sum
    line=$(grep "^$1 " android/native/sources.lock); url=$(echo "$line" | awk '{print $2}'); sum=$(echo "$line" | awk '{print $3}')
    [ -f "$DL/$1" ] || curl -sfL -o "$DL/$1" "$url"
    echo "$sum  $DL/$1" | sha256sum -c --quiet - || { echo "checksum mismatch: $1"; exit 1; }
}

cross_file() {
    cat > "$B/cross-aarch64.ini" <<CROSS
[binaries]
c = '$TC/bin/aarch64-linux-android$API-clang'
cpp = '$TC/bin/aarch64-linux-android$API-clang++'
ar = '$TC/bin/llvm-ar'
strip = '$TC/bin/llvm-strip'
ranlib = '$TC/bin/llvm-ranlib'
nm = '$TC/bin/llvm-nm'
objcopy = '$TC/bin/llvm-objcopy'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
CROSS
}

deps() {
    cross_file
    if [ ! -f "$PREFIX/lib/pkgconfig/glib-2.0.pc" ]; then
        fetch glib-2.86.5.tar.xz
        rm -rf "$B/glib-2.86.5"; tar -C "$B" -xJf "$DL/glib-2.86.5.tar.xz"
        "$MESON" setup "$B/glib-build" "$B/glib-2.86.5" --cross-file "$B/cross-aarch64.ini" --prefix "$PREFIX" \
            --libdir lib -Ddefault_library=static -Dtests=false -Dintrospection=disabled -Dnls=disabled \
            -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dman-pages=disabled -Ddocumentation=false \
            -Dsysprof=disabled -Dlibelf=disabled --wrap-mode=default --force-fallback-for=libffi,pcre2 \
            > "$B/logs/glib-setup.log" 2>&1 || { tail -30 "$B/logs/glib-setup.log"; exit 1; }
        ninja -C "$B/glib-build" install > "$B/logs/glib-build.log" 2>&1 || { tail -30 "$B/logs/glib-build.log"; exit 1; }
    fi
    if [ ! -f "$PREFIX/lib/pkgconfig/pixman-1.pc" ]; then
        fetch pixman-0.46.4.tar.gz
        rm -rf "$B/pixman-0.46.4"; tar -C "$B" -xzf "$DL/pixman-0.46.4.tar.gz"
        "$MESON" setup "$B/pixman-build" "$B/pixman-0.46.4" --cross-file "$B/cross-aarch64.ini" --prefix "$PREFIX" \
            --libdir lib -Ddefault_library=static -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
            > "$B/logs/pixman-setup.log" 2>&1 || { tail -30 "$B/logs/pixman-setup.log"; exit 1; }
        ninja -C "$B/pixman-build" install > "$B/logs/pixman-build.log" 2>&1 || { tail -30 "$B/logs/pixman-build.log"; exit 1; }
    fi
    echo "deps: glib, pixman in $PREFIX"
}

syvg() {
    SYVG_CXX="$TC/bin/aarch64-linux-android$API-clang++" SYVG_AR="$TC/bin/llvm-ar" SYVG_OUT=build/android/syvg \
        scripts/build-syvg.sh > "$B/logs/syvg.log" 2>&1 || { tail -20 "$B/logs/syvg.log"; exit 1; }
    echo "syvg: $B/syvg/libsyvg.a"
}

# a toolchain directory with the names QEMU's configure derives from --cross-prefix
toolchain() {
    local d="$B/tc"; mkdir -p "$d"
    # wrappers, not links: the NDK's clang scripts find clang next to themselves
    # (-march=armv8.2-a: LSE atomics, every phone since 2018; QEMU's TLB and locks use them constantly)
    printf '#!/bin/sh\nexec "%s" --target=aarch64-linux-android%s "$@"\n' "$TC/bin/clang" "$API" > "$d/aarch64-linux-android-cc"
    printf '#!/bin/sh\nexec "%s" --target=aarch64-linux-android%s "$@"\n' "$TC/bin/clang++" "$API" > "$d/aarch64-linux-android-c++"
    chmod +x "$d/aarch64-linux-android-cc" "$d/aarch64-linux-android-c++"
    for t in ar nm ranlib strip objcopy readelf; do ln -sf "$TC/bin/llvm-$t" "$d/aarch64-linux-android-$t"; done
    printf '#!/bin/sh\nexec env PKG_CONFIG_LIBDIR="%s" PKG_CONFIG_PATH= pkg-config "$@"\n' "$PREFIX/lib/pkgconfig" > "$d/aarch64-linux-android-pkg-config"
    chmod +x "$d/aarch64-linux-android-pkg-config"
}

qemu() {
    local VER=11.1.3 SRC="$B/qemu-11.1.3" BLD="$B/qemu-build"
    toolchain
    [ -d "$SRC" ] || tar -C "$B" -xJf "$ROOT/downloads/qemu-$VER.tar.xz"
    # every patch once, also ones added after the tree was unpacked (as scripts/build-qemu-modern.sh); the
    # applied list is the tree's record of what it is built from
    for p in patches/qemu-modern/*.patch android/native/patches/*.patch; do
        [ -e "$p" ] || continue
        grep -qxF "$(basename "$p")" "$SRC/.syborg-patches" 2>/dev/null && continue
        echo ">> patch $(basename "$p")"; patch -s -d "$SRC" -p1 --forward < "$p" || { echo "patch $p failed"; exit 1; }
        basename "$p" >> "$SRC/.syborg-patches"
    done
    cp src/qemu-syborg/hw/arm/syborg*.{c,h} src/syvg/syvg.h src/syvg/vg2_fn.h "$SRC/hw/arm/"
    cp android/native/syborg_android.c "$SRC/hw/arm/"
    python3 android/native/gen_epoxy_shim.py "$TC/sysroot/usr/include" "$B/include" \
        src/qemu-syborg/hw/arm/syborg*.[ch] src/syvg/*.cpp src/syvg/*.h
    grep -q 'config SYBORG' "$SRC/hw/arm/Kconfig" || printf '\nconfig SYBORG\n    bool\n    default y\n    depends on TCG && ARM\n    select PTIMER\n' >> "$SRC/hw/arm/Kconfig"
    sed -i "/CONFIG_SYBORG/d" "$SRC/hw/arm/meson.build"
    local FILES; FILES=$( (cd src/qemu-syborg/hw/arm && ls syborg*.c; echo syborg_android.c) | sed "s/.*/'&'/" | paste -sd, -)
    # no FFmpeg (audio off), no epoxy (android/native/include/epoxy: NDK EGL/GLES), our syvg built by the NDK
    sed -i "1a arm_common_ss.add(when: 'CONFIG_SYBORG', if_true: [files($FILES), declare_dependency(compile_args: ['-DSYBORG_NO_FFMPEG', '-I$B/include'], link_args: ['$B/syvg/libsyvg.a', '-lc++_static', '-lc++abi', '-lEGL', '-lGLESv1_CM', '-lGLESv3', '-landroid', '-llog'])])" "$SRC/hw/arm/meson.build"
    mkdir -p "$BLD"
    if [ ! -f "$BLD/build.ninja" ]; then
        (cd "$BLD" && PATH="$B/tc:$PATH" "$SRC/configure" --cross-prefix=aarch64-linux-android- \
            --cc=aarch64-linux-android-cc --cxx=aarch64-linux-android-c++ --host-cc=cc \
            "--extra-cflags=-fPIC -march=armv8.2-a -include $ROOT/android/native/include/android_compat.h" \
            "--extra-cxxflags=-fPIC -march=armv8.2-a" --disable-pie --enable-lto \
            --target-list=arm-softmmu --disable-docs --disable-tools --disable-guest-agent --disable-werror \
            --disable-sdl --disable-gtk --disable-vnc --disable-spice --disable-curses --disable-opengl \
            --disable-kvm --disable-xen --disable-linux-aio --disable-linux-io-uring --disable-libusb \
            --disable-usb-redir --disable-curl --disable-libssh --disable-gnutls --disable-nettle --disable-gcrypt \
            --disable-libudev --disable-seccomp --disable-slirp --disable-vhost-user --disable-vhost-net \
            --disable-cap-ng --disable-attr --disable-numa --disable-pipewire --disable-alsa --disable-pa \
            --disable-oss --disable-jack --disable-sndio --disable-plugins --disable-selinux --disable-libdw \
            --disable-png --disable-zstd --disable-bzip2 --disable-lzo --disable-snappy --disable-fuse \
            --disable-libpmem --disable-rbd --disable-libiscsi --disable-libnfs --disable-vde \
            --disable-netmap --disable-l2tpv3 --disable-tpm --disable-u2f --disable-smartcard --disable-virtfs \
            --disable-qcow1 --disable-vdi --disable-vvfat --disable-dmg --disable-parallels --disable-bochs \
            --disable-cloop --disable-qed --disable-vpc --disable-vhdx --enable-fdt=internal \
            > "$B/logs/qemu-configure.log" 2>&1) || { tail -30 "$B/logs/qemu-configure.log"; exit 1; }
    fi
    (cd "$BLD" && PATH="$B/tc:$PATH" ninja qemu-system-arm > "$B/logs/qemu-build.log" 2>&1) ||
        { grep -E 'error|FAILED' "$B/logs/qemu-build.log" | head -20; exit 1; }
    # the same link as the executable, as a shared library without system/main.c (android/native/syborg_android.c
    # starts it), so the app can load it
    # (ninja keeps the response file of the link with -d keeprsp)
    rm -f "$BLD/qemu-system-arm"
    (cd "$BLD" && PATH="$B/tc:$PATH" ninja -d keeprsp qemu-system-arm > /dev/null)
    local CMD; CMD=$(cd "$BLD" && ninja -t commands qemu-system-arm | tail -1)
    sed -e 's#qemu-system-arm.p/system_main.c.o##g' -e 's#\(^\| \)-pie\( \|$\)# #g' \
        -e 's#-o qemu-system-arm\( \|$\)#-shared -Wl,-soname,libqemu-syborg.so -o libqemu-syborg.so #' \
        "$BLD/qemu-system-arm.rsp" > "$BLD/libqemu-syborg.rsp"
    CMD=$(echo "$CMD" | sed -e 's#@qemu-system-arm.rsp#@libqemu-syborg.rsp#')
    (cd "$BLD" && PATH="$B/tc:$PATH" eval "$CMD") > "$B/logs/qemu-link.log" 2>&1 || { tail -20 "$B/logs/qemu-link.log"; exit 1; }
    mkdir -p "$B/jniLibs/arm64-v8a"
    "$TC/bin/llvm-strip" -o "$B/jniLibs/arm64-v8a/libqemu-syborg.so" "$BLD/libqemu-syborg.so"
    ls -l "$B/jniLibs/arm64-v8a/libqemu-syborg.so"
}

case "$STEP" in
deps) deps ;;
syvg) syvg ;;
qemu) qemu ;;
all) deps; syvg; qemu ;;
*) echo "usage: $0 [deps|syvg|qemu|all]"; exit 1 ;;
esac
