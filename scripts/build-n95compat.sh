#!/usr/bin/env bash
# Build src/n95compat (kernel-side N95 stand-in drivers) against the Syborg variant, then
# build the hybrid's kernel-side ROM: the Syborg text-shell obey file plus those drivers.
#   scripts/build-n95compat.sh [udeb]
# Output: build/rom-hybrid/syborg_hybrid_kernel_ARMV5_<var>.img (input for tools/romcompose.py)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  exec "$ROOT/scripts/in-container.sh" /work/scripts/build-n95compat.sh "$@"
fi
source /work/scripts/env.sh
VAR="${1:-udeb}"; CFG="arm.v5.$VAR.gcce4_4_1.surge"
DST=${EPOCROOT}sf/adapt/qemu/baseport/syborg/n95compat
rm -rf "$DST"; cp -r /work/src/n95compat "$DST"
cd "$DST"
sbs -b bld.inf -c "$CFG" -f "$LOGS/n95compat-$VAR.log" > "$LOGS/n95compat-$VAR.out" 2>&1 || true
grep -E 'sbs : errors' "$LOGS/n95compat-$VAR.out" || true
if grep -q -E '<error|: error' "$LOGS/n95compat-$VAR.log"; then
  grep -E '<error|: error' "$LOGS/n95compat-$VAR.log" | head -20; exit 1
fi
REL=${EPOCROOT}epoc32/release/armv5/$VAR
OBY="${EPOCROOT}epoc32/rom/syborg/syborg_tshell_ARMV5_${VAR}_rom.oby"
OUT=/work/build/rom-hybrid; mkdir -p "$OUT"; cd "$OUT"
NAME=syborg_hybrid_kernel_ARMV5_$VAR
VARID=$(grep -m1 -o 'device\[0x[0-9a-f]*\]' "$OBY" | grep -o '0x[0-9a-f]*')
# rombuild lays files out in obey order: insert the stand-ins right after the first device[] line
# so they stay inside the kernel-side window below the N95 user side (romcompose.py checks this)
for m in $(grep -o '^[a-z0-9_]*/[a-z0-9_]*\.mmp' /work/src/n95compat/bld.inf); do
  f=$(basename "$m" .mmp)
  case "$f" in
    lcd_n95) continue ;;	# replaces LCD.DLL below instead of being added
    med*) kind=extension; ext=pdd ;;
    *) kind=device; ext=ldd ;;
  esac
  printf '%s[%s]=\t%s/_syborg_%s.%s\t\\sys\\bin\\%s.%s\n' "$kind" "$VARID" "$REL" "$f" "$ext" "$f" "$ext"
done > extra.oby
awk -v name="$NAME" '
  /^romname=/ { print "romname=" name ".img"; next }
  /_syborg_lcd\.dll/ { sub(/_syborg_lcd\.dll/, "_syborg_lcd_n95.dll") }
  { print }
  /^device\[/ && !done { while ((getline l < "extra.oby") > 0) print l; done=1 }' "$OBY" > "$NAME.oby"
rombuild "$NAME.oby" > "$NAME.rombuild.log" 2>&1 || { tail -20 "$NAME.rombuild.log"; exit 1; }
ls -la "$OUT/$NAME.img"
