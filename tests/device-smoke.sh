#!/usr/bin/env bash
# Device smoke test (docs/refactor-plan.md): the checks every platform change must pass.
#   tests/device-smoke.sh [DEVICE-ID] [--soft] [--keep]
# Boots the device headless in its own sydb session (build/sydb-smoke, never the owner's), from the
# clean seed drives, and checks: idle reached within the profile's budget, screen not black, the
# Multimedia key brings Carousel to the front without a panic (GL: host backend, or --soft), and
# Switch off from the power menu ends the session by itself (H-modem-power).
# Exit 0 = all green. Prints one line per check and the idle time.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
DEV=nokia-n95-8gb-rm320; GL=host; KEEP=0
for a in "$@"; do
  case "$a" in --soft) GL=soft ;; --keep) KEEP=1 ;; -*) echo "unknown option $a" >&2; exit 2 ;; *) DEV=$a ;; esac
done
S="tools/sydb/sydb.py --session build/sydb-smoke"
BUDGET=$(python3 -I tools/device.py get "$DEV" milestones.expected_idle_s 2>/dev/null || echo 26)
BUDGET=$(python3 -c "print(int(float('$BUDGET') * 1.5))")
fail=0
ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

$S stop >/dev/null 2>&1
rm -rf build/sydb-smoke
echo "== smoke $DEV (gl $GL, idle budget ${BUDGET}s)"
start=$($S start --seed build/runs/mtc-drives --gl "$GL" --timeout "$BUDGET" 2>&1 | tail -1)
case "$start" in *"phone idle at"*) ok "boot: $start" ;; *) bad "boot: $start"; $S stop >/dev/null 2>&1; exit 1 ;; esac
$S keepawake on >/dev/null 2>&1

python3 - "$ROOT/build/sydb-smoke/idle.png" <<'EOF' || bad "screen is black"
import subprocess, sys
subprocess.run(['tools/sydb/sydb.py', '--session', 'build/sydb-smoke', 'screenshot', sys.argv[1]], capture_output=True)
from PIL import Image
im = Image.open(sys.argv[1]).convert('L')
lo, hi = im.getextrema()
sys.exit(0 if hi - lo > 40 else 1)
EOF
[ $fail = 0 ] && ok "screen not black"

# Carousel: its boot instance starts ~10 s after idle; the Multimedia key toggles it
for i in $(seq 40); do grep -aq "pbuffer surface 64x64" build/sydb-smoke/serial.log && break; sleep 1; done
sleep 3
$S key multimedia:0.2 >/dev/null 2>&1
sleep 6
if $S wg 2>/dev/null | grep '^\*' | grep -q Carousel; then ok "Multimedia key: Carousel in front"; else bad "Multimedia key: Carousel not in front"; fi
if $S events 2>/dev/null | grep -i '"thread.panic"' | grep -qi carousel; then bad "Carousel panicked"; else ok "no Carousel panic"; fi
grep -aq "host GL backend: $([ $GL = soft ] && echo off || echo on)" build/sydb-smoke/serial.log && ok "GL backend $GL" || bad "GL backend not $GL"

if [ $KEEP = 1 ]; then echo "session kept: $S"; exit $fail; fi
# Switch off: hold Power for the power menu, its first item is "Switch off!"
$S key end >/dev/null 2>&1; sleep 1
$S key power:0.3 >/dev/null 2>&1; sleep 2
$S key ok >/dev/null 2>&1
for i in $(seq 20); do sleep 1; $S status >/dev/null 2>&1 || break; done
if $S status >/dev/null 2>&1; then bad "Switch off: session still running"; $S stop >/dev/null 2>&1; else ok "Switch off ended the session"; fi
echo "== $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
