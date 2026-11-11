#!/usr/bin/env bash
# Build the device's kernel-side stand-ins (src/standins, chosen by its profile's [[standins]] and
# [kernel.lcd]) against the Syborg variant, then the hybrid's kernel-side ROM: the Syborg text-shell
# obey file plus those drivers, in profile order.
#   scripts/build-standins.sh [udeb] [DEVICE-ID]      (device: $SYBORG_DEVICE or the registry default)
# Output: build/roms/<id>/<fw>/kernel-side.img (input for tools/romcompose.py; one per device, so devices
# sharing a baseport do not overwrite each other's); generated inputs in build/roms/<id>/<fw>/gen/;
# <baseport> = the profile's kernel.baseport (syborg: multiple memory model; syborgmv: moving).
# kernel.place = "after-firmware": a pad file right after our ROM directory pushes our kernel-side
# files past the end of the firmware's core image (ROMs whose user side starts among their own
# kernel-side files, e.g. the N73); romcompose.py checks every file against the firmware's user side.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VAR="${1:-udeb}"; DEVICE="${2:-${SYBORG_DEVICE:-}}"
if [ -z "${SYBORG_IN_CONTAINER:-}" ]; then
  # host side: the generated bld.inf / obey list (tools/device.py) for the container side (the fake
  # modem's reply table is QEMU's now: tools/device.py qemu-args writes it at every launch)
  export SYBORG_DEVICE="$DEVICE"
  DEVDIR=$(python3 -I "$ROOT/tools/device.py" paths dir)
  GEN="$DEVDIR/gen"; mkdir -p "$GEN"
  python3 -I "$ROOT/tools/device.py" gen standins-bldinf > "$GEN/bld.inf"
  python3 -I "$ROOT/tools/device.py" gen standins-oby > "$GEN/standins.oby.txt"
  python3 -I "$ROOT/tools/device.py" get kernel.omit_baseport 2>/dev/null \
    | python3 -c 'import json, sys; [print(x) for x in json.load(sys.stdin)]' > "$GEN/omit.txt" 2>/dev/null || : > "$GEN/omit.txt"
  python3 -I "$ROOT/tools/device.py" get kernel.lcd.target > "$GEN/lcd.txt" 2>/dev/null || echo "" > "$GEN/lcd.txt"
  BP=$(python3 -I "$ROOT/tools/device.py" get kernel.baseport 2>/dev/null | tr -d '"') || BP=syborg
  PAD=0
  if [ "$(python3 -I "$ROOT/tools/device.py" get kernel.place 2>/dev/null | tr -d '"')" = after-firmware ]; then
    CORE=$(python3 -I "$ROOT/tools/device.py" paths core_flat)
    PAD=$(stat -L -c %s "$CORE")	# core-flat.img is a link into build/firmware/ (device-model-v2)
  fi
  rm -f "$GEN/pad.bin"; [ "$PAD" -gt 0 ] && truncate -s "$PAD" "$GEN/pad.bin"
  CONTAINER_ARGS="${CONTAINER_ARGS:-} -e SYBORG_BASEPORT=${BP:-syborg} -e SYBORG_DEVDIR=/work/${DEVDIR#$ROOT/}" \
    exec "$ROOT/scripts/in-container.sh" /work/scripts/build-standins.sh "$VAR"
fi
source /work/scripts/env.sh
CFG="arm.v5.$VAR.gcce4_4_1.surge"
DEVDIR=${SYBORG_DEVDIR:?device build dir}
GEN=$DEVDIR/gen
BP=${SYBORG_BASEPORT:-syborg}
DST=${EPOCROOT}sf/adapt/qemu/baseport/$BP/standins
rm -rf "$DST" "${EPOCROOT}sf/adapt/qemu/baseport/$BP/n95compat"; cp -r /work/src/standins "$DST"
cp "$GEN/bld.inf" "$DST/bld.inf"
cd "$DST"
sbs -b bld.inf -c "$CFG" -f "$LOGS/standins-$BP-$VAR.log" > "$LOGS/standins-$BP-$VAR.out" 2>&1 || true
grep -E 'sbs : errors' "$LOGS/standins-$BP-$VAR.out" || true
if grep -q -E '<error|: error' "$LOGS/standins-$BP-$VAR.log"; then
  grep -E '<error|: error' "$LOGS/standins-$BP-$VAR.log" | head -20; exit 1
fi
REL=${EPOCROOT}epoc32/release/armv5/$VAR
OBY="${EPOCROOT}epoc32/rom/$BP/${BP}_tshell_ARMV5_${VAR}_rom.oby"
OUT=/work/build/rom-hybrid; mkdir -p "$OUT"; cd "$OUT"
NAME=${BP}_hybrid_kernel_ARMV5_$VAR
VARID=$(grep -m1 -o 'device\[0x[0-9a-f]*\]' "$OBY" | grep -o '0x[0-9a-f]*')
LCD=$(cat "$GEN/lcd.txt")
# rombuild lays files out in obey order: insert the stand-ins right after the first device[] line
# so they stay inside the kernel-side window below the user side (romcompose.py checks this)
while read -r kind f ext; do
  [ -n "$kind" ] || continue
  printf '%s[%s]=\t%s/_%s_%s.%s\t\\sys\\bin\\%s.%s\n' "$kind" "$VARID" "$REL" "$BP" "$f" "$ext" "$f" "$ext"
done < "$GEN/standins.oby.txt" > extra.oby
PAD=""; [ -f "$GEN/pad.bin" ] && PAD="$GEN/pad.bin"
OMIT=$(cat "$GEN/omit.txt" 2>/dev/null | tr '[:upper:]' '[:lower:]' | tr '\n' ' ')   # kernel.omit_baseport
awk -v name="$NAME" -v lcd="$LCD" -v bp="$BP" -v pad="$PAD" -v omit="$OMIT" '
  BEGIN { n = split(omit, o, " ") }
  { l = tolower($0); for (i = 1; i <= n; i++) if (o[i] != "" && index(l, "\\sys\\bin\\" o[i])) next }
  /^romname=/ { print "romname=" name ".img"; next }
  pad != "" && /^romsize=/ { print "romsize=0x4000000"; next }
  lcd != "" && index($0, "_" bp "_lcd.dll") { sub("_" bp "_lcd\\.dll", "_" bp "_" lcd ".dll") }
  { print }
  pad != "" && /^files=/ { print "data=" pad "\t\\sys\\data\\romcompose.pad" }
  /^device\[/ && !done { while ((getline l < "extra.oby") > 0) print l; done=1 }' "$OBY" > "$NAME.oby"
rombuild "$NAME.oby" > "$NAME.rombuild.log" 2>&1 || { tail -20 "$NAME.rombuild.log"; exit 1; }
ls -la "$OUT/$NAME.img"
cp "$OUT/$NAME.img" "$DEVDIR/kernel-side.img"
ls -la "$DEVDIR/kernel-side.img"
