#!/bin/bash
# v0.67: the card writer after the save-package change (src/vmcw.c) against the writer as it was
# (host/backup_2026-11-24_pre67/vmcw.c): the same copies and deletes on copies of the owner's cards must
# leave byte-identical card files. (What the old writer wrote was judged by mymcplus in vmc_test.sh.)
#   bash host/vmcw_regress.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
SRC="$HERE/../../launcher/mc_backup/2026-11-22"
B="$HOME/prism_app_host/vmcw_regress"
rm -rf "$B"; mkdir -p "$B"
one() {   # $1 = tag, $2 = vmcw.c, $3 = extra source ("" or the old writer's stand-ins for the .psu calls)
    local W="$B/$1"
    mkdir -p "$W/bak"
    gcc -O1 -w -I"$HERE/stub" -I"$S" "$HERE/vmc_test.c" "$S/vmc.c" "$2" ${3:+"$3"} -lm -o "$B/vmc_test_$1" || exit 2
    cp "$SRC/OPL.VM2" "$W/OPL.VM2"; cp "$SRC/Internal Memory Card ps2.VM2" "$W/INT.VM2"; cp "$SRC/_NEW_0600324400893446.VM2" "$W/NEW.VM2"
    local T="$B/vmc_test_$1"
    run() { "$T" "$W/bak" "$@" > /dev/null 2>&1; echo "$? $*" | sed "s#$W/##g" >> "$W/log.txt"; }
    run copy "$W/OPL.VM2" BASLUS-20946GTA50000 "$W/NEW.VM2"
    run copy "$W/OPL.VM2" BASLUS-20202CZYTAXI "$W/NEW.VM2"
    run copy "$W/INT.VM2" BASCUS-97134SOCOM "$W/OPL.VM2"
    run delete "$W/OPL.VM2" BASLUS-213592A000000
    run copy "$W/NEW.VM2" BASLUS-20946GTA50000 "$W/INT.VM2"
    run copy "$W/OPL.VM2" APPS "$W/INT.VM2"
    run copy "$W/OPL.VM2" BASLUS-20946GTA50000 "$W/NEW.VM2" expect-fail
    run delete "$W/NEW.VM2" BASLUS-20946GTA50000
    run copy "$W/OPL.VM2" BOOT "$W/NEW.VM2"
    run copy "$W/INT.VM2" BASLUS-20946GTA50000 "$W/OPL.VM2" expect-fail
    (cd "$W" && sha256sum OPL.VM2 INT.VM2 NEW.VM2 log.txt > sums.txt)
}
one old "$HERE/backup_2026-11-24_pre67/vmcw.c" "$HERE/vmcw_old_stubs.c"
one new "$S/vmcw.c" ""
cat "$B/new/log.txt"
if cmp -s "$B/old/sums.txt" "$B/new/sums.txt" && ! grep -qv "^0 " "$B/new/log.txt"; then
    echo "vmcw_regress: ok, the three cards and the ten results are identical"
else
    echo "vmcw_regress: FAILED"; diff "$B/old/sums.txt" "$B/new/sums.txt"; exit 1
fi
cmp -s "$SRC/OPL.VM2" "$B/new/OPL.VM2" && { echo "vmcw_regress: FAILED, the card was never changed"; exit 1; }
exit 0
