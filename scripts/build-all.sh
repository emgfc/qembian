#!/usr/bin/env bash
# One-shot: fetch pinned inputs, build everything, boot-test the result.
#   scripts/build-all.sh [udeb|urel]
# Host requirements: bash, git, curl, sha256sum, podman (or ENGINE=docker).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VAR="${1:-udeb}"
cd "$ROOT"
step() { echo; echo "=================== $* ==================="; }
step "fetch";            scripts/fetch.sh
step "setup-epocroot";   scripts/setup-epocroot.sh
step "build-qemu";       scripts/build-qemu.sh
step "build-hosttools";  scripts/build-hosttools.sh
step "build-kernel";     scripts/build-kernel.sh "$VAR"
step "build-syborg";     scripts/build-syborg.sh "$VAR"
step "boot-test";        scripts/boot-test.sh --secs 60 --type 'dir z:\sys\bin\e*.exe' \
                           build/epocroot/epoc32/rom/syborg_tshell_ARMV5_${VAR}.img < /dev/null
echo; echo "Screenshots/serial log: build/boot-test/syborg_tshell_ARMV5_${VAR}/"
