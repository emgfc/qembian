#!/usr/bin/env bash
# Device smoke test (docs/refactor-plan.md): the checks every platform change must pass.
#   tests/device-smoke.sh [DEVICE-ID] [--soft] [--keep] [--budget S]
# The boot budget (seconds to idle) is a property of the host running the test, not of the phone:
# --budget S or SMOKE_BUDGET, default 90.
# Boots the device headless in a throw-away instance (`smoke`, ephemeral: build/instances/smoke, made
# anew each run with tools/instance.py, never the owner's), from the
# device's seed drives build/seeds/<ID> (C:/E: images of a phone past its first-boot queries; git-ignored,
# made by hand: docs/adding-a-device.md), and checks: idle reached within the budget, the sydb agent up
# (boot hook), screen not black; on a profile with GL (`[gl] impl` not "none") the Multimedia key brings
# Carousel to the front without a panic (GL: host backend, or --soft); Switch off from the power menu
# ends the session by itself (H-modem-power).
# Exit 0 = all green. Prints one line per check and the idle time.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
DEV=nokia-n95-8gb-rm320; GL=host; KEEP=0; BUDGET=${SMOKE_BUDGET:-90}; next=
for a in "$@"; do
  if [ "$next" = budget ]; then BUDGET=$a; next=; continue; fi
  case "$a" in --soft) GL=soft ;; --keep) KEEP=1 ;; --budget) next=budget ;; -*) echo "unknown option $a" >&2; exit 2 ;; *) DEV=$a ;; esac
done
export SYBORG_DEVICE=$DEV
SEED=build/seeds/$DEV
[ -d "$SEED" ] || { echo "== smoke $DEV: no seed drives in $SEED (boot --fresh, answer the first-boot queries, copy <session>/drives there)"; exit 2; }
HASGL=1; [ "$(python3 tools/device.py get "$DEV" gl.impl 2>/dev/null)" = none ] && HASGL=0
# checks a profile declares not brought up yet: [status] smoke_skip = { check = "reason" }
skip_reason() { python3 -c 'import sys,json; sys.path.insert(0,"tools"); import device
d=device.load(sys.argv[1]).get("status",{}).get("smoke_skip",{}); print(d.get(sys.argv[2],""))' "$DEV" "$1" 2>/dev/null; }
S="tools/sydb/sydb.py --instance smoke"
fail=0
ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

$S stop >/dev/null 2>&1
python3 tools/instance.py create smoke "$DEV" --ephemeral --replace --audio wav --gl "$GL" >/dev/null || { echo "== smoke $DEV: no instance"; exit 2; }
SD=$(python3 tools/instance.py get smoke dir)
echo "== smoke $DEV (gl $GL, idle budget ${BUDGET}s)"
start=$($S start --seed "$SEED" --gl "$GL" --timeout "$BUDGET" 2>&1 | tail -1)
case "$start" in *"phone idle at"*) ok "boot: $start" ;; *) bad "boot: $start"; $S stop >/dev/null 2>&1; exit 1 ;; esac
case "$start" in *"agent up"*) ok "agent up" ;; *) bad "agent not responding" ;; esac
$S keepawake on >/dev/null 2>&1

# the idle marker can come before the home screen is drawn: retry for a few seconds
if python3 - "$SD/idle.png" <<'EOF'; then ok "screen not black"; else bad "screen is black"; fi
import subprocess, sys
import time
from PIL import Image
for i in range(10):
    subprocess.run(['tools/sydb/sydb.py', '--instance', 'smoke', 'screenshot', sys.argv[1]], capture_output=True)
    lo, hi = Image.open(sys.argv[1]).convert('L').getextrema()
    if hi - lo > 40:
        sys.exit(0)
    time.sleep(1)
sys.exit(1)
EOF

# Carousel: its boot instance starts ~10 s after idle; the Multimedia key toggles it
if [ $HASGL = 1 ]; then
for i in $(seq 40); do grep -aq "pbuffer surface 64x64" "$SD/serial.log" && break; sleep 1; done
sleep 3
$S key multimedia:0.2 >/dev/null 2>&1
sleep 6
if $S wg 2>/dev/null | grep '^\*' | grep -q Carousel; then ok "Multimedia key: Carousel in front"; else bad "Multimedia key: Carousel not in front"; fi
if $S events 2>/dev/null | grep -i '"thread.panic"' | grep -qi carousel; then bad "Carousel panicked"; else ok "no Carousel panic"; fi
grep -aq "host GL backend: $([ $GL = soft ] && echo off || echo on)" "$SD/serial.log" && ok "GL backend $GL" || bad "GL backend not $GL"
fi

if [ $KEEP = 1 ]; then echo "session kept: $S"; exit $fail; fi
# Switch off: hold Power for the power menu, its first item is "Switch off!"
why=$(skip_reason switch-off)
if [ -n "$why" ]; then echo "  skip  Switch off: $why"; $S stop >/dev/null 2>&1; echo "== $([ $fail = 0 ] && echo PASS || echo FAIL)"; exit $fail; fi
$S key end >/dev/null 2>&1; sleep 1
$S key power:0.3 >/dev/null 2>&1; sleep 2
$S key ok >/dev/null 2>&1
for i in $(seq 20); do sleep 1; $S status >/dev/null 2>&1 || break; done
if $S status >/dev/null 2>&1; then bad "Switch off: session still running"; $S stop >/dev/null 2>&1; else ok "Switch off ended the session"; fi
echo "== $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
