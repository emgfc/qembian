#!/usr/bin/env bash
# Old entry point: the firmware inventory is per device now (tools/fw/inventory.py, profile [firmware]).
#   N95_FW_DIR=/path/to/RM-320 scripts/n95-inventory.sh   ==   tools/fw/inventory.py --fw-dir DIR
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -I tools/fw/inventory.py --device "${SYBORG_DEVICE:-nokia-n95-8gb-rm320}" ${N95_FW_DIR:+--fw-dir "$N95_FW_DIR"} "$@"
