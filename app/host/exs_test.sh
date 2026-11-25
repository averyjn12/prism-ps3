#!/bin/bash
# v0.74: tests for what PS2 save files on an exFAT-only card need (src/exfat_w.c exw_mkdir, exw_list,
# exw_read), run in WSL:   bash host/exs_test.sh
# Judged by tools Prism does not own, as exw_test.sh and exd_test.sh: fsck.exfat -n, and exfat-fuse, which
# must see the folder the writer made, keep working in it (add 300 files: the folder has to grow), and agree
# with the writer's own listing and file reads. The free clusters are filled with junk first, so a folder
# cluster that was not written as zeros shows up as junk entries.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
W=/tmp/exs
sudo umount "$W/mnt" 2>/dev/null
rm -rf "$W"; mkdir -p "$W/mnt"
gcc -O2 -Wall -Wextra -I"$SRC" "$HERE/exw_test.c" "${EXW:-$SRC/exfat_w.c}" -o "$W/exw_test" || exit 2
T="$W/exw_test"
pass=0; fail=0
ok()  { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }
LOOP=""
mnt() { LOOP=$(sudo losetup -f --show -o $(( $2 * 512 )) "$1") && sudo mount.exfat-fuse "$LOOP" "$W/mnt" -o allow_other >/dev/null 2>&1; }
umnt() { sudo umount "$W/mnt"; sudo losetup -d "$LOOP"; LOOP=""; }
fsck_ok() { local L; L=$(sudo losetup -f --show -o $(( $2 * 512 )) "$1"); sudo fsck.exfat -n "$L" > "$W/fsck.txt" 2>&1; local r=$?; sudo losetup -d "$L"; return $r; }
snap() { mnt "$1" "$2" || return 9; (cd "$W/mnt" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$3"; umnt; }
mkimg() { # name MB cluster: PS2ISO with one game, and every free cluster holding junk
	truncate -s "${2}M" "$W/$1"
	mkfs.exfat -c "$3" -L PRISMTEST "$W/$1" > /dev/null
	mnt "$W/$1" 0
	sudo mkdir "$W/mnt/PS2ISO"
	sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/OLD GAME.iso" bs=1M count=3 status=none
	sudo dd if=/dev/urandom of="$W/mnt/junk.bin" bs=1M status=none 2>/dev/null
	sudo rm -f "$W/mnt/junk.bin"
	umnt
}

# ---- 1. a folder made in the root ----
mkimg t1.img 64 32K
snap "$W/t1.img" 0 "$W/t1.before"
if "$T" --mkdir "$W/t1.img" PS2SAVES > "$W/t1.out"; then
	fsck_ok "$W/t1.img" 0 && ok "new folder: volume clean" || bad "new folder: fsck: $(tail -3 $W/fsck.txt)"
	mnt "$W/t1.img" 0
	[ -d "$W/mnt/PS2SAVES" ] && ok "new folder: exfat-fuse sees a folder" || bad "new folder: exfat-fuse sees no folder"
	[ -z "$(ls -A "$W/mnt/PS2SAVES" 2>&1)" ] && ok "new folder: it is empty (no junk entries)" || bad "new folder: not empty: $(ls -A "$W/mnt/PS2SAVES" 2>&1 | head -3 | tr '\n' ' ')"
	umnt
	snap "$W/t1.img" 0 "$W/t1.after"
	cmp -s "$W/t1.before" "$W/t1.after" && ok "new folder: every file reads as before" || bad "new folder: other files changed"
else bad "new folder: $(cat $W/t1.out)"; fi
sha256sum < "$W/t1.img" > "$W/t1.sha"
"$T" --mkdir "$W/t1.img" ps2saves > "$W/t1.out" && bad "the same folder made twice" || ok "same name again (other case) refused: $(head -1 $W/t1.out)"
"$T" --mkdir "$W/t1.img" PS2ISO > "$W/t1.out" && bad "an existing folder made again" || ok "existing folder refused"
"$T" --mkdir "$W/t1.img" 'bad:name' > "$W/t1.out" && bad "a name with a colon accepted" || ok "a name exFAT cannot hold refused"
sha256sum < "$W/t1.img" | cmp -s - "$W/t1.sha" && ok "after the refusals the image is byte-identical" || bad "a refusal changed the image"

# ---- 2. the writer puts a file in the folder it made; exfat-fuse reads it; the writer lists and reads it ----
"$T" "$W/t1.img" PS2SAVES "BASLUS-20202CRAZY.psu" 70123 5 > "$W/t2.out" || bad "file into the new folder: $(cat $W/t2.out)"
fsck_ok "$W/t1.img" 0 && ok "file in the new folder: volume clean" || bad "file in the new folder: fsck: $(tail -3 $W/fsck.txt)"
"$T" --gen "$W/expect.bin" 70123 5
mnt "$W/t1.img" 0; cmp -s "$W/mnt/PS2SAVES/BASLUS-20202CRAZY.psu" "$W/expect.bin" && ok "file in the new folder: exfat-fuse reads it exactly" || bad "file in the new folder: bytes differ"; umnt
"$T" --cat "$W/t1.img" PS2SAVES "baslus-20202crazy.PSU" 2>"$W/t2.err" | cmp -s - "$W/expect.bin" && [ ! -s "$W/t2.err" ] && ok "the writer reads its own file back (name in another case)" || bad "the writer's read differs"
[ "$("$T" --ls "$W/t1.img" PS2SAVES)" = "70123 BASLUS-20202CRAZY.psu" ] && ok "the writer lists it: $("$T" --ls "$W/t1.img" PS2SAVES)" || bad "the writer's list: $("$T" --ls "$W/t1.img" PS2SAVES)"

# ---- 3. exfat-fuse keeps working in the writer-made folder: 300 files make it grow ----
mnt "$W/t1.img" 0
for i in $(seq 1 300); do sudo dd if=/dev/urandom of="$W/mnt/PS2SAVES/$(printf 'SAVE %03d with a longer name.psu' $i)" bs=$((700 + i * 13)) count=1 status=none; done
sudo dd if=/dev/urandom of="$W/mnt/PS2SAVES/Pokémon – ünïcode.psu" bs=4321 count=1 status=none
sudo mkdir "$W/mnt/PS2SAVES/A FOLDER"
(cd "$W/mnt/PS2SAVES" && for f in *; do if [ -d "$f" ]; then echo "d $f"; else echo "$(stat -c %s "$f") $f"; fi; done | sort) > "$W/t3.fuse"
(cd "$W/mnt/PS2SAVES" && sha256sum "SAVE 001 with a longer name.psu" "SAVE 150 with a longer name.psu" "SAVE 300 with a longer name.psu" "Pokémon – ünïcode.psu" | cut -d' ' -f1) > "$W/t3.sha"
umnt
fsck_ok "$W/t1.img" 0 && ok "301 more files by exfat-fuse in the writer's folder: volume clean" || bad "grown folder: fsck: $(tail -3 $W/fsck.txt)"
"$T" --ls "$W/t1.img" PS2SAVES | sort > "$W/t3.writer"
cmp -s "$W/t3.fuse" "$W/t3.writer" && ok "the writer lists the same $(wc -l < $W/t3.writer) names and sizes as exfat-fuse (one with accents, one folder)" || bad "lists differ: $(diff "$W/t3.fuse" "$W/t3.writer" | head -4 | tr '\n' ' ')"
for f in "SAVE 001 with a longer name.psu" "SAVE 150 with a longer name.psu" "SAVE 300 with a longer name.psu" "Pokémon – ünïcode.psu"; do "$T" --cat "$W/t1.img" PS2SAVES "$f" 2>/dev/null | sha256sum | cut -d' ' -f1; done > "$W/t3.wsha"
cmp -s "$W/t3.sha" "$W/t3.wsha" && ok "the writer reads four of them exactly as exfat-fuse does" || bad "reads differ"
"$T" --cat "$W/t1.img" PS2SAVES "A FOLDER" > /dev/null 2>"$W/t3.err" && bad "a folder was read as a file" || ok "reading a folder as a file refused"
"$T" --cat "$W/t1.img" PS2SAVES "NOT THERE.psu" > /dev/null 2>"$W/t3.err" && bad "a missing file was read" || ok "reading a missing file refused"
"$T" --ls "$W/t1.img" NOFOLDER > /dev/null 2>"$W/t3.err" && bad "a missing folder was listed" || ok "listing a missing folder refused"

# ---- 4. a file in pieces (a FAT chain) is read exactly ----
mkimg t4.img 96 32K
mnt "$W/t4.img" 0; sudo mkdir "$W/mnt/PS2SAVES"
for i in $(seq 1 12); do sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/f$i" bs=1M count=6 status=none 2>/dev/null || break; done
for i in 2 4 6 8 10; do sudo rm -f "$W/mnt/PS2ISO/f$i"; done
sudo dd if=/dev/urandom of="$W/mnt/PS2SAVES/PIECES.psu" bs=1M count=14 status=none
sha256sum < "$W/mnt/PS2SAVES/PIECES.psu" > "$W/t4.sha"
umnt
python3 - "$W/t4.img" > "$W/t4.flag" <<'EOF'
import sys
img=open(sys.argv[1],'rb').read()
i=img.find('PIECES.psu'.encode('utf-16-le')); st=i-2-32
print("nofat" if img[st+1] & 2 else "chain")
EOF
[ "$(cat $W/t4.flag)" = chain ] && ok "the 14 MB test file is in pieces" || bad "the test file is not fragmented: this case tests nothing"
"$T" --cat "$W/t4.img" PS2SAVES "PIECES.psu" 2>/dev/null | sha256sum | cmp -s - "$W/t4.sha" && ok "a file in pieces is read exactly" || bad "a file in pieces reads differently"

# ---- 5. 4 KB clusters, and a card layout (MBR, FAT32 first): mkdir, then delete a file in it ----
truncate -s 384M "$W/t5.img"
printf 'label: dos\nstart=2048, size=131072, type=c\nstart=133120, type=7\n' | sfdisk -q "$W/t5.img"
L=$(sudo losetup -f --show -o $((2048 * 512)) --sizelimit $((131072 * 512)) "$W/t5.img"); sudo mkfs.vfat -F 32 "$L" > /dev/null; sudo losetup -d "$L"
L=$(sudo losetup -f --show -o $((133120 * 512)) "$W/t5.img"); sudo mkfs.exfat -c 4K "$L" > /dev/null; sudo losetup -d "$L"
head -c $((133120 * 512)) "$W/t5.img" | sha256sum > "$W/t5.before"
"$T" --mkdir "$W/t5.img" PS2SAVES > "$W/t5.out" || bad "card layout mkdir: $(cat $W/t5.out)"
"$T" "$W/t5.img" PS2SAVES "ONE.psu" 9000 3 > "$W/t5.out" || bad "card layout file: $(cat $W/t5.out)"
"$T" "$W/t5.img" PS2SAVES "TWO.psu" 12345 4 > "$W/t5.out" || bad "card layout file 2: $(cat $W/t5.out)"
"$T" --del "$W/t5.img" PS2SAVES "ONE.psu" > "$W/t5.out" || bad "card layout delete: $(cat $W/t5.out)"
fsck_ok "$W/t5.img" 133120 && ok "card layout, 4 KB clusters: folder, two files, one deleted: exFAT part clean" || bad "card layout: fsck: $(tail -3 $W/fsck.txt)"
[ "$("$T" --ls "$W/t5.img" PS2SAVES)" = "12345 TWO.psu" ] && ok "card layout: the folder lists the one file left" || bad "card layout list: $("$T" --ls "$W/t5.img" PS2SAVES)"
head -c $((133120 * 512)) "$W/t5.img" | sha256sum | cmp -s - "$W/t5.before" && ok "card layout: MBR and FAT32 part byte-identical" || bad "card layout: the area before the exFAT part changed"

# ---- 5b. a FULL volume with one free cluster between used ones: the folder takes exactly that one ----
truncate -s 24M "$W/t5b.img"; mkfs.exfat -c 32K "$W/t5b.img" > /dev/null
mnt "$W/t5b.img" 0
i=0; while sudo dd if=/dev/urandom of="$W/mnt/$(printf 'f%04d.bin' $i)" bs=1k count=20 status=none 2>/dev/null; do i=$((i+1)); [ $i -gt 3000 ] && break; done
sudo rm -f "$W/mnt/$(printf 'f%04d.bin' $i)"		# the one that did not fit
HOLE=$(printf 'f%04d.bin' $((i / 2)))
sudo rm -f "$W/mnt/$HOLE"
umnt
FREE=$("$T" --space "$W/t5b.img" | cut -d' ' -f1)
[ "$FREE" = 32768 ] && ok "the full test volume has exactly one free cluster ($i files made)" || bad "the test volume has $FREE bytes free, not one cluster: this case tests nothing"
snap "$W/t5b.img" 0 "$W/t5b.before"
"$T" --mkdir "$W/t5b.img" PS2SAVES > "$W/t5b.out" || bad "hole: $(cat $W/t5b.out)"
fsck_ok "$W/t5b.img" 0 && ok "folder into the one free cluster: volume clean" || bad "folder into a hole: fsck: $(tail -3 $W/fsck.txt)"
snap "$W/t5b.img" 0 "$W/t5b.after"
cmp -s "$W/t5b.before" "$W/t5b.after" && ok "folder into the one free cluster: every file reads as before" || bad "folder into a hole: a neighbour changed"
"$T" --mkdir "$W/t5b.img" MORE > "$W/t5b.out" && bad "a folder made on a full volume" || ok "full volume: another folder refused: $(head -1 $W/t5b.out)"

# ---- 6. reading and listing change nothing ----
sha256sum < "$W/t1.img" > "$W/t6.sha"
"$T" --ls "$W/t1.img" PS2SAVES > /dev/null; "$T" --cat "$W/t1.img" PS2SAVES "SAVE 150 with a longer name.psu" > /dev/null 2>&1
sha256sum < "$W/t1.img" | cmp -s - "$W/t6.sha" && ok "listing and reading change nothing" || bad "listing or reading changed the image"

echo "---- $pass passed, $fail failed"
[ $fail = 0 ]
