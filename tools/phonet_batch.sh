#!/bin/bash
# Every read-only ISI request qembian's fake modem leaves unanswered (N8 Belle logs, 2026-11-11), sent to the real
# phone's modem over USB phonet. Phone in Nokia Suite mode; run from the repo root:  sudo bash tools/phonet_batch.sh
# Output: build/evidence/phonet/batch-<time>.txt (raw modem data: not for git).
# Left out on purpose (they change state): 08 07 (lock code check), 0B 66 (audio), 31 06 (GSS settings write).
set -u
cd "$(dirname "$0")/.."
modprobe phonet cdc_phonet 2>/dev/null
IF=$(ip -o link | awk -F': ' '/usbpn/ {print $2; exit}')
[ -n "$IF" ] || { echo "no usbpn interface: phone in Nokia Suite mode?"; exit 1; }
ip link set "$IF" up
OUT=build/evidence/phonet/batch-$(date +%Y%m%d-%H%M%S).txt; mkdir -p build/evidence/phonet
q() { echo "== $*" | tee -a "$OUT"; python3 tools/phonet_query.py --iface "$IF" --wait 1.5 --raw "$@" 2>&1 | grep -v "^phonet address" | tee -a "$OUT"; }
q 31 19
q 31 30
q 31 11 00 ff ff 00 00 00
q 54 0d 00 00
q 54 73 00 00
q 54 78 00 00
q 54 50 00 00
q c8 40
q c8 0b 00 02
q 09 23 25
q 09 60 fa 00
q 09 13 1a 00
q 09 13 28 00
q 09 13 38 00
q 09 13 74 00
q 09 dc 0f 01 00 e4 00 08 01 00 00 df
q 09 ba 64 00 ff ff 00 00 3f 00 7f 20 6f 48 00 00 00 00 00 00
q 09 c0 00 00
q 09 c0 01 00
q 15 02 00 00
echo "saved: $OUT"
