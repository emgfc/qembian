#!/usr/bin/env bash
# Build the device's kernel-side stand-ins (src/standins, chosen by its profile's [[standins]] and
# [kernel.lcd]) against the Syborg variant, then the hybrid's kernel-side ROM: the Syborg text-shell
# obey file plus those drivers, in profile order.
#   scripts/build-standins.sh [udeb] [DEVICE-ID]      (device: $SYBORG_DEVICE or devices/default)
# Output: build/rom-hybrid/syborg_hybrid_kernel_ARMV5_<var>.img (input for tools/romcompose.py)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VAR="${1:-udeb}"; DEVICE="${2:-${SYBORG_DEVICE:-}}"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  # host side: the fake modem's reply table from the device's ISI database, and the generated
  # bld.inf / obey list (tools/device.py) for the container side
  export SYBORG_DEVICE="$DEVICE"
  GEN="$ROOT/build/standins-gen"; mkdir -p "$GEN"
  python3 -I "$ROOT/tools/isi.py" gen-header "$ROOT/src/standins/_shared/isireplies.h" >/dev/null
  python3 -I "$ROOT/tools/device.py" gen standins-bldinf > "$GEN/bld.inf"
  python3 -I "$ROOT/tools/device.py" gen standins-oby > "$GEN/standins.oby.txt"
  python3 -I "$ROOT/tools/device.py" get kernel.lcd.target > "$GEN/lcd.txt" 2>/dev/null || echo "" > "$GEN/lcd.txt"
  exec "$ROOT/scripts/in-container.sh" /work/scripts/build-standins.sh "$VAR"
fi
source /work/scripts/env.sh
CFG="arm.v5.$VAR.gcce4_4_1.surge"
GEN=/work/build/standins-gen
DST=${EPOCROOT}sf/adapt/qemu/baseport/syborg/standins
rm -rf "$DST" "${EPOCROOT}sf/adapt/qemu/baseport/syborg/n95compat"; cp -r /work/src/standins "$DST"
cp "$GEN/bld.inf" "$DST/bld.inf"
cd "$DST"
sbs -b bld.inf -c "$CFG" -f "$LOGS/standins-$VAR.log" > "$LOGS/standins-$VAR.out" 2>&1 || true
grep -E 'sbs : errors' "$LOGS/standins-$VAR.out" || true
if grep -q -E '<error|: error' "$LOGS/standins-$VAR.log"; then
  grep -E '<error|: error' "$LOGS/standins-$VAR.log" | head -20; exit 1
fi
REL=${EPOCROOT}epoc32/release/armv5/$VAR
OBY="${EPOCROOT}epoc32/rom/syborg/syborg_tshell_ARMV5_${VAR}_rom.oby"
OUT=/work/build/rom-hybrid; mkdir -p "$OUT"; cd "$OUT"
NAME=syborg_hybrid_kernel_ARMV5_$VAR
VARID=$(grep -m1 -o 'device\[0x[0-9a-f]*\]' "$OBY" | grep -o '0x[0-9a-f]*')
LCD=$(cat "$GEN/lcd.txt")
# rombuild lays files out in obey order: insert the stand-ins right after the first device[] line
# so they stay inside the kernel-side window below the user side (romcompose.py checks this)
while read -r kind f ext; do
  [ -n "$kind" ] || continue
  printf '%s[%s]=\t%s/_syborg_%s.%s\t\\sys\\bin\\%s.%s\n' "$kind" "$VARID" "$REL" "$f" "$ext" "$f" "$ext"
done < "$GEN/standins.oby.txt" > extra.oby
awk -v name="$NAME" -v lcd="$LCD" '
  /^romname=/ { print "romname=" name ".img"; next }
  lcd != "" && /_syborg_lcd\.dll/ { sub(/_syborg_lcd\.dll/, "_syborg_" lcd ".dll") }
  { print }
  /^device\[/ && !done { while ((getline l < "extra.oby") > 0) print l; done=1 }' "$OBY" > "$NAME.oby"
rombuild "$NAME.oby" > "$NAME.rombuild.log" 2>&1 || { tail -20 "$NAME.rombuild.log"; exit 1; }
ls -la "$OUT/$NAME.img"
