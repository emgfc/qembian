#!/usr/bin/env bash
# Build the EKA2 kernel package (sf/os/kernelhwsrv) with GCCE 4.4.1 via Raptor.
#
#   scripts/build-kernel.sh [udeb|urel] [extra sbs args...]
#
# Steps:
#   1. kernel-side runtime support (compsupp/gcce -> ksrt_gcce.lib), needed by everything kernel-side
#   2. the whole package via package_definition.xml (kernel, user library, file server, drivers,
#      tests). -k (keep going): some test components are known not to build with GCCE.
# Logs: build/logs/kernel-<step>-<cfg>.log  (Raptor XML log; grep "<error" / "<warning")
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  exec "$ROOT/scripts/in-container.sh" /work/scripts/build-kernel.sh "$@"
fi
source /work/scripts/env.sh
VAR="${1:-udeb}"; shift || true
CFG="arm.v5.${VAR}.gcce4_4_1.surge"
K="${EPOCROOT}sf/os/kernelhwsrv"

summarise() {  # log
  local errs; errs=$(grep -c '<error' "$1" || true)
  echo "   $(basename "$1"): $errs <error> entries, $(grep -c '<warning' "$1" || true) <warning> entries"
}

echo ">> [1/2] kernel-side runtime (ksrt_gcce) [$CFG]"
( cd "$K/kernel/eka/compsupp/gcce" && sbs -c "$CFG" -f "$LOGS/kernel-ksrt-$VAR.log" "$@" ) || true
summarise "$LOGS/kernel-ksrt-$VAR.log"

echo ">> [2/2] kernelhwsrv package [$CFG]"
( cd "$K" && sbs -s package_definition.xml -c "$CFG" -k -j "$(nproc)" -f "$LOGS/kernel-pkg-$VAR.log" "$@" ) || true
summarise "$LOGS/kernel-pkg-$VAR.log"
# the package build keeps going past components that do not build with this toolchain; the kernel
# itself must not be one of them (e.g. personality tables missing from kernel/execpers_map.h)
if grep -q 'kernel/eka/kernel/[^ ]*: error:' "$LOGS/kernel-pkg-$VAR.log"; then
  grep 'kernel/eka/kernel/[^ ]*: error:' "$LOGS/kernel-pkg-$VAR.log" | head -5
  echo "!! the kernel did not compile (if execpers_map.h is stale: python3 tools/device.py build ID --kernel regenerates it)" >&2
  exit 1
fi

R="${EPOCROOT}epoc32/release/armv5/$VAR"
for f in _template_ekern.exe euser_v5.dll efile.exe efsrv.dll eshell.exe ewsrv.exe econs.dll elocal.fsy domainsrv.exe; do
  if [ -f "$R/$f" ]; then printf "   %-12s %8d bytes\n" "$f" "$(stat -c %s "$R/$f")"
  else printf "   %-12s MISSING\n" "$f"; fi
done
