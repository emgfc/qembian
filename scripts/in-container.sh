#!/usr/bin/env bash
# Run a command inside the legacy build container with the project mounted at /work.
#   scripts/in-container.sh [cmd args...]     (default: interactive bash)
# Env: ENGINE (podman|docker, default podman), IMAGE (default syborg-legacy:squeeze-i386),
#      CONTAINER_ARGS (extra whitespace-separated args for `run`, e.g. port/volume mappings)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-podman}"
IMAGE="${IMAGE:-localhost/syborg-legacy:squeeze-i386}"

if ! "$ENGINE" image exists "$IMAGE" 2>/dev/null; then
  echo ">> building container image $IMAGE" >&2
  "$ENGINE" build --arch 386 -t "$IMAGE" -f "$ROOT/container/Containerfile" "$ROOT/container" >&2
fi

tty_flags=()
[ -t 0 ] && [ -t 1 ] && tty_flags=(-it)

userns=()
[ "$ENGINE" = podman ] && userns=(--userns=keep-id)

exec "$ENGINE" run --rm "${tty_flags[@]}" "${userns[@]}" --arch 386 \
  -v "$ROOT:/work:z" -w /work \
  -e HOME=/work/build/home -e SYBORG_IN_CONTAINER=1 \
  ${CONTAINER_ARGS:-} \
  "$IMAGE" "${@:-bash}"
