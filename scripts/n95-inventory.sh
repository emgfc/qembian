#!/usr/bin/env bash
# Read-only inventory of an N95 8GB (RM-320) firmware set. Nothing from the firmware is
# committed; all outputs go to build/n95/ (git-ignored).
#   N95_FW_DIR=/path/to/RM-320 scripts/n95-inventory.sh
# Host requirements: python3; an arm-none-symbianelf objdump/nm (ARM_OBJDUMP, default GCC 14
# build at ~/gcc-builds/gcc-14.2.0); the project's built epocroot (for our EUSER + execs.txt).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
: "${N95_FW_DIR:?set N95_FW_DIR to the directory holding RM-320_*.C0R}"
SDK_EUSER="${SDK_EUSER:-$HOME/work/S60_3rd_FP1/Epoc32/release/armv5/lib/euser.dso}"
EKA2L1_SVC="${EKA2L1_SVC:-$HOME/work/EKA2L1/src/emu/kernel/src/svc.cpp}"
K=build/epocroot/sf/os/kernelhwsrv/kernel/eka
O=build/n95; mkdir -p $O/abi
C0R=$(ls "$N95_FW_DIR"/*.C0R | head -1)
sha256sum "$C0R" > $O/firmware.sha256
python3 -I tools/fiasco.py "$C0R" --out $O/c0r > $O/c0r-partitions.txt
python3 -I tools/rom_unpage.py $O/c0r/SOS_CORE@*.bin $O/core-flat.img
python3 -I tools/romimg.py $O/core-flat.img --images --json $O/core.json > $O/core-files.txt
EU=$(awk '/Sys\/Bin\/EUser.dll$/{print $1}' $O/core-files.txt)
python3 -I tools/swi_map.py --rom-image $O/core-flat.img --at "$EU" --names "$SDK_EUSER" --names $K/eabi/euseru.def -o $O/abi/n95-euser-swi.json
python3 -I tools/swi_map.py --elf build/epocroot/epoc32/release/armv5/udeb/_syborg_euser.dll.sym -o $O/abi/our-euser-swi.json
python3 -I tools/swi_map.py --compare $O/abi/n95-euser-swi.json $O/abi/our-euser-swi.json --execs $K/kernel/execs.txt > $O/abi/swi-n95-vs-ours.txt
python3 -I tools/xip_imports.py $O/core-flat.img $O/core.json --ordinals $O/abi/euser-ordinal-usage.tsv | tee $O/abi/xip-imports.txt
tail -1 $O/abi/swi-n95-vs-ours.txt
