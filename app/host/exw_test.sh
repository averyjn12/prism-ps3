#!/bin/bash
# Tests for the exFAT writer (src/exfat_w.c), run in WSL:  bash host/exw_test.sh
# Each test builds a fresh exFAT image with mkfs.exfat, puts files in it through exfat-fuse (an
# independent exFAT driver), adds a file with the writer, then checks with tools Prism does not own:
#   fsck.exfat -n  (no repair) must report the volume clean, and
#   exfat-fuse must read back exactly the bytes the writer was given (cmp against --gen).
# Calibration first: fsck.exfat must FAIL on a planted bad SetChecksum and a cleared bitmap bit,
# or its silence elsewhere means nothing.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
W=/tmp/exw
sudo umount "$W/mnt" 2>/dev/null
rm -rf "$W"; mkdir -p "$W/mnt"
gcc -O2 -Wall -Wextra -I"$SRC" "$HERE/exw_test.c" "${EXW:-$SRC/exfat_w.c}" -o "$W/exw_test" || exit 1
T="$W/exw_test"
pass=0; fail=0
ok()  { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }
LOOP=""

# mount image $1 at sector offset $2 (exfat-fuse through a loop device)
mnt() { LOOP=$(sudo losetup -f --show -o $(( $2 * 512 )) "$1") && sudo mount.exfat-fuse "$LOOP" "$W/mnt" -o allow_other >/dev/null 2>&1; }
umnt() { sudo umount "$W/mnt"; sudo losetup -d "$LOOP"; LOOP=""; }
fsck_ok() { # image offset -> 0 = clean
	local L; L=$(sudo losetup -f --show -o $(( $2 * 512 )) "$1")
	sudo fsck.exfat -n "$L" > "$W/fsck.txt" 2>&1; local r=$?
	sudo losetup -d "$L"
	return $r
}
same() { # image offset folder name bytes seed -> 0 = the file reads back as written
	"$T" --gen "$W/expect.bin" "$5" "$6"
	mnt "$1" "$2" || return 9
	cmp -s "$W/mnt/$3/$4" "$W/expect.bin"; local r=$?
	umnt
	return $r
}
mkimg() { # name MB -> whole-disk exFAT, 32 KB clusters, with PS2ISO and two files
	truncate -s "${2}M" "$W/$1"
	mkfs.exfat -c 32K -L PRISMTEST "$W/$1" > /dev/null
	mnt "$W/$1" 0
	sudo mkdir "$W/mnt/PS2ISO" "$W/mnt/PS2PRISM"
	sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/OLD GAME.iso" bs=1M count=3 status=none
	sudo dd if=/dev/urandom of="$W/mnt/readme.txt" bs=1k count=5 status=none
	sha256sum "$W/mnt/PS2ISO/OLD GAME.iso" "$W/mnt/readme.txt" | sed "s#$W/mnt/##" > "$W/$1.old.sha"
	umnt
}
old_same() { # image offset -> the files that were there are untouched
	mnt "$1" "$2" || return 9
	(cd "$W/mnt" && sha256sum -c --quiet "$W/$(basename "$1").old.sha") > /dev/null 2>&1; local r=$?
	umnt
	return $r
}

# ---- calibration: fsck.exfat notices the two defects the writer could make ----
mkimg cal.img 256
"$T" "$W/cal.img" PS2ISO "CAL.iso" 1048576 1 > /dev/null
fsck_ok "$W/cal.img" 0 && ok "calibration: the written image is clean" || bad "calibration: the written image is not clean: $(tail -3 $W/fsck.txt)"
cp "$W/cal.img" "$W/cal_sum.img"
python3 - "$W/cal_sum.img" <<'EOF'
import sys,struct
f=open(sys.argv[1],'r+b'); d=f.read()
i=d.find('CAL.iso'.encode('utf-16-le'))   # the name entry; the File entry is 2 entries before
fe=i-2-64
assert d[fe]==0x85, hex(d[fe])
f.seek(fe+2); s=struct.unpack('<H',d[fe+2:fe+4])[0]; f.write(struct.pack('<H',s^0x5555)); f.close()
EOF
fsck_ok "$W/cal_sum.img" 0 && bad "calibration: fsck.exfat missed a bad SetChecksum" || ok "calibration: fsck.exfat catches a bad SetChecksum"
cp "$W/cal.img" "$W/cal_bmp.img"
python3 - "$W/cal_bmp.img" <<'EOF'
import sys,struct
f=open(sys.argv[1],'r+b'); d=f.read(4096); f.seek(0); img=f.read()
heap=struct.unpack('<I',d[88:92])[0]; spc=1<<d[109]; root=struct.unpack('<I',d[96:100])[0]
# the bitmap entry (0x81) in the root folder's first cluster
r=(heap+(root-2)*spc)*512
for k in range(0,spc*512,32):
    if img[r+k]==0x81: bc=struct.unpack('<I',img[r+k+20:r+k+24])[0]; break
i=img.find('CAL.iso'.encode('utf-16-le')); st=i-2-32
fc=struct.unpack('<I',img[st+20:st+24])[0]
bit=fc-2; off=(heap+(bc-2)*spc)*512+bit//8
f.seek(off); b=img[off]; f.write(bytes([b & ~(1<<(bit%8))])); f.close()
EOF
fsck_ok "$W/cal_bmp.img" 0 && bad "calibration: fsck.exfat missed a cleared bitmap bit" || ok "calibration: fsck.exfat catches a cleared bitmap bit"

# ---- 1. a 70 MB file into PS2ISO ----
mkimg t1.img 256
if "$T" "$W/t1.img" PS2ISO "SLUS_209.46.iso" 73400320 7 > "$W/t1.out"; then
	fsck_ok "$W/t1.img" 0 && ok "70 MB file: volume clean" || bad "70 MB file: fsck: $(tail -3 $W/fsck.txt)"
	same "$W/t1.img" 0 PS2ISO "SLUS_209.46.iso" 73400320 7 && ok "70 MB file: reads back exactly" || bad "70 MB file: bytes differ"
	old_same "$W/t1.img" 0 && ok "70 MB file: the files already there are untouched" || bad "70 MB file: other files changed"
else bad "70 MB file: $(cat $W/t1.out)"; fi

# ---- 2. a long name (several name entries), a size that is not whole sectors ----
N="Metal Gear Solid 3 - Snake Eater (USA) [SLUS-21359] (v2.00).iso"
if "$T" "$W/t1.img" PS2ISO "$N" 5000123 11 > "$W/t2.out"; then
	fsck_ok "$W/t1.img" 0 && ok "long name, odd size: volume clean" || bad "long name: fsck: $(tail -3 $W/fsck.txt)"
	same "$W/t1.img" 0 PS2ISO "$N" 5000123 11 && ok "long name, odd size: reads back exactly" || bad "long name: bytes differ"
else bad "long name: $(cat $W/t2.out)"; fi

# ---- 3. refusals: the same name again (any case), a folder that is not there, too big ----
"$T" "$W/t1.img" PS2ISO "slus_209.46.ISO" 1000 3 > "$W/t3.out" && bad "same name accepted" || ok "same name refused: $(head -1 $W/t3.out)"
"$T" "$W/t1.img" PSXISO "X.iso" 1000 3 > "$W/t3.out" && bad "missing folder accepted" || ok "missing folder refused: $(head -1 $W/t3.out)"
"$T" "$W/t1.img" PS2ISO "BIG.iso" 900000000 3 > "$W/t3.out" && bad "too big accepted" || ok "too big refused: $(head -1 $W/t3.out)"
fsck_ok "$W/t1.img" 0 && ok "after the refusals: volume still clean" || bad "after refusals: fsck: $(tail -3 $W/fsck.txt)"

# ---- 4. many files in a row (entry sets crossing sector and cluster bounds) ----
mkimg t4.img 128
k=0; okc=1
for i in $(seq 1 120); do
	nm=$(printf 'GAME %03d %.*s.iso' $i $((i % 40)) "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP")
	if ! "$T" "$W/t4.img" PS2ISO "$nm" $((4096 + i * 517)) $i > "$W/t4.out"; then okc=0; echo "    file $i: $(cat $W/t4.out)"; break; fi
	k=$i
done
fsck_ok "$W/t4.img" 0 && ok "$k files in one folder: volume clean" || bad "$k files: fsck: $(tail -3 $W/fsck.txt)"
same "$W/t4.img" 0 PS2ISO "$(printf 'GAME %03d %.*s.iso' 77 37 abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP)" $((4096 + 77 * 517)) 77 && ok "$k files: file 77 reads back" || bad "$k files: file 77 differs"
[ $okc = 1 ] && ok "120 files added" || bad "stopped at file $((k + 1))"

# ---- 5. a fragmented volume: the writer takes a gap that fits, refuses what fits nowhere ----
mkimg t5.img 128
mnt "$W/t5.img" 0
for i in $(seq 1 14); do sudo dd if=/dev/zero of="$W/mnt/PS2PRISM/f$i" bs=1M count=7 status=none 2>/dev/null || break; done
for i in 2 4 6 8 10 12; do sudo rm -f "$W/mnt/PS2PRISM/f$i"; done
sha256sum "$W/mnt/PS2ISO/OLD GAME.iso" "$W/mnt/readme.txt" | sed "s#$W/mnt/##" > "$W/t5.img.old.sha"
umnt
"$T" "$W/t5.img" PS2ISO "FITS.iso" 6291456 5 > "$W/t5.out" && ok "fragmented: 6 MB into a 7 MB gap" || bad "fragmented 6 MB: $(cat $W/t5.out)"
"$T" "$W/t5.img" PS2ISO "NOFIT.iso" 40000000 5 > "$W/t5.out" && bad "fragmented: 40 MB accepted" || ok "fragmented: 40 MB refused: $(head -1 $W/t5.out)"
fsck_ok "$W/t5.img" 0 && ok "fragmented: volume clean" || bad "fragmented: fsck: $(tail -3 $W/fsck.txt)"
same "$W/t5.img" 0 PS2ISO "FITS.iso" 6291456 5 && ok "fragmented: reads back" || bad "fragmented: bytes differ"

# ---- 6. a card like Bananafuzzbuddy's: MBR, FAT32 first, exFAT second; nothing outside the exFAT part moves ----
truncate -s 384M "$W/t6.img"
printf 'label: dos\nstart=2048, size=131072, type=c\nstart=133120, type=7\n' | sfdisk -q "$W/t6.img"
L=$(sudo losetup -f --show -o $((2048 * 512)) --sizelimit $((131072 * 512)) "$W/t6.img"); sudo mkfs.vfat -F 32 "$L" > /dev/null; sudo losetup -d "$L"
L=$(sudo losetup -f --show -o $((133120 * 512)) "$W/t6.img"); sudo mkfs.exfat -c 32K "$L" > /dev/null; sudo losetup -d "$L"
mnt "$W/t6.img" 133120; sudo mkdir "$W/mnt/PS2ISO"; umnt
head -c $((133120 * 512)) "$W/t6.img" | sha256sum > "$W/t6.before"
if "$T" "$W/t6.img" PS2ISO "CARD TEST.iso" 20971520 9 > "$W/t6.out"; then
	head -c $((133120 * 512)) "$W/t6.img" | sha256sum | cmp -s - "$W/t6.before" && ok "card layout: MBR and FAT32 part byte-identical" || bad "card layout: the area before the exFAT part changed"
	fsck_ok "$W/t6.img" 133120 && ok "card layout: exFAT part clean" || bad "card layout: fsck: $(tail -3 $W/fsck.txt)"
	same "$W/t6.img" 133120 PS2ISO "CARD TEST.iso" 20971520 9 && ok "card layout: reads back" || bad "card layout: bytes differ"
else bad "card layout: $(cat $W/t6.out)"; fi

# ---- 7. a volume marked dirty is refused ----
mkimg t7.img 64
python3 - "$W/t7.img" <<'EOF'
import sys
f=open(sys.argv[1],'r+b'); f.seek(106); b=f.read(2); f.seek(106); f.write(bytes([b[0]|2,b[1]])); f.close()
EOF
"$T" "$W/t7.img" PS2ISO "D.iso" 1000 1 > "$W/t7.out" && bad "dirty volume accepted" || ok "dirty volume refused: $(head -1 $W/t7.out)"

# ---- 8. Prism's own card reader (lr_fs.c, the PS2 side's) lists and reads what the writer made ----
TREE="$HERE/../../tree/ps2emu_stage2/prism"
gcc -O2 -w -DPRISM_LR=5 -DPS2GXEMU -I"$SRC" -I"$SRC/stub" -I"$TREE" "$HERE/fsport_host.c" "$HERE/fsprobe.c" "$TREE/lr_fs.c" -o "$W/fsprobe" 2>"$W/fsprobe.err" || echo "    fsprobe build: $(head -3 $W/fsprobe.err)"
if [ -x "$W/fsprobe" ]; then
	"$W/fsprobe" "$W/t6.img" list > "$W/t8.list" 2>/dev/null
	grep -q "CARD TEST.iso" "$W/t8.list" && ok "lr_fs lists the written game: $(grep 'CARD TEST' $W/t8.list | tr '	' ' ')" || bad "lr_fs does not list it: $(cat $W/t8.list | head -3)"
	"$T" --gen "$W/expect.bin" 20971520 9
	"$W/fsprobe" "$W/t6.img" cat "/PS2ISO/CARD TEST.iso" 2>/dev/null | cmp -s - "$W/expect.bin" && ok "lr_fs reads the written game exactly" || bad "lr_fs reads different bytes"
fi

echo "---- $pass passed, $fail failed"
[ $fail = 0 ]
