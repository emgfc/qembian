#!/usr/bin/env bash
# Compile a Symbian .rss resource against the S60 3rd FP1 SDK headers.
#   tools/rcomp.sh IN.rss OUT.rsc [UID2 UID3]
# Preprocesses on the host (cpp, SDK include dirs), compiles with the 32-bit rcomp from
# build/epocroot/epoc32/tools in the legacy container (scripts/in-container.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
IN="$(realpath "$1")"; OUT="$(realpath -m "$2")"
mkdir -p "$ROOT/build/rcomp" "$(dirname "$OUT")"
B="$ROOT/build/rcomp/$(basename "${IN%.*}")"
# SDK headers are CRLF and mixed case: copy the needed include dir with lower-case aliases is
# overkill; cpp finds them case-sensitively, so the .rss must use the on-disk names.
mkdir -p "$ROOT/build/rcomp/inc"; ln -sf "$SDK/include/startupitem.hrh" "$ROOT/build/rcomp/inc/StartupItem.hrh"
cpp -P -undef -nostdinc -traditional-cpp -D_UNICODE -DLANGUAGE_SC -I"$(dirname "$IN")" -I"$ROOT/build/rcomp/inc" \
    -I"$SDK/include" -I"$SDK/include/variant" "$IN" | tr -d '\r' > "$B.rpp"
REL_RPP="${B#$ROOT/}.rpp"; REL_OUT="build/rcomp/$(basename "$OUT")"
UIDS=()
[ $# -ge 4 ] && UIDS=("-{$3,$4}")
"$ROOT/scripts/in-container.sh" /work/build/epocroot/epoc32/tools/rcomp -u "${UIDS[@]}" \
    -o"/work/$REL_OUT" -s"/work/$REL_RPP" >&2
[ "$ROOT/$REL_OUT" = "$OUT" ] || cp "$ROOT/$REL_OUT" "$OUT"
ls -la "$OUT"
