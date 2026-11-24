#!/bin/bash
# The host test must FAIL on a known-bad adapter: build fsprobe with every read one sector late
# and with the partition table ignored by nothing else changed; expect failures.
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$HERE/../../tree/ps2emu_stage2/prism"
B="$HOME/prism_app_host/mut"
mkdir -p "$B"
sed 's/(long)lba \* 512/(long)(lba + 1) * 512/' "$HERE/fsport_host.c" > "$B/fsport_host_mut.c"
grep -c "(lba + 1)" "$B/fsport_host_mut.c" || { echo "mutation not applied"; exit 2; }
gcc -O2 -DPRISM_LR=5 -DPS2GXEMU -I"$HERE" -I"$HERE/../src" -I"$HERE/../src/stub" -I"$TREE" \
    "$B/fsport_host_mut.c" "$HERE/fsprobe.c" "$TREE/lr_fs.c" -o "$B/fsprobe"
python3 "$HERE/test_fsport.py" "$B/fsprobe" "$B/cards" > "$B/out.txt" 2>&1
rc=$?
tail -1 "$B/out.txt"
[ $rc -ne 0 ] && echo "MUTANT KILLED (test exit $rc)" || echo "MUTANT SURVIVED"
