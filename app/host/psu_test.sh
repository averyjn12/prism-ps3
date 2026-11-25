#!/bin/bash
# v0.67: the .psu save files (src/vmcw.c vmc_export_psu / vmc_import_psu) on COPIES of the owner's cards,
# judged by mymcplus, an outside PS2 card tool:
#   A. what Prism writes, mymcplus brings into a card it formatted itself: every file byte for byte
#      as on the source card (both read through mymcplus, ECC checked)
#   B. what mymcplus writes, Prism brings into a card: mymcplus finds no errors, every file identical
#   C. card -> .psu -> card by Prism alone is the card file a straight copy makes, byte for byte
#   D. damaged and wrong files are refused and the card file is not touched
#   E. every written page's ECC (vmc_inspect.py)
#   psu_test.sh <mymcplus.exe> [work dir]        exit code = failures
set -u -o pipefail
M="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
SRC="$HERE/../../launcher/mc_backup/2026-11-22"
W="${2:-/path/to/prism_psu_test}"      # on the Windows side: mymcplus.exe must reach it
B="$HOME/prism_app_host/psu_test"
MP="$(dirname "$M")/python.exe"
mkdir -p "$B"
gcc -O1 -g -Wall -Wno-unused-function -I"$HERE/stub" -I"$S" "$HERE/vmc_test.c" "$S/vmc.c" "${VMCW:-$S/vmcw.c}" -lm -o "$B/vmc_test" || exit 2
T="$B/vmc_test"
rm -rf "$W"; mkdir -p "$W/bak" "$W/orig" "$W/psu" "$W/mpsu"
cp "$SRC/OPL.VM2" "$W/orig/OPL.VM2"; cp "$SRC/Internal Memory Card ps2.VM2" "$W/orig/INT.VM2"; cp "$SRC/_NEW_0600324400893446.VM2" "$W/orig/NEW.VM2"
fail=0
win() { wslpath -w "$1"; }
run() { echo "-- $*" | sed "s#$W/##g"; "$T" "$W/bak" "$@" | sed 's/^/     /' | grep -v "^      " ; [ "${PIPESTATUS[0]}" = 0 ] || { echo "   ^^^ FAILED"; fail=$((fail+1)); }; }
mm() { "$M" "$@" 2>&1 | tr -d '\r'; }
cmpx() { out=$("$MP" "$(win "$HERE/mc_compare.py")" "$@" 2>&1 | tr -d '\r'); rc=$?; echo "$out" | sed 's/^/   /'; [ $rc -eq 0 ] || fail=$((fail+1)); }
same() { cmp -s "$1" "$2" && echo "   identical: $3" || { echo "   FAIL, differs: $3"; fail=$((fail+1)); }; }

OPL_SAVES="BASLUS-20946GTA50000 BASLUS-20202CZYTAXI APPS BOOT OPL SYS-CONF BEDATA-SYSTEM BASLUS-213592A000000"
echo "== A. Prism writes, mymcplus reads"
cp "$W/orig/OPL.VM2" "$W/A_src.VM2"
mm "$(win "$W/A_fresh.ps2")" format | sed 's/^/   /'
for s in $OPL_SAVES; do
    run export "$W/A_src.VM2" "$s" "$W/psu/$s.psu"
    out=$(mm "$(win "$W/A_fresh.ps2")" import "$(win "$W/psu/$s.psu")"); echo "   mymcplus import $s: ${out:-ok}"
done
same "$W/A_src.VM2" "$W/orig/OPL.VM2" "the source card after eight exports (never written)"
out=$(mm "$(win "$W/A_fresh.ps2")" check); echo "   fresh card check: $out"; echo "$out" | grep -q "No errors found" || fail=$((fail+1))
cmpx same "$(win "$W/orig/OPL.VM2")" "$(win "$W/A_fresh.ps2")" $OPL_SAVES

echo "== B. mymcplus writes, Prism reads"
cp "$W/orig/NEW.VM2" "$W/B_to.VM2"
( cd "$W/mpsu" && for s in BASLUS-20946GTA50000 BASLUS-20202CZYTAXI BOOT; do mm "$(win "$W/orig/OPL.VM2")" export "$s" | sed 's/^/   /'; done
  mm "$(win "$W/orig/INT.VM2")" export BASCUS-97134SOCOM | sed 's/^/   /' )
ls "$W/mpsu" | sed 's/^/   made by mymcplus: /'
for f in "$W"/mpsu/*.psu; do run import "$f" "$W/B_to.VM2"; done
out=$(mm "$(win "$W/B_to.VM2")" check); echo "   card check: $out"; echo "$out" | grep -q "No errors found" || fail=$((fail+1))
cmpx same "$(win "$W/orig/OPL.VM2")" "$(win "$W/B_to.VM2")" BASLUS-20946GTA50000 BASLUS-20202CZYTAXI BOOT
cmpx same "$(win "$W/orig/INT.VM2")" "$(win "$W/B_to.VM2")" BASCUS-97134SOCOM
run import "$W/mpsu/BOOT.psu" "$W/B_to.VM2" expect-fail          # already on the card
# Prism's file and mymcplus's file of the same save: the same entries and the same file bytes
"$MP" "$(win "$HERE/psu_same.py")" "$(win "$W/psu/BASLUS-20946GTA50000.psu")" "$(win "$W/mpsu/BASLUS-20946GTA50000.psu")" 2>&1 | tr -d '\r' | sed 's/^/   /'
[ "${PIPESTATUS[0]}" = 0 ] || fail=$((fail+1))

echo "== C. card -> .psu -> card is a straight copy"
cp "$W/orig/NEW.VM2" "$W/C_copy.VM2"; cp "$W/orig/NEW.VM2" "$W/C_psu.VM2"
for s in BASLUS-20946GTA50000 APPS BASLUS-20202CZYTAXI; do
    run copy "$W/orig/OPL.VM2" "$s" "$W/C_copy.VM2"
    run import "$W/psu/$s.psu" "$W/C_psu.VM2"
done
same "$W/C_copy.VM2" "$W/C_psu.VM2" "three saves by copy and by .psu"
same "$W/orig/OPL.VM2" "$SRC/OPL.VM2" "the source card after the copies"

echo "== D. refusals leave the card alone"
cp "$W/orig/NEW.VM2" "$W/D.VM2"
G="$W/psu/BASLUS-20946GTA50000.psu"
head -c 3000 "$G" > "$W/psu/cut_in_file.psu"
head -c 700 "$G" > "$W/psu/cut_in_entry.psu"
# cut inside the LAST file's bytes (past its filling): no later entry is there to give the cut away
head -c $(( $(stat -c %s "$G") - 1500 )) "$G" > "$W/psu/cut_in_last_file.psu"
head -c 100000 /dev/urandom > "$W/psu/noise.psu"
: > "$W/psu/empty.psu"
"$MP" "$(win "$HERE/psu_same.py")" make-bad "$(win "$G")" "$(win "$W/psu")" 2>&1 | tr -d '\r' | sed 's/^/   /'
for b in cut_in_file cut_in_entry cut_in_last_file noise empty bad_folder_inside bad_twice bad_count bad_name bad_dotname bad_notdir; do
    run import "$W/psu/$b.psu" "$W/D.VM2" expect-fail
done
same "$W/D.VM2" "$W/orig/NEW.VM2" "the card after eleven refused files"
[ -e "$W/bak/D.VM2.last" ] && { echo "   FAIL: a refused file still made a backup, so something was about to be written"; fail=$((fail+1)); }
run export "$W/orig/OPL.VM2" NO-SUCH-SAVE "$W/psu/none.psu" 2>/dev/null | grep -q "no save" && echo "   a save that is not there: nothing written"
[ -e "$W/psu/none.psu" ] && { echo "   FAIL: a file was made for a missing save"; fail=$((fail+1)); }

echo "== E. ECC of every page"
out=$("$MP" "$(win "$HERE/vmc_inspect.py")" "$(win "$W/B_to.VM2")" "$(win "$W/C_psu.VM2")" 2>&1 | tr -d '\r' | grep "pages,"); echo "$out" | sed 's/^/   /'
echo "$out" | grep -v "mismatch 0$" | grep -q . && { echo "   FAIL ECC"; fail=$((fail+1)); }
[ -n "$out" ] || { echo "   FAIL: the ECC count printed nothing"; fail=$((fail+1)); }

echo "== psu_test: $fail failure(s)"
exit $((fail > 0))
