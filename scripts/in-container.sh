#!/usr/bin/env bash
# Run a command inside one of the two build containers.
#   scripts/in-container.sh [cmd args...]                 legacy image (default: interactive bash)
#   CONTAINER=modern scripts/in-container.sh [cmd ...]    modern image
# legacy  container/Containerfile         Debian squeeze i386: Symbian host tools, GCCE 4.4 kernel,
#         baseport, stand-ins, QEMU 0.9.1. Project mounted at /work.
# modern  container/Containerfile.modern  Debian trixie: modern QEMU, GCC 14 symbianelf + elf2e32_next
#         (sygl, apps), Python tools. Project mounted at its host path, so paths and sockets that
#         tools write (sydb sessions, QMP) mean the same inside and outside.
# Env: ENGINE (podman|docker, default podman), IMAGE (override), CONTAINER_ARGS (extra `run` args),
#      S60SDK (host path of the S60 SDK's Epoc32, mounted read-only at the same path; never in the image)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-podman}"
KIND="${CONTAINER:-legacy}"
userns=(); [ "$ENGINE" = podman ] && userns=(--userns=keep-id)
[ "$ENGINE" = docker ] && userns=(--user "$(id -u):$(id -g)")
tty_flags=(); [ -t 0 ] && [ -t 1 ] && tty_flags=(-it)

have_image() {
  if [ "$ENGINE" = podman ]; then podman image exists "$1"; else docker image inspect "$1" > /dev/null 2>&1; fi
}

case "$KIND" in
  legacy)
    IMAGE="${IMAGE:-localhost/qembian-legacy:squeeze-i386}"
    if ! have_image "$IMAGE"; then
      echo ">> building container image $IMAGE" >&2
      "$ENGINE" build --arch 386 -t "$IMAGE" -f "$ROOT/container/Containerfile" "$ROOT/container" >&2
    fi
    exec "$ENGINE" run --rm "${tty_flags[@]}" "${userns[@]}" --arch 386 \
      -v "$ROOT:/work:z" -w /work \
      -e HOME=/work/build/home -e SYBORG_IN_CONTAINER=1 \
      ${CONTAINER_ARGS:-} \
      "$IMAGE" "${@:-bash}" ;;
  modern)
    IMAGE="${IMAGE:-localhost/qembian-modern:trixie}"
    if ! have_image "$IMAGE"; then
      echo ">> building container image $IMAGE (GCC 14 toolchain: takes a while once)" >&2
      "$ENGINE" build -t "$IMAGE" -f "$ROOT/container/Containerfile.modern" "$ROOT" >&2
    fi
    sdk=(); [ -n "${S60SDK:-}" ] && sdk=(-v "$S60SDK:$S60SDK:ro,z" -e "S60SDK=$S60SDK")
    mkdir -p "$ROOT/build/home"
    init=(--init); [[ " ${CONTAINER_ARGS:-} " == *" --pid host "* ]] && init=()   # PID 1 only in a private PID namespace
    exec "$ENGINE" run --rm "${init[@]}" "${tty_flags[@]}" "${userns[@]}" \
      -v "$ROOT:$ROOT:z" -w "$ROOT" "${sdk[@]}" \
      -e HOME="$ROOT/build/home" -e SYBORG_IN_MODERN=1 \
      ${CONTAINER_ARGS:-} \
      "$IMAGE" "${@:-bash}" ;;
  *) echo "CONTAINER must be legacy or modern" >&2; exit 2 ;;
esac
