#!/usr/bin/env bash
# Fetch all pinned inputs listed in sources.lock into external/ and downloads/.
# Idempotent: existing clones are re-checked-out at the pinned commit, existing
# downloads are re-verified by sha256.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p external downloads

while read -r kind name url pin; do
  case "$kind" in
    git)
      dir="external/$name"
      if [ ! -d "$dir/.git" ]; then
        echo ">> clone $name"
        git clone -q "$url" "$dir"
      fi
      git -C "$dir" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$dir" fetch -q origin
      git -C "$dir" -c advice.detachedHead=false checkout -q "$pin"
      echo "   $name @ $(git -C "$dir" rev-parse --short HEAD)"
      ;;
    file)
      f="downloads/$name"
      if [ ! -f "$f" ] || ! echo "$pin  $f" | sha256sum -c --status; then
        echo ">> download $name"
        curl -fSL --retry 5 -C - -o "$f" "$url"
      fi
      echo "$pin  $f" | sha256sum -c --quiet && echo "   $name: sha256 OK"
      ;;
  esac
done < <(grep -vE '^\s*(#|$)' sources.lock)
