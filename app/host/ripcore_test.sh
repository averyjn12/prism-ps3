#!/bin/bash
# Tests for the disc-copy core (src/ripcore.c), run in WSL:  bash host/ripcore_test.sh
#   1. host/ripcore_test.c "unit": the track list, cue sheet, title ID and names, and both copy loops
#      against a pretend drive that fails the run on the two dangerous requests (a read past the
#      disc's end; a data sector asked for with the audio flag).
#   2. A pretend CD (a real PS2 ISO as the data track + 3 music tracks) copied to .bin + .cue, then
#      judged by a tool Prism does not own: bchunk must split it by the cue sheet into the ISO Prism
#      started from and the three music tracks.
#   3. The same copy, and a DVD copy, written through the exFAT writer into a card-like image:
#      fsck.exfat must call the volume clean and exfat-fuse must read back the exact bytes; Prism's
#      own card reader (lr_fs.c) must list the files.
#   4. Planted bugs: each must make step 1 fail, or step 1's silence means nothing.
# The title ID it expects comes from Linux mounting the ISO, not from Prism's code.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
W=/tmp/ripcore
sudo umount "$W/mnt" 2>/dev/null
rm -rf "$W"; mkdir -p "$W/mnt" "$W/cd" "$W/mut"
pass=0; fail=0
ok()  { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }

build() { gcc -O2 -Wall -Wextra -I"$SRC" "$HERE/ripcore_test.c" "$1" "$SRC/exfat_w.c" -o "$2"; }
build "$SRC/ripcore.c" "$W/t" || exit 1
T="$W/t"

# the test disc: Tiny Tees (its own sector count, read from the file) with random sectors after it, 4000 sectors in all
cp "$HERE/../setup/tinytees.iso" "$W/big.iso"
TT=$(( $(stat -c %s "$W/big.iso") / 2048 ))
head -c $(( (4000 - TT) * 2048 )) /dev/urandom >> "$W/big.iso"
sudo mount -o loop,ro "$W/big.iso" "$W/mnt" || { echo "cannot mount the test ISO"; exit 1; }
CNF=$(find "$W/mnt" -maxdepth 1 -iname 'system.cnf*' | head -1)	# Linux shows the name in lower case
WANT=$(tr -d '\r' < "$CNF" | sed -n 's/^BOOT2 *= *cdrom0:\\\(.*\);1.*/\1/p')
sudo umount "$W/mnt"
echo "the test disc's title ID, read by Linux: $WANT"
[ -n "$WANT" ] || { echo "no title ID from the mounted ISO"; exit 1; }

# ---- 1. the scenarios ----
"$T" unit "$W/big.iso" "$WANT" > "$W/unit.txt"; rc=$?
cat "$W/unit.txt"
u_pass=$(grep -c '^PASS' "$W/unit.txt"); u_fail=$(grep -c '^FAIL' "$W/unit.txt")
pass=$((pass + u_pass)); fail=$((fail + u_fail))
[ $rc = 0 ] && [ "$u_pass" -ge 61 ] || bad "unit run: exit $rc with $u_pass passes (61 or more expected)"

# ---- 2. bchunk splits the copy by its cue sheet ----
if "$T" rip "$W/big.iso" "$W/cd" > "$W/rip.txt"; then
	(cd "$W/cd" && bchunk game.bin game.cue out > "$W/bchunk.txt" 2>&1)
	if [ -f "$W/cd/out01.iso" ]; then
		head -c $(( 4000 * 2048 )) "$W/cd/out01.iso" | cmp -s - "$W/big.iso" && ok "bchunk: track 1 of the copy is the ISO, byte for byte" || bad "bchunk: track 1 differs from the ISO"
		# the 150 pregap sectors the pretend drive would not give are blank
		[ "$(tail -c +$(( 4000 * 2048 + 1 )) "$W/cd/out01.iso" | tr -d '\0' | wc -c)" = 0 ] && ok "bchunk: after the game, track 1 holds only the blank pregap" || bad "bchunk: data after the ISO in track 1"
		n=0
		for k in 02 03 04; do
			cmp -s "$W/cd/out$k.cdr" "$W/cd/track$k.raw" && n=$((n+1))
		done
		[ $n = 3 ] && ok "bchunk: the 3 music tracks start and end where the cue sheet says" || bad "bchunk: $n of 3 music tracks match ($(ls $W/cd | tr '\n' ' '))"
	else bad "bchunk made no out01.iso: $(tail -3 $W/bchunk.txt)"; fi
else bad "the pretend CD copy failed: $(cat $W/rip.txt)"; fi
# v0.44: the same disc copied the fast way must be the same file
mkdir -p "$W/cdf"
if RC_FAST=1 "$T" rip "$W/big.iso" "$W/cdf" > "$W/ripf.txt"; then
	cmp -s "$W/cd/game.bin" "$W/cdf/game.bin" && cmp -s "$W/cd/game.cue" "$W/cdf/game.cue" && ok "the fast copy is the same .bin and .cue as the slow one ($(stat -c %s "$W/cdf/game.bin") bytes)" || bad "the fast copy differs from the slow one"
else bad "the fast pretend CD copy failed: $(cat $W/ripf.txt)"; fi

# ---- 3. through the exFAT writer ----
LOOP=""
mnt() { LOOP=$(sudo losetup -f --show -o $(( $2 * 512 )) "$1") && sudo mount.exfat-fuse "$LOOP" "$W/mnt" -o allow_other >/dev/null 2>&1; }
umnt() { sudo umount "$W/mnt"; sudo losetup -d "$LOOP"; LOOP=""; }
fsck_ok() { local L; L=$(sudo losetup -f --show -o $(( $2 * 512 )) "$1"); sudo fsck.exfat -n "$L" > "$W/fsck.txt" 2>&1; local r=$?; sudo losetup -d "$L"; return $r; }
truncate -s 384M "$W/card.img"
printf 'label: dos\nstart=2048, size=131072, type=c\nstart=133120, type=7\n' | sfdisk -q "$W/card.img"
L=$(sudo losetup -f --show -o $((2048 * 512)) --sizelimit $((131072 * 512)) "$W/card.img"); sudo mkfs.vfat -F 32 "$L" > /dev/null; sudo losetup -d "$L"
L=$(sudo losetup -f --show -o $((133120 * 512)) "$W/card.img"); sudo mkfs.exfat -c 32K "$L" > /dev/null; sudo losetup -d "$L"
mnt "$W/card.img" 133120; sudo mkdir "$W/mnt/PS2ISO"; umnt
head -c $((133120 * 512)) "$W/card.img" | sha256sum > "$W/card.before"
CDN="Tiny Tees ($WANT).bin"; DVDN="Tiny Tees ($WANT).iso"
RC_FAST=1 "$T" exfat "$W/card.img" PS2ISO "$CDN" "$W/big.iso" cd > "$W/x_cd.txt" && cp "$W/card.img.expect" "$W/cd.expect" && ok "CD copy (the fast way) into the exFAT part: written and named" || bad "CD copy into exFAT: $(cat $W/x_cd.txt)"
"$T" exfat "$W/card.img" PS2ISO "$DVDN" "$W/big.iso" dvd > "$W/x_dvd.txt" && cp "$W/card.img.expect" "$W/dvd.expect" && ok "DVD copy into the exFAT part: written and named" || bad "DVD copy into exFAT: $(cat $W/x_dvd.txt)"
fsck_ok "$W/card.img" 133120 && ok "exFAT part clean after both (fsck.exfat)" || bad "fsck: $(tail -3 $W/fsck.txt)"
head -c $((133120 * 512)) "$W/card.img" | sha256sum | cmp -s - "$W/card.before" && ok "MBR and FAT32 part byte-identical" || bad "the area before the exFAT part changed"
mnt "$W/card.img" 133120
cmp -s "$W/mnt/PS2ISO/$CDN" "$W/cd.expect" && ok "exfat-fuse reads the CD copy back exactly ($(stat -c %s "$W/cd.expect") bytes)" || bad "CD copy reads back different"
cmp -s "$W/mnt/PS2ISO/$DVDN" "$W/big.iso" && ok "exfat-fuse reads the DVD copy back: it is the ISO" || bad "DVD copy reads back different"
umnt
TREE="$HERE/../../tree/ps2emu_stage2/prism"
if gcc -O2 -w -DPRISM_LR=5 -DPS2GXEMU -I"$SRC" -I"$SRC/stub" -I"$TREE" "$HERE/fsport_host.c" "$HERE/fsprobe.c" "$TREE/lr_fs.c" -o "$W/fsprobe" 2>"$W/fsprobe.err"; then
	"$W/fsprobe" "$W/card.img" list > "$W/list.txt" 2>/dev/null
	grep -q "Tiny Tees ($WANT).bin" "$W/list.txt" && grep -q "Tiny Tees ($WANT).iso" "$W/list.txt" && ok "Prism's card reader lists both copies" || bad "lr_fs list: $(head -5 $W/list.txt)"
	"$W/fsprobe" "$W/card.img" cat "/PS2ISO/$CDN" 2>/dev/null | cmp -s - "$W/cd.expect" && ok "Prism's card reader reads the CD copy exactly" || bad "lr_fs reads the CD copy differently"
else bad "fsprobe build: $(head -3 $W/fsprobe.err)"; fi

# ---- 3b. a split game on the FAT32 part: Prism's card reader joins the parts ----
mkdir -p "$W/parts"
if "$T" parts "$W/big.iso" "$W/parts" "Split Game ($WANT).iso" > "$W/parts.txt"; then
	np=$(ls "$W/parts" | wc -l)
	cat "$W/parts/Split Game ($WANT).iso."? | cmp -s - "$W/big.iso" && [ "$np" = 4 ] && ok "split: $np parts, joined by cat = the ISO" || bad "split: $np parts do not join to the ISO"
	L=$(sudo losetup -f --show -o $((2048 * 512)) --sizelimit $((131072 * 512)) "$W/card.img")
	sudo mount -t vfat "$L" "$W/mnt" && sudo mkdir "$W/mnt/PS2PRISM" && sudo cp "$W/parts/"* "$W/mnt/PS2PRISM/"
	sudo umount "$W/mnt"; sudo losetup -d "$L"
	if [ -x "$W/fsprobe" ]; then
		"$W/fsprobe" "$W/card.img" list > "$W/list2.txt" 2>/dev/null
		grep -q "^Split Game ($WANT).iso	" "$W/list2.txt" && ok "split: Prism's card reader lists it as one game" || bad "split: lr_fs list: $(cat $W/list2.txt | head -5)"
		"$W/fsprobe" "$W/card.img" cat "/PS2PRISM/Split Game ($WANT).iso" 2>/dev/null | cmp -s - "$W/big.iso" && ok "split: Prism's card reader reads the parts as the ISO, byte for byte" || bad "split: lr_fs reads the parts differently"
	fi
else bad "split: $(cat $W/parts.txt)"; fi

# ---- 3c. the title list: every 97th line looked up through src/titles.c must come back as written ----
python3 "$HERE/../tools/mk_titles.py" "$HERE/../db" "$W/titles.txt" > "$W/titles.log" && ok "title list built: $(cat $W/titles.log)" || bad "title list: $(cat $W/titles.log)"
cat > "$W/tl.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "titles.h"
int main(int argc, char **argv)
{
	char ln[400], t[200];
	FILE *f = fopen(argv[1], "rb");
	int n = 0, bad = 0, asked = 0;

	titles_path = argv[1];
	while (fgets(ln, sizeof ln, f))
	{
		char *bar = strchr(ln, '|');

		ln[strcspn(ln, "\n")] = 0;
		if (!bar || n++ % 97)
			continue;
		*bar = 0;
		asked++;
		if (title_lookup(ln, t, sizeof t) || strcmp(t, bar + 1))
			bad++;
	}
	fclose(f);
	bad += title_lookup("SLUS_999.98", t, sizeof t) == 0;	/* not in the list */
	bad += title_lookup("AAAA_000.00", t, sizeof t) == 0;	/* before the first line */
	bad += title_lookup("ZZZZ_999.99", t, sizeof t) == 0;	/* after the last */
	printf("%d asked, %d wrong\n", asked, bad);
	return bad || asked < 200;
}
EOF
gcc -O2 -Wall -I"$SRC" "$W/tl.c" "$SRC/titles.c" -o "$W/tl" && "$W/tl" "$W/titles.txt" > "$W/tl.txt" && ok "title lookup: $(cat $W/tl.txt)" || bad "title lookup: $(cat $W/tl.txt 2>/dev/null)"
[ "$(grep -c '^SLUS_000.76|Loaded$' "$W/titles.txt")" = 1 ] && [ "$(grep -c '^SLUS_212.43|' "$W/titles.txt")" = 1 ] && [ "$(grep -c '^SLUS_214.47|Guitar Hero II$' "$W/titles.txt")" = 1 ] && ok "title list holds Bananafuzzbuddy's three test discs (SLUS_000.76 Loaded, SLUS_212.43, SLUS_214.47 Guitar Hero II)" || bad "title list misses a test disc"
[ ! -e "$HERE/../db/TitlesDB_PS1_English.txt" ] && grep -q "GPL-3.0" "$HERE/../db/SOURCES.txt" && ok "only the two lists with a stated licence are in db/" || bad "db/ holds a list without a stated licence"

# ---- 4. planted bugs: the scenarios must notice each ----
mutant() { # name, sed expression
	sed "$2" "$SRC/ripcore.c" > "$W/mut/ripcore.c"
	if cmp -s "$W/mut/ripcore.c" "$SRC/ripcore.c"; then bad "mutant $1: the change did not apply"; return; fi
	if ! build "$W/mut/ripcore.c" "$W/mut/t" 2> "$W/mut/err.txt"; then bad "mutant $1: does not build"; return; fi
	timeout 120 "$W/mut/t" unit "$W/big.iso" "$WANT" > "$W/mut/out.txt" 2>&1
	local rc=$? nf; nf=$(grep -c '^FAIL' "$W/mut/out.txt")
	# a kill is the scenarios' own FAIL verdict: a crash or a hang is a broken run, not a catch
	if [ $rc = 1 ] && [ "$nf" -gt 0 ] && tail -1 "$W/mut/out.txt" | grep -q '^---- '; then ok "planted bug caught: $1 ($nf scenario(s) failed)"
	elif [ $rc = 0 ]; then bad "planted bug NOT caught: $1"
	else bad "planted bug $1: the run broke (exit $rc) instead of failing a scenario"; fi
}
if [ "$u_fail" != 0 ] || [ $rc != 0 ]; then
	bad "the planted-bug round was not run: the scenarios must all pass on the real code first"
	echo "---- $pass passed, $fail failed"; exit 1
fi
mutant "music tracks read as data and data as music" 's/a = toc->t\[i\].audio;/a = !toc->t[i].audio;/'
mutant "gap sectors retried like any other" 's/tries = gap ? 1 : CD_TRIES/tries = CD_TRIES/'
mutant "a short last block written at full length" 's/io->write(io->ctx, buf, n \* RC_RAW)/io->write(io->ctx, buf, chunk * RC_RAW)/'
mutant "unreadable CD sectors not blanked" 's/memset(p, 0, RC_RAW);/;/'
mutant "unreadable DVD sectors not blanked" 's/memset(p, 0, 2048);/;/'
mutant "cue frames counted in 60ths" 's/(unsigned)(lba % 75)/(unsigned)(lba % 60)/'
mutant "a data track taken for music" 's/ct == 0 || ct == 1/ct == 4 || ct == 1/'
mutant "Mode 1 and Mode 2 offsets swapped" 's/raw\[15\] == 2 ? 24 : raw\[15\] == 1 ? 16/raw[15] == 2 ? 16 : raw[15] == 1 ? 24/'
mutant "one sector past the disc end" 's/total = toc->leadout, run = 0;/total = toc->leadout + 1, run = 0;/'
mutant "never gives up on a disc that does not read" 's/(run >= MAX_RUN \&\& !good)/0/g; s/st->bad > MAX_BAD || //g'
mutant "a second data track accepted" 's/if (!out->t\[i\].audio)/if (0)/'
# (two checks refuse a list without an end mark; with only one of them gone the other still does)
mutant "no end mark accepted" 's/if (!lead)/if (0)/; s/out->leadout <= out->t\[out->n - 1\].lba/0/'
mutant "DVD block failure not retried sector by sector" 's/ok = io->read_2048(io->ctx, lba + k, 1, p) == 0;/ok = 0;/'
mutant "the stop request ignored" 's/return RC_STOP;/;/'
mutant "the title ID cut one character short" 's/k < idn - 1 \&\& k < 11/k < idn - 1 \&\& k < 10/'
mutant "a dead gap blanks the gaps after it too" 's/return toc->t\[i\].lba;/return toc->leadout;/'
mutant "a dead gap is still asked for sector by sector" 's/if (++gap_run >= GAP_GIVE_UP)/if (0)/'
mutant "a gap given up after one bad sector" 's/#define GAP_GIVE_UP	8u/#define GAP_GIVE_UP	1u/'
mutant "parts one sector too long" 's/p->part_max - p->in_part : (uint64_t)-1/p->part_max + 2048 - p->in_part : (uint64_t)-1/'
mutant "every part written over the first" 's/p->part++;/;/'
mutant "a short copy called whole" 's/p->done == p->total ? 0 : RC_EWRITE/0/'
mutant "any number of parts accepted" 's/(total + part_max - 1) \/ part_max > RC_PARTS_MAX/0/'
mutant "fast read: sub-channel bytes left between the sectors" 's/const uint8_t \*q = buf + (size_t)k \* RC_RAWQ;/const uint8_t *q = buf + (size_t)k * RC_RAW;/'
mutant "fast read: a wrong data sector kept" 's/have\[k\] = (uint8_t)(rc_audio_at(toc, s) || rc_data_ok(q, s));/have[k] = 1;/'
mutant "fast read: a failed command taken for a good one" 's/if (io->read_cd_n(io->ctx, lba, m, buf) == 0)/if (io->read_cd_n(io->ctx, lba, m, buf) <= 0)/'
mutant "fast read: music taken the fast way when it may not be" 's/(io->fast_audio || !rc_audio_at(toc, lba + m))/(1)/'
mutant "fast read: never given up" 's/if (fail_run >= FAST_FAIL_RUN || st->fast_wrong > FAST_WRONG_MAX)/if (0)/'
mutant "fast read: the address a sector carries not checked" 's/return rc_user_off(raw) >= 0 \&\& raw\[12\] == .*/return rc_user_off(raw) >= 0;/'
mutant "fast read: its sectors read again one by one" 's/if (have\[k\])$/if (0 \&\& have[k])/'
mutant "fast read: sectors moved one place down" 's/memmove(buf + (size_t)k \* RC_RAW, q, RC_RAW);/memmove(buf + (size_t)(k - 1) * RC_RAW, q, RC_RAW);/'
mutant "names keep a slash" 's/strchr("\\\\\/:\*?\\"<>|%+", c)/strchr(":*?<>|%+", c)/'

echo "---- $pass passed, $fail failed"
[ $fail = 0 ]
