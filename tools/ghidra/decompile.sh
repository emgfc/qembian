#!/usr/bin/env bash
# Decompile one N95 XIP binary with headless Ghidra (no GUI, no MCP).
#   tools/ghidra/decompile.sh NAME.dll [OUTDIR]   -> OUTDIR/NAME.c (+ NAME.elf for the Ghidra GUI)
#   tools/ghidra/decompile.sh path/to/file.elf [OUTDIR]  (e.g. a ROFS binary from tools/e32load.py)
# GHIDRA_HOME defaults to the newest ~/ghidra_* install.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
DEVDIR="$(python3 -I tools/device.py paths dir)"   # the device ($SYBORG_DEVICE / the registry default)
NAME="$1"; OUT="${2:-$DEVDIR/ghidra}"; mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"   # absolute: Ghidra runs elsewhere
GHIDRA_HOME="${GHIDRA_HOME:-$(ls -d $HOME/ghidra_* | sort -V | tail -1)}"
if [[ "$NAME" == *.elf ]]; then cp "$NAME" "$OUT/"; NAME=$(basename "$NAME" .elf)
else python3 -I tools/xip2elf.py "$DEVDIR/core-flat.img" "$DEVDIR/core.json" "$NAME" "$OUT/$NAME.elf"; fi
PROJ=$(mktemp -d)
"$GHIDRA_HOME/support/analyzeHeadless" "$PROJ" p -import "$OUT/$NAME.elf" -overwrite \
  -scriptPath "$ROOT/tools/ghidra" -postScript ExportDecompiled.java "$OUT/$NAME.c" \
  -deleteProject > "$OUT/$NAME.ghidra.log" 2>&1 || { tail -20 "$OUT/$NAME.ghidra.log"; exit 1; }
rm -rf "$PROJ"
grep -c '^// ====' "$OUT/$NAME.c" | sed "s#^#$OUT/$NAME.c: functions #"
