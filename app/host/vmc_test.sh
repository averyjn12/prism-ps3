#!/bin/bash
# The app's card writer on COPIES of the owner's cards, judged by an outside reader (2026-11-22).
#   vmc_test.sh <card backups dir> <work dir> <mymcplus.exe> <windows python>
# Every result card must: pass `mymcplus check`; export every save with no ECC message; hold
# every copied file byte for byte as on the source card (both read by mymcplus); and have every
# written page's ECC right (vmc_inspect.py). PS1: tools/ps1_card.py builds a card with a save and
# checks the result independently.
set -u -o pipefail
SRC="$1"; W="$2"; M="$3"; PY="$4"
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
B="$HOME/prism_app_host/vmc_test"
mkdir -p "$B"
gcc -O1 -g -Wall -Wno-unused-function -I"$HERE/stub" -I"$S" "$HERE/vmc_test.c" "$S/vmc.c" "${VMCW:-$S/vmcw.c}" -lm -o "$B/vmc_test" || exit 2
T="$B/vmc_test"
rm -rf "$W"; mkdir -p "$W/bak" "$W/orig"
cp "$SRC/OPL.VM2" "$W/OPL.VM2"; cp "$SRC/Internal Memory Card ps2.VM2" "$W/INT.VM2"; cp "$SRC/_NEW_0600324400893446.VM2" "$W/NEW.VM2"
cp "$W"/*.VM2 "$W/orig/"
fail=0
run() { echo "-- $*"; "$T" "$W/bak" "$@" || { echo "   ^^^ FAILED"; fail=$((fail+1)); }; }

run copy "$W/OPL.VM2" BASLUS-20946GTA50000 "$W/NEW.VM2"      # root 2 -> 3: the root grows a cluster
run copy "$W/OPL.VM2" BASLUS-20202CZYTAXI "$W/NEW.VM2"       # root 3 -> 4: same cluster
run copy "$W/INT.VM2" BASCUS-97134SOCOM "$W/OPL.VM2"         # OPL root 10 -> 11: grows a cluster
run delete "$W/OPL.VM2" BASLUS-213592A000000                 # MGS3 off OPL
run copy "$W/NEW.VM2" BASLUS-20946GTA50000 "$W/INT.VM2"      # a copy of a copy
run copy "$W/OPL.VM2" APPS "$W/INT.VM2"                      # APPS holds a DELETED entry: left behind
run copy "$W/OPL.VM2" BASLUS-20946GTA50000 "$W/NEW.VM2" expect-fail   # already there
run delete "$W/NEW.VM2" BASLUS-20946GTA50000
run copy "$W/OPL.VM2" BOOT "$W/NEW.VM2"                      # reuses the freed root slot 2
run copy "$W/INT.VM2" BASLUS-20946GTA50000 "$W/OPL.VM2" expect-fail   # same name on OPL

echo "== backups kept"
ls -la "$W/bak" | awk 'NR>1{print "   "$5, $9}'
cmp -s "$W/bak/OPL.VM2.orig" "$W/orig/OPL.VM2" && echo "   OPL.orig = the card before any change" || { echo "   FAIL OPL.orig"; fail=$((fail+1)); }

win() { wslpath -w "$1"; }
echo "== mymcplus (outside reader)"
for c in OPL INT NEW; do
  out=$("$M" "$(win "$W/$c.VM2")" check 2>&1 | tr -d '\r'); echo "   $c check: $out"
  echo "$out" | grep -q "No errors found" || { echo "   FAIL $c check"; fail=$((fail+1)); }
  "$M" "$(win "$W/$c.VM2")" dir 2>&1 | tr -d '\r' | sed 's/^/      /'
done

# every copied save, read by mymcplus (ECC checked), byte for byte against the card it came from;
# saves NOT touched, against the original card; deleted saves, absent
MP="$(dirname "$M")/python.exe"
cmpx() { out=$("$MP" "$(win "$HERE/mc_compare.py")" "$@" 2>&1 | tr -d '\r'); rc=$?; echo "$out" | sed 's/^/   /'; [ $rc -eq 0 ] || fail=$((fail+1)); }
echo "== contents (mymcplus)"
cmpx same "$(win "$W/orig/OPL.VM2")" "$(win "$W/NEW.VM2")" BASLUS-20202CZYTAXI BOOT
cmpx same "$(win "$W/orig/INT.VM2")" "$(win "$W/OPL.VM2")" BASCUS-97134SOCOM
cmpx same "$(win "$W/orig/OPL.VM2")" "$(win "$W/INT.VM2")" BASLUS-20946GTA50000 APPS
cmpx same "$(win "$W/orig/OPL.VM2")" "$(win "$W/OPL.VM2")" APPS BOOT OPL SYS-CONF BEDATA-SYSTEM BASLUS-20946GTA50000 BASLUS-20202CZYTAXI
cmpx same "$(win "$W/orig/INT.VM2")" "$(win "$W/INT.VM2")" BASCUS-97134SOCOM
cmpx absent "$(win "$W/OPL.VM2")" BASLUS-213592A000000
cmpx absent "$(win "$W/NEW.VM2")" BASLUS-20946GTA50000

echo "== ECC of every page"
"$PY" "$(win "$HERE/vmc_inspect.py")" "$(win "$W/OPL.VM2")" "$(win "$W/INT.VM2")" "$(win "$W/NEW.VM2")" 2>&1 | tr -d '\r' | grep "pages,"
"$PY" "$(win "$HERE/vmc_inspect.py")" "$(win "$W/OPL.VM2")" "$(win "$W/INT.VM2")" "$(win "$W/NEW.VM2")" 2>&1 | tr -d '\r' | grep "pages," | grep -v "mismatch 0$" && { echo "   FAIL ECC"; fail=$((fail+1)); }

echo "== PS1"
PS1=$("$PY" "$(win "$HERE/ps1_card.py")" make "$(win "$W/P1A.VM1")" "$(win "$W/P1B.VM1")" 2>&1 | tr -d '\r'); echo "$PS1"
run copy "$W/P1A.VM1" BASLUS-00594MGS0 "$W/P1B.VM1"
run copy "$W/P1A.VM1" BASLUS-00594MGS0 "$W/P1B.VM1" expect-fail
run delete "$W/P1A.VM1" BASLUS-00594MGS0
out=$("$PY" "$(win "$HERE/ps1_card.py")" check "$(win "$W/P1A.VM1")" "$(win "$W/P1B.VM1")" 2>&1 | tr -d '\r'); echo "$out"
echo "$out" | grep -q "^PS1 OK" || { echo "   FAIL PS1"; fail=$((fail+1)); }

echo "== $fail failure(s)"
exit $((fail > 0))
