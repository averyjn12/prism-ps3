#!/bin/bash
# v0.73: tests for deleting a file from an exFAT volume (src/exfat_w.c exw_delete), run in WSL:
#   bash host/exd_test.sh
# As exw_test.sh: images made by mkfs.exfat, filled through exfat-fuse (an independent exFAT driver), then
# judged by tools Prism does not own: fsck.exfat -n must call the volume clean, exfat-fuse must no longer
# list the file, must read every OTHER file exactly as before, and must report the freed space (df).
# Calibration first: fsck.exfat must FAIL on the two defects a wrong delete could leave (a file whose
# clusters are marked free; a half-removed entry set), or its silence means nothing. What it is found NOT
# to notice (space left marked used) is measured another way, with df.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
W=/tmp/exd
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
# every file on the volume with its sha256, sorted: "what is there"
snap() { mnt "$1" "$2" || return 9; (cd "$W/mnt" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$3"; df --output=avail -B1 "$W/mnt" | tail -1 > "$3.avail"; umnt; }
# snapshot $1 minus the lines naming file $2 must equal snapshot $3
only_gone() { grep -vF "  ./$2" "$1" | cmp -s - "$3"; }
mkimg() { # name MB
	truncate -s "${2}M" "$W/$1"
	mkfs.exfat -c 32K -L PRISMTEST "$W/$1" > /dev/null
	mnt "$W/$1" 0
	sudo mkdir "$W/mnt/PS2ISO" "$W/mnt/PS2PRISM"
	sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/OLD GAME.iso" bs=1M count=3 status=none
	sudo dd if=/dev/urandom of="$W/mnt/readme.txt" bs=1k count=5 status=none
	umnt
}

# ---- calibration ----
mkimg cal.img 128
"$T" "$W/cal.img" PS2ISO "CAL.iso" 1048576 1 > /dev/null
fsck_ok "$W/cal.img" 0 && ok "calibration: the image is clean before" || bad "calibration: not clean before: $(tail -3 $W/fsck.txt)"
cp "$W/cal.img" "$W/cal_free.img"   # the file still named, its clusters marked free
python3 - "$W/cal_free.img" <<'EOF'
import sys,struct
f=open(sys.argv[1],'r+b'); img=f.read(); d=img[:512]
heap=struct.unpack('<I',d[88:92])[0]; spc=1<<d[109]; root=struct.unpack('<I',d[96:100])[0]
r=(heap+(root-2)*spc)*512
for k in range(0,spc*512,32):
    if img[r+k]==0x81: bc=struct.unpack('<I',img[r+k+20:r+k+24])[0]; break
i=img.find('CAL.iso'.encode('utf-16-le')); st=i-2-32
fc=struct.unpack('<I',img[st+20:st+24])[0]; n=1048576//(spc*512)
base=(heap+(bc-2)*spc)*512
for c in range(fc-2, fc-2+n):
    f.seek(base+c//8); b=f.read(1)[0]; f.seek(base+c//8); f.write(bytes([b & ~(1<<(c%8))]))
f.close()
EOF
fsck_ok "$W/cal_free.img" 0 && bad "calibration: fsck.exfat missed a file whose clusters are marked free" || ok "calibration: fsck.exfat catches a file whose clusters are marked free"
cp "$W/cal.img" "$W/cal_half.img"   # only the File entry removed: Stream and Name entries left in use
python3 - "$W/cal_half.img" <<'EOF'
import sys
f=open(sys.argv[1],'r+b'); img=f.read()
i=img.find('CAL.iso'.encode('utf-16-le')); fe=i-2-64
assert img[fe]==0x85
f.seek(fe); f.write(bytes([0x05])); f.close()
EOF
fsck_ok "$W/cal_half.img" 0 && HALF=0 || HALF=1
[ $HALF = 1 ] && ok "calibration: fsck.exfat catches a half-removed entry set" || echo "NOTE  calibration: fsck.exfat does NOT notice a half-removed entry set; the entry bytes are checked directly below"
cp "$W/cal.img" "$W/cal_leak.img"   # the whole set removed, the clusters left marked used
python3 - "$W/cal_leak.img" <<'EOF'
import sys
f=open(sys.argv[1],'r+b'); img=f.read()
i=img.find('CAL.iso'.encode('utf-16-le')); fe=i-2-64
for k in range(3):
    f.seek(fe+32*k); f.write(bytes([img[fe+32*k] & 0x7F]))
f.close()
EOF
fsck_ok "$W/cal_leak.img" 0 && LEAK=0 || LEAK=1
[ $LEAK = 1 ] && ok "calibration: fsck.exfat catches space left marked used" || echo "NOTE  calibration: fsck.exfat does NOT notice space left marked used; df measures it below"
snap "$W/cal.img" 0 "$W/cal.snap"; snap "$W/cal_leak.img" 0 "$W/cal_leak.snap"
[ "$(cat $W/cal.snap.avail)" = "$(cat $W/cal_leak.snap.avail)" ] && ok "calibration: df shows no space back when the clusters stay marked" || bad "calibration: df cannot see a leak either ($(cat $W/cal.snap.avail) vs $(cat $W/cal_leak.snap.avail))"

# in-use entries with no File entry before them, read straight from the image: must be none in PS2ISO's entries
stray() { python3 - "$1" "$2" <<'EOF'
import sys,struct
img=open(sys.argv[1],'rb').read(); off=int(sys.argv[2])*512; d=img[off:off+512]
heap=struct.unpack('<I',d[88:92])[0]; spc=1<<d[109]; root=struct.unpack('<I',d[96:100])[0]; fat=struct.unpack('<I',d[80:84])[0]
def chain(c, nofat, n):
    out=[]
    while len(out) < n:
        out.append(c)
        c = c+1 if nofat else struct.unpack('<I', img[off+fat*512+c*4: off+fat*512+c*4+4])[0]
        if c >= 0xFFFFFFF7: break
    return out
def ents(clus):
    b=b''.join(img[off+(heap+(c-2)*spc)*512: off+(heap+(c-2)*spc+spc)*512] for c in clus)
    return [b[i:i+32] for i in range(0,len(b),32)]
bad=0
def scan(es, depth):
    global bad
    left=0; i=0
    while i < len(es):
        e=es[i]
        if e[0]==0: break
        if e[0]==0x85:
            left=e[1]
            if es[i+1][0]==0xC0 and (struct.unpack('<H',e[4:6])[0] & 0x10) and depth==0:
                st=es[i+1]; n=(struct.unpack('<Q',st[24:32])[0]+spc*512-1)//(spc*512)
                scan(ents(chain(struct.unpack('<I',st[20:24])[0], st[1]&2, n)), 1)
        elif e[0] in (0xC0,0xC1):
            if left==0: bad+=1
            else: left-=1
        i+=1
scan(ents(chain(root,0,64)),0)
print(bad)
EOF
}
[ "$(stray "$W/cal.img" 0)" = 0 ] && [ "$(stray "$W/cal_half.img" 0)" != 0 ] && ok "calibration: the stray-entry reader sees a half-removed set and none on a clean image" || bad "calibration: the stray-entry reader ($(stray "$W/cal.img" 0), $(stray "$W/cal_half.img" 0))"

# one delete, judged every way
del_check() { # label image offset folder name
	local before="$W/$1.before" after="$W/$1.after"
	snap "$2" "$3" "$before"
	if ! "$T" --del "$2" "$4" "$5" > "$W/$1.out"; then bad "$1: $(cat $W/$1.out)"; return; fi
	snap "$2" "$3" "$after"
	fsck_ok "$2" "$3" && ok "$1: volume clean" || bad "$1: fsck: $(tail -3 $W/fsck.txt)"
	grep -qF "  ./$4/$5" "$before" || bad "$1: the file was not there before"
	only_gone "$before" "$4/$5" "$after" && ok "$1: that file is gone and every other file reads as before" || bad "$1: the other files changed, or the file is still there"
	local sz; sz=$(( $(cat "$after.avail") - $(cat "$before.avail") ))
	[ "$sz" -ge "$6" ] && [ "$sz" -lt $(( $6 + 65536 )) ] && ok "$1: $sz bytes came back (file $6)" || bad "$1: $sz bytes came back, the file was $6"
	[ "$(stray "$2" "$3")" = 0 ] && ok "$1: no entry left without its File entry" || bad "$1: stray entries left in the folder"
}

# ---- 1. a file the writer itself made (one run, no FAT chain) ----
mkimg t1.img 256
"$T" "$W/t1.img" PS2ISO "SLUS_209.46.iso" 73400320 7 > /dev/null
N="Metal Gear Solid 3 - Snake Eater (USA) [SLUS-21359] (v2.00).iso"
"$T" "$W/t1.img" PS2ISO "$N" 5000123 11 > /dev/null
del_check "writer-made 70 MB file" "$W/t1.img" 0 PS2ISO "SLUS_209.46.iso" 73400320
# ---- 2. a long name (five name entries), a size that is not whole clusters, named in another case ----
snap "$W/t1.img" 0 "$W/t2.before"
if "$T" --del "$W/t1.img" PS2ISO "metal gear solid 3 - snake eater (usa) [slus-21359] (V2.00).ISO" > "$W/t2.out"; then
	snap "$W/t1.img" 0 "$W/t2.after"
	only_gone "$W/t2.before" "PS2ISO/$N" "$W/t2.after" && ok "long name in another case: gone, the rest as before" || bad "long name: the rest changed"
	fsck_ok "$W/t1.img" 0 && ok "long name: volume clean" || bad "long name: fsck: $(tail -3 $W/fsck.txt)"
else bad "long name: $(cat $W/t2.out)"; fi
# ---- 3. the space is usable again: a file that fits only where the two were ----
FREE=$("$T" --free "$W/t1.img" PS2ISO "probe.bin")
"$T" "$W/t1.img" PS2ISO "AFTER.iso" $(( FREE - 1048576 )) 21 > "$W/t3.out" && ok "a file as big as all the free space goes in after the deletes" || bad "refill: $(cat $W/t3.out)"
fsck_ok "$W/t1.img" 0 && ok "refill: volume clean" || bad "refill: fsck: $(tail -3 $W/fsck.txt)"

# ---- 4. a file exfat-fuse made in pieces (a FAT chain), on a fragmented volume ----
mkimg t4.img 128
mnt "$W/t4.img" 0
for i in $(seq 1 14); do sudo dd if=/dev/urandom of="$W/mnt/PS2PRISM/f$i" bs=1M count=7 status=none 2>/dev/null || break; done
for i in 2 4 6 8 10 12; do sudo rm -f "$W/mnt/PS2PRISM/f$i"; done
sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/PIECES.iso" bs=1M count=30 status=none
umnt
python3 - "$W/t4.img" > "$W/t4.flag" <<'EOF'
import sys
img=open(sys.argv[1],'rb').read()
i=img.find('PIECES.iso'.encode('utf-16-le')); st=i-2-32
print("nofat" if img[st+1] & 2 else "chain")
EOF
[ "$(cat $W/t4.flag)" = chain ] && ok "the 30 MB test file is in pieces (a FAT chain)" || bad "the test file is not fragmented: this case tests nothing"
del_check "file in pieces" "$W/t4.img" 0 PS2ISO "PIECES.iso" 31457280

# ---- 5. many files: entry sets crossing sector and cluster bounds, deleted from the middle ----
mkimg t5.img 128
nm() { printf 'GAME %03d %.*s.iso' $1 $(($1 % 40)) "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP"; }
for i in $(seq 1 120); do "$T" "$W/t5.img" PS2ISO "$(nm $i)" $((4096 + i * 517)) $i > /dev/null || { bad "setup: file $i"; break; }; done
snap "$W/t5.img" 0 "$W/t5.before"; cp "$W/t5.before" "$W/t5.expect"
okc=1
for i in 1 5 6 7 16 17 33 64 77 78 79 100 119 120; do
	"$T" --del "$W/t5.img" PS2ISO "$(nm $i)" > "$W/t5.out" || { okc=0; echo "    $i: $(cat $W/t5.out)"; }
	grep -vF "  ./PS2ISO/$(nm $i)" "$W/t5.expect" > "$W/t5.tmp"; mv "$W/t5.tmp" "$W/t5.expect"
done
snap "$W/t5.img" 0 "$W/t5.after"
[ $okc = 1 ] && cmp -s "$W/t5.expect" "$W/t5.after" && ok "14 of 120 files deleted: exactly the other 106 (and the old files) are left, bytes as before" || bad "14 of 120: what is left differs"
fsck_ok "$W/t5.img" 0 && ok "14 of 120: volume clean" || bad "14 of 120: fsck: $(tail -3 $W/fsck.txt)"
[ "$(stray "$W/t5.img" 0)" = 0 ] && ok "14 of 120: no entry left without its File entry" || bad "14 of 120: stray entries"
"$T" "$W/t5.img" PS2ISO "$(nm 77)" 9999 3 > "$W/t5.out" && ok "a deleted name can be written again" || bad "rewrite: $(cat $W/t5.out)"
fsck_ok "$W/t5.img" 0 && ok "rewrite: volume clean" || bad "rewrite: fsck: $(tail -3 $W/fsck.txt)"

# ---- 6. refusals change nothing: no such name, a folder, another folder's file, a dirty volume ----
mkimg t6.img 64
mnt "$W/t6.img" 0; sudo mkdir "$W/mnt/PS2ISO/SUB"; sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/SUB/in.bin" bs=1k count=40 status=none; umnt
sha256sum < "$W/t6.img" > "$W/t6.sha"
"$T" --del "$W/t6.img" PS2ISO "NOT THERE.iso" > "$W/t6.out" && bad "a missing name was 'deleted'" || ok "missing name refused: $(head -1 $W/t6.out | cut -c1-60)"
"$T" --del "$W/t6.img" PS2ISO "SUB" > "$W/t6.out" && bad "a folder was deleted" || ok "folder refused: $(head -1 $W/t6.out | cut -c1-60)"
"$T" --del "$W/t6.img" PS2ISO "readme.txt" > "$W/t6.out" && bad "a file of the root was deleted through PS2ISO" || ok "the root's file is not PS2ISO's: refused"
"$T" --del "$W/t6.img" NOFOLDER "OLD GAME.iso" > "$W/t6.out" && bad "deleted from a folder that is not there" || ok "missing folder refused"
sha256sum < "$W/t6.img" | cmp -s - "$W/t6.sha" && ok "after the refusals the image is byte-identical" || bad "a refusal changed the image"
cp "$W/t6.img" "$W/t6d.img"
python3 - "$W/t6d.img" <<'EOF'
import sys
f=open(sys.argv[1],'r+b'); f.seek(106); b=f.read(2); f.seek(106); f.write(bytes([b[0]|2,b[1]])); f.close()
EOF
sha256sum < "$W/t6d.img" > "$W/t6d.sha"
"$T" --del "$W/t6d.img" PS2ISO "OLD GAME.iso" > "$W/t6.out" && bad "deleted on a dirty volume" || ok "dirty volume refused"
sha256sum < "$W/t6d.img" | cmp -s - "$W/t6d.sha" && ok "dirty volume: byte-identical" || bad "dirty volume: changed"
# a file whose cluster is already marked free (the volume disagrees with itself): refused, untouched
cp "$W/t6.img" "$W/t6b.img"
python3 - "$W/t6b.img" <<'EOF'
import sys,struct
f=open(sys.argv[1],'r+b'); img=f.read(); d=img[:512]
heap=struct.unpack('<I',d[88:92])[0]; spc=1<<d[109]; root=struct.unpack('<I',d[96:100])[0]
r=(heap+(root-2)*spc)*512
for k in range(0,spc*512,32):
    if img[r+k]==0x81: bc=struct.unpack('<I',img[r+k+20:r+k+24])[0]; break
i=img.find('OLD GAME.iso'.encode('utf-16-le')); st=i-2-32
fc=struct.unpack('<I',img[st+20:st+24])[0]+5
base=(heap+(bc-2)*spc)*512; c=fc-2
f.seek(base+c//8); b=f.read(1)[0]; f.seek(base+c//8); f.write(bytes([b & ~(1<<(c%8))])); f.close()
EOF
sha256sum < "$W/t6b.img" > "$W/t6b.sha"
"$T" --del "$W/t6b.img" PS2ISO "OLD GAME.iso" > "$W/t6.out" && bad "deleted a file whose clusters disagree with the bitmap" || ok "bitmap disagreement refused: $(head -1 $W/t6.out | cut -c1-70)"
sha256sum < "$W/t6b.img" | cmp -s - "$W/t6b.sha" && ok "bitmap disagreement: byte-identical" || bad "bitmap disagreement: changed"

# ---- 6b. a file whose bits are far into the allocation bitmap (4 KB clusters: 16384 clusters, 4 sectors of bitmap) ----
truncate -s 64M "$W/t6c.img"
mkfs.exfat -c 4K "$W/t6c.img" > /dev/null
mnt "$W/t6c.img" 0; sudo mkdir "$W/mnt/PS2ISO"; sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/FILLER.iso" bs=1M count=44 status=none; sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/FAR.iso" bs=1M count=6 status=none; umnt
python3 - "$W/t6c.img" > "$W/t6c.where" <<'EOF'
import sys,struct
img=open(sys.argv[1],'rb').read()
i=img.find('FAR.iso'.encode('utf-16-le')); st=i-2-32
print((struct.unpack('<I',img[st+20:st+24])[0]-2)//8//512)
EOF
[ "$(cat $W/t6c.where)" -ge 1 ] && ok "the far file's bits start in bitmap sector $(cat $W/t6c.where), not the first" || bad "the far file is in the first bitmap sector: this case tests nothing"
del_check "file far into the bitmap, 4 KB clusters" "$W/t6c.img" 0 PS2ISO "FAR.iso" 6291456

# ---- 6c. a file whose chain goes on past what its size says (the volume disagrees with itself): refused, untouched ----
truncate -s 128M "$W/t6e.img"
mkfs.exfat -c 32K "$W/t6e.img" > /dev/null
mnt "$W/t6e.img" 0; sudo mkdir "$W/mnt/PS2ISO" "$W/mnt/PS2PRISM"
for i in $(seq 1 14); do sudo dd if=/dev/urandom of="$W/mnt/PS2PRISM/f$i" bs=1M count=7 status=none 2>/dev/null || break; done
for i in 2 4 6 8 10 12; do sudo rm -f "$W/mnt/PS2PRISM/f$i"; done
sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/LONGCHAIN.iso" bs=1M count=30 status=none
umnt
python3 - "$W/t6e.img" > "$W/t6e.flag" <<'EOF'
import sys,struct
f=open(sys.argv[1],'r+b'); img=f.read()
i=img.find('LONGCHAIN.iso'.encode('utf-16-le')); st=i-2-32
print("nofat" if img[st+1] & 2 else "chain")
n=struct.unpack('<Q',img[st+24:st+32])[0]-3*32768       # the entry now says three clusters fewer than the chain has
f.seek(st+8); f.write(struct.pack('<Q',n)); f.seek(st+24); f.write(struct.pack('<Q',n)); f.close()
EOF
[ "$(cat $W/t6e.flag)" = chain ] && ok "the long-chain test file is a FAT chain" || bad "the long-chain test file is not a chain: this case tests nothing"
sha256sum < "$W/t6e.img" > "$W/t6e.sha"
"$T" --del "$W/t6e.img" PS2ISO "LONGCHAIN.iso" > "$W/t6.out" && bad "deleted a file whose chain is longer than its size" || ok "chain longer than the size refused: $(head -1 $W/t6.out | cut -c1-70)"
sha256sum < "$W/t6e.img" | cmp -s - "$W/t6e.sha" && ok "chain longer than the size: byte-identical" || bad "chain longer than the size: changed"

# ---- 7. a card like Bananafuzzbuddy's: MBR, FAT32 first, exFAT second; nothing outside the exFAT part moves ----
truncate -s 384M "$W/t7.img"
printf 'label: dos\nstart=2048, size=131072, type=c\nstart=133120, type=7\n' | sfdisk -q "$W/t7.img"
L=$(sudo losetup -f --show -o $((2048 * 512)) --sizelimit $((131072 * 512)) "$W/t7.img"); sudo mkfs.vfat -F 32 "$L" > /dev/null; sudo losetup -d "$L"
L=$(sudo losetup -f --show -o $((133120 * 512)) "$W/t7.img"); sudo mkfs.exfat -c 32K "$L" > /dev/null; sudo losetup -d "$L"
mnt "$W/t7.img" 133120; sudo mkdir "$W/mnt/PS2ISO"; sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/Crazy Taxi (USA).bin" bs=1M count=20 status=none; sudo dd if=/dev/urandom of="$W/mnt/PS2ISO/KEEP.iso" bs=1M count=4 status=none; umnt
head -c $((133120 * 512)) "$W/t7.img" | sha256sum > "$W/t7.before"
del_check "card layout" "$W/t7.img" 133120 PS2ISO "Crazy Taxi (USA).bin" 20971520
head -c $((133120 * 512)) "$W/t7.img" | sha256sum | cmp -s - "$W/t7.before" && ok "card layout: MBR and FAT32 part byte-identical" || bad "card layout: the area before the exFAT part changed"

# ---- 8. Prism's own card reader (lr_fs.c) no longer lists it, and still reads the one that stayed ----
TREE="$HERE/../../tree/ps2emu_stage2/prism"
gcc -O2 -w -DPRISM_LR=5 -DPS2GXEMU -I"$SRC" -I"$SRC/stub" -I"$TREE" "$HERE/fsport_host.c" "$HERE/fsprobe.c" "$TREE/lr_fs.c" -o "$W/fsprobe" 2>"$W/fsprobe.err" || echo "    fsprobe build: $(head -3 $W/fsprobe.err)"
if [ -x "$W/fsprobe" ]; then
	"$W/fsprobe" "$W/t7.img" list > "$W/t8.list" 2>/dev/null
	grep -q "Crazy Taxi" "$W/t8.list" && bad "lr_fs still lists the deleted game" || ok "lr_fs no longer lists the deleted game"
	grep -q "KEEP.iso" "$W/t8.list" && ok "lr_fs still lists the game that stayed" || bad "lr_fs lost the game that stayed: $(head -3 $W/t8.list)"
fi

# ---- 9. v0.73, the volume's free space for SETTINGS: what df says through exfat-fuse, within one cluster ----
space_check() { # label image offset
	local w a; w=$("$T" --space "$2" | cut -d' ' -f1)
	mnt "$2" "$3"; a=$(df --output=avail -B1 "$W/mnt" | tail -1); umnt
	local d=$(( w > a ? w - a : a - w ))
	[ -n "$w" ] && [ "$d" -le 131072 ] && ok "$1: free space $w, df says $a" || bad "$1: free space $w, df says $a"
}
space_check "whole-disk volume" "$W/t1.img" 0
space_check "card layout" "$W/t7.img" 133120
space_check "4 KB clusters" "$W/t6c.img" 0
truncate -s 64M "$W/t9.img"; mkfs.vfat -F 32 "$W/t9.img" > /dev/null
"$T" --space "$W/t9.img" > "$W/t9.out" && bad "a FAT32-only image has an exFAT volume" || ok "FAT32-only image: no exFAT volume ($(cut -d' ' -f3- $W/t9.out))"
sha256sum < "$W/t1.img" > "$W/t9.sha"; "$T" --space "$W/t1.img" > /dev/null; sha256sum < "$W/t1.img" | cmp -s - "$W/t9.sha" && ok "asking for the space changes nothing" || bad "asking for the space changed the image"

echo "---- $pass passed, $fail failed"
[ $fail = 0 ]
