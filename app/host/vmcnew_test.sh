#!/bin/bash
# v0.79: new memory cards and card names (src/vmcnew.c), judged from outside the code:
#   1. the PS2 card it makes IS the PS3's own empty card, byte for byte (sha256 of the owner's card as the
#      XMB made it, backed up 2026-11-22; the card file itself is compared too when it is on this PC)
#   2. the time stamp, against Python's calendar
#   3. the PS1 card is host/ps1_card.py's blank card (written apart, from psx-spx)
#   4. the app's own reader takes both: formatted, no saves, the free space of the real empty card
#   5. vmc_new: the file appears whole under its name, nothing else is left, a taken name is refused
#   6. vmc_rename: the bytes are untouched; a taken name, a bad name, a non-card are refused
#   7. a real save goes into the new card and comes out again the same
#   8. mymcplus, an outside PS2 card tool, finds no errors (when its path is given)
#   vmcnew_test.sh [mymcplus.exe]       exit code = failures.  VMCNEW=<file> tests another vmcnew.c (mutants)
set -u -o pipefail
M="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
SRC="$HERE/../../launcher/mc_backup/2026-11-22"
W="/path/to/prism_vmcnew_test"      # on the Windows side: mymcplus.exe must reach it
B="$HOME/prism_app_host/vmcnew_test"          # on the Linux side: file names keep their case here
REF_SHA=97d88855752c4d2c55d86816cd6fa50b58c0d0262b6a00c5da8658d394b6b64f
REF_STAMP=002e20090606e907
mkdir -p "$B"
gcc -O1 -g -Wall -Wno-unused-function -I"$HERE/stub" -I"$S" "$HERE/vmcnew_test.c" "$S/vmc.c" "$S/vmcw.c" "${VMCNEW:-$S/vmcnew.c}" -lm -o "$B/t" || { echo "BUILD-BROKEN"; exit 99; }
T="$B/t"
rm -rf "$W" "$B/d"; mkdir -p "$W/bak" "$B/d"
fail=0
ok() { echo "   ok    $1"; }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }
is() { [ "$1" = "$2" ] && ok "$3" || bad "$3: got [$1], want [$2]"; }
sha() { sha256sum "$1" | cut -c1-64; }
win() { wslpath -w "$1"; }

echo "== 1. the PS2 card against the PS3's own empty card"
"$T" ps2 "$W/gen.VM2" $REF_STAMP
is "$(sha "$W/gen.VM2")" "$REF_SHA" "the image with the real card's stamp has the real card's sha256"
is "$(stat -c %s "$W/gen.VM2")" 8650752 "its size"
if [ -f "$SRC/_NEW_0600324400893446.VM2" ]; then
    is "$(sha "$SRC/_NEW_0600324400893446.VM2")" "$REF_SHA" "the owner's card file still has that sha256"
    cmp -s "$W/gen.VM2" "$SRC/_NEW_0600324400893446.VM2" && ok "cmp with the owner's card file" || bad "cmp with the owner's card file"
else
    echo "   (the owner's card file is not on this PC: judged by the sha256 alone)"
fi
"$T" ps2 "$W/gen_other.VM2" 002e20090606e807
[ "$(sha "$W/gen_other.VM2")" != "$REF_SHA" ] && ok "calibration: another stamp gives another sha256" || bad "calibration: the sha256 does not see the stamp"

echo "== 2. the time stamp against Python"
for t in 0 86399 951782400 951868800 1749202366 1798761599 1798761600 4102444799; do
    want=$(python3 -c "
import datetime, sys
d = datetime.datetime(1970, 1, 1) + datetime.timedelta(seconds=int(sys.argv[1]) + 9 * 3600)
print('00%02x%02x%02x%02x%02x%02x%02x' % (d.second, d.minute, d.hour, d.day, d.month, d.year & 255, d.year >> 8))" $t)
    is "$("$T" stamp $t)" "$want" "stamp of $t"
done

echo "== 3. the PS1 card against ps1_card.py's blank card"
"$T" ps1 "$W/gen.VM1"
python3 -c "
import sys
sys.path.insert(0, sys.argv[1])
import ps1_card
open(sys.argv[2], 'wb').write(ps1_card.blank())" "$HERE" "$W/ref.VM1"
is "$(stat -c %s "$W/gen.VM1")" 131072 "its size"
cmp -s "$W/gen.VM1" "$W/ref.VM1" && ok "byte for byte the blank card of ps1_card.py" || bad "differs from ps1_card.py's blank card"

echo "== 4. the app's own reader"
is "$("$T" load "$W/gen.VM2" | head -1)" "ok 1 ps1 0 saves 0 free 8134" "the new PS2 card: formatted, empty, 8134 KB free (its 8134 free clusters)"
if [ -f "$SRC/_NEW_0600324400893446.VM2" ]; then
    is "$("$T" load "$SRC/_NEW_0600324400893446.VM2" | head -1)" "ok 1 ps1 0 saves 0 free 8134" "the owner's empty card reads the same"
fi
is "$("$T" load "$W/gen.VM1" | head -1)" "ok 1 ps1 1 saves 0 free 120" "the new PS1 card: formatted, empty, 15 blocks free"

echo "== 5. vmc_new (in $B/d, where names keep their case)"
D="$B/d"
is "$("$T" new "$D" "Prism PS2 Card 1" 0 1749202366)" "r 0 " "a PS2 card is made"
"$T" ps2 "$B/want.VM2" "$("$T" stamp 1749202366)"
cmp -s "$D/Prism PS2 Card 1.VM2" "$B/want.VM2" && ok "the file is the image for that time" || bad "the file is not the image for that time"
is "$("$T" new "$D" "Prism PS1 Card 1" 1 1749202366)" "r 0 " "a PS1 card is made"
cmp -s "$D/Prism PS1 Card 1.VM1" "$W/gen.VM1" && ok "the PS1 file is the blank card" || bad "the PS1 file is not the blank card"
is "$(ls "$D" | sort | tr '\n' '|')" "Prism PS1 Card 1.VM1|Prism PS2 Card 1.VM2|" "nothing else is left in the folder"
before=$(sha "$D/Prism PS2 Card 1.VM2")
is "$("$T" new "$D" "Prism PS2 Card 1" 0 1800000000)" "r -2 a card of that name is already there" "the same name again is refused"
is "$("$T" new "$D" "prism ps2 card 1" 0 1800000000)" "r -2 a card of that name is already there" "the same name in small letters is refused"
is "$("$T" new "$D" "Prism PS2 Card 1" 1 1800000000)" "r -2 a card of that name is already there" "a PS1 card of a PS2 card's name is refused"
is "$(sha "$D/Prism PS2 Card 1.VM2")" "$before" "the card that was there is untouched"
is "$("$T" new "$D" "bad/name" 0 1800000000 | cut -c1-4)" "r -1" "a bad name is refused"
is "$("$T" new "$D/none" "X" 0 1800000000)" "r -2 the memory card folder cannot be read" "a missing folder is refused"
is "$(ls "$D" | wc -l)" 2 "still two files"
# a write that fails part way (the file size limit stands in for a full disk): refused, and no half-made file stays
is "$( ( trap '' XFSZ; ulimit -f 1000; "$T" new "$D" "Too big" 0 1749202366 ) 2>&1 )" "r -4 the card file could not be written (is the hard disk full?)" "a write that fails part way is refused"
is "$(ls "$D" | wc -l)" 2 "and leaves no half-made file"

echo "== 6. vmc_rename"
is "$("$T" rename "$D" "Prism PS2 Card 1.VM2" "Tekken saves")" "r 0 [Tekken saves.VM2] " "renamed"
is "$(sha "$D/Tekken saves.VM2")" "$before" "the renamed card's bytes are the same"
[ ! -e "$D/Prism PS2 Card 1.VM2" ] && ok "the old name is gone" || bad "the old name is still there"
is "$("$T" rename "$D" "Tekken saves.VM2" "Prism PS1 Card 1" | cut -c1-5)" "r -4 " "another card's name is refused"
is "$("$T" rename "$D" "Tekken saves.VM2" "PRISM PS1 CARD 1" | cut -c1-5)" "r -4 " "another card's name in capitals is refused"
is "$("$T" rename "$D" "Tekken saves.VM2" "Tekken saves" | cut -c1-5)" "r -3 " "its own name is refused"
is "$("$T" rename "$D" "Tekken saves.VM2" "TEKKEN SAVES")" "r 0 [TEKKEN SAVES.VM2] " "its own name in capitals is a rename"
is "$("$T" rename "$D" "TEKKEN SAVES.VM2" "a.b" | cut -c1-5)" "r -2 " "a bad name is refused"
is "$("$T" rename "$D" "TEKKEN SAVES.VM2" "" | cut -c1-5)" "r -2 " "an empty name is refused"
echo x > "$D/notes.txt"
is "$("$T" rename "$D" "notes.txt" "Notes" | cut -c1-5)" "r -1 " "a file that is not a card is refused"
is "$("$T" rename "$D" "gone.VM2" "Here" | cut -c1-5)" "r -5 " "a card that is not there is refused"
is "$("$T" rename "$D" "../d/TEKKEN SAVES.VM2" "Up" | cut -c1-5)" "r -1 " "a path is refused"
rm "$D/notes.txt"
ps1sha=$(sha "$D/Prism PS1 Card 1.VM1")
is "$("$T" rename "$D" "Prism PS1 Card 1.VM1" "MGS saves")" "r 0 [MGS saves.VM1] " "a PS1 card renamed stays a PS1 card"
is "$(sha "$D/MGS saves.VM1")" "$ps1sha" "with its bytes"
is "$("$T" rename "$D" "MGS saves.VM1" "Prism PS1 Card 1")" "r 0 [Prism PS1 Card 1.VM1] " "and back"
is "$(ls "$D" | sort | tr '\n' '|')" "Prism PS1 Card 1.VM1|TEKKEN SAVES.VM2|" "the folder holds the two cards"
is "$(sha "$D/TEKKEN SAVES.VM2")" "$before" "and the PS2 card's bytes never changed"
is "$("$T" names | tail -1)" "names: 0 failure(s)" "the name rules, 44 cases"

echo "== 7. a real save into the new card and out again"
if [ -f "$SRC/OPL.VM2" ]; then
    for s in BASLUS-20946GTA50000 BASLUS-20202CZYTAXI; do
        is "$("$T" export "$SRC/OPL.VM2" $s "$W/$s.psu")" "r 0 " "$s out of the owner's card"
        is "$("$T" import "$W/bak" "$W/gen.VM2" "$W/$s.psu")" "r 0 " "$s into the new card"
        is "$("$T" export "$W/gen.VM2" $s "$W/$s.again.psu")" "r 0 " "$s out of the new card"
        cmp -s "$W/$s.psu" "$W/$s.again.psu" && ok "$s: the same save file both times" || bad "$s: the save file changed on the way through the new card"
    done
    is "$("$T" load "$W/gen.VM2" | head -1 | cut -d' ' -f1-6)" "ok 1 ps1 0 saves 2" "the new card now holds two saves"
else
    echo "   (the owner's cards are not on this PC: skipped)"
fi

echo "== 8. mymcplus"
if [ -n "$M" ] && [ -x "$M" ]; then
    "$T" ps2 "$W/m_empty.VM2" "$("$T" stamp 1749202366)"
    out=$("$M" "$(win "$W/m_empty.VM2")" check 2>&1 | tr -d '\r'); echo "$out" | grep -q "No errors found" && ok "the empty new card: $out" || bad "mymcplus on the empty new card: $out"
    out=$("$M" "$(win "$W/m_empty.VM2")" df 2>&1 | tr -d '\r'); echo "   mymcplus df: $out"
    if [ -f "$SRC/OPL.VM2" ]; then
        out=$("$M" "$(win "$W/gen.VM2")" check 2>&1 | tr -d '\r'); echo "$out" | grep -q "No errors found" && ok "the new card with two saves: $out" || bad "mymcplus on the new card with saves: $out"
        "$M" "$(win "$W/gen.VM2")" dir 2>&1 | tr -d '\r' | sed 's/^/   mymcplus dir: /' | head -8
    fi
else
    echo "   (no mymcplus given: skipped)"
fi
echo "$fail failure(s)"
exit $fail
