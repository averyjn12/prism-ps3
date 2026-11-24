#!/bin/bash
# Build the app's card reader for the PC (gcc, x86) and test it against the Linux-made cards.
#   wsl -d PSBBN -- bash "/path/to/PS3 software/lr/app/host/run_host_tests.sh"
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$HERE/../../tree/ps2emu_stage2/prism"
B="$HOME/prism_app_host"
mkdir -p "$B"
gcc -O2 -Wall -DPRISM_LR=5 -DPS2GXEMU -I"$HERE/../src" -I"$HERE/../src/stub" -I"$TREE" \
    "$HERE/fsport_host.c" "$HERE/fsprobe.c" "$TREE/lr_fs.c" -o "$B/fsprobe"
python3 "$HERE/test_fsport.py" "$B/fsprobe" "$B/cards"
