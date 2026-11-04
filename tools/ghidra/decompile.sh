#!/usr/bin/env bash
# Decompile one N95 XIP binary with headless Ghidra (no GUI, no MCP).
#   tools/ghidra/decompile.sh NAME.dll [OUTDIR]   -> OUTDIR/NAME.c (+ NAME.elf for the Ghidra GUI)
# GHIDRA_HOME defaults to the newest ~/ghidra_* install.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
NAME="$1"; OUT="${2:-build/n95/ghidra}"; mkdir -p "$OUT"
GHIDRA_HOME="${GHIDRA_HOME:-$(ls -d $HOME/ghidra_* | sort -V | tail -1)}"
python3 -I tools/xip2elf.py build/n95/core-flat.img build/n95/core.json "$NAME" "$OUT/$NAME.elf"
PROJ=$(mktemp -d)
"$GHIDRA_HOME/support/analyzeHeadless" "$PROJ" p -import "$OUT/$NAME.elf" -overwrite \
  -scriptPath "$ROOT/tools/ghidra" -postScript ExportDecompiled.java "$ROOT/$OUT/$NAME.c" \
  -deleteProject > "$OUT/$NAME.ghidra.log" 2>&1 || { tail -20 "$OUT/$NAME.ghidra.log"; exit 1; }
rm -rf "$PROJ"
grep -c '^// ====' "$OUT/$NAME.c" | sed "s#^#$OUT/$NAME.c: functions #"
