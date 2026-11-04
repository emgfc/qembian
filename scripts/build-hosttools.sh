#!/usr/bin/env bash
# Build Raptor (SBSv2) and the Linux host tools (elf2e32, rcomp, rombuild, ...) from the
# LinuxBuild package, using its own cross-plat-dev-utils scripts.
# Requires scripts/setup-epocroot.sh to have run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  exec "$ROOT/scripts/in-container.sh" /work/scripts/build-hosttools.sh "$@"
fi
source /work/scripts/env.sh
cd "${EPOCROOT}linux_build/cross-plat-dev-utils"

if [ ! -x "$SBS_HOME/linux-unknown-libc2_11/python27/bin/python" ]; then
  echo ">> build_raptor.pl"; perl build_raptor.pl > "$LOGS/raptor-build.log" 2>&1
fi
echo ">> prep_env.pl";  perl prep_env.pl  > "$LOGS/prep-env.log" 2>&1
echo ">> build_all.pl"; perl build_all.pl > "$LOGS/hosttools-build.log" 2>&1 \
  || { grep -E '^\*\*\*' "$LOGS/hosttools-build.log"; exit 1; }
for t in elf2e32 rcomp rombuild readimage; do
  [ -x "${EPOCROOT}epoc32/tools/$t" ] || { echo "missing host tool: $t" >&2; exit 1; }
done
echo ">> host tools OK"
