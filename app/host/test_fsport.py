"""Test the app's card reader (lr_fs.c through the app's block adapter, built for the PC as
host/fsprobe) against the Linux-made test cards. Ground truth is emu/cards/manifest.txt, written
by mk_cards.sh / mk_card_set.sh / mk_card_split.sh with the kernel's own drivers, never by us.
Run in WSL:  python3 test_fsport.py <fsprobe binary> <scratch dir>     exit 0 only if all pass."""
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EMU = os.path.join(HERE, "..", "..", "emu")
sys.path.insert(0, EMU)
from pack_simg import Card  # noqa: E402

probe, scratch = sys.argv[1], sys.argv[2]
os.makedirs(scratch, exist_ok=True)

man, splits = {}, {}
for line in open(os.path.join(EMU, "cards", "manifest.txt"), encoding="utf-8"):
    f = line.rstrip("\n").split("|")
    if f[0] in ("file", "split"):
        man.setdefault(f[1], {})[f[2]] = (int(f[3]), f[4])
        if f[0] == "split":
            splits.setdefault(f[1], set()).add(f[2])

fails = []


def check(ok, what):
    print("%-4s %s" % ("ok" if ok else "FAIL", what))
    if not ok:
        fails.append(what)


def run(img, *args):
    r = subprocess.run([probe, img] + list(args), capture_output=True)
    return r.returncode, r.stdout, r.stderr.decode("utf-8", "replace")


for card in ("fat32_mbr", "exfat_gpt", "fat32_super", "exfat_set", "two_part", "fat16"):
    simg = os.path.join(EMU, "cards", card + ".simg")
    raw = os.path.join(scratch, card + ".img")
    c = Card(simg)
    with open(raw, "wb") as fh:
        step = 2048
        for lba in range(0, c.blocks, step):
            fh.write(c.read(lba, min(step, c.blocks - lba)))
    rc, out, err = run(raw, "list")
    if card == "fat16":
        check(rc != 0 and "format the card FAT32 or exFAT" in err, "%s: refused" % card)
        os.remove(raw)
        continue
    listed = dict(l.split("\t") for l in out.decode("utf-8").splitlines())
    files = man.get(card, {})
    want = {p.split("/")[-1]: (s + 2047) // 2048 for p, (s, h) in files.items()
            if (p.startswith("/PS2ISO/") or p.startswith("/PS2PRISM/")) and (p.lower().endswith((".iso", ".bin")))}
    if card == "two_part":
        want = {k: v for k, v in want.items()}  # Gap/Odd are refused: they are not "file" lines
    check(rc == 0 and {k: int(v) for k, v in listed.items()} == want, "%s: list %r" % (card, sorted(listed)))
    for p, (size, sha) in sorted(files.items()):
        if p.endswith((".0", ".1", ".2")):
            continue
        rc, out, err = run(raw, "cat", p)
        check(rc == 0 and len(out) == size and hashlib.sha256(out).hexdigest() == sha, "%s: %s by path" % (card, p))
    if card == "two_part":
        # 1.00: lr_fs_open_exfat opens a path on the exFAT part only (its first part is FAT32, its second exFAT; the
        # manifest lists each part's files before that part's fsck line). A file on the FAT32 part alone must be refused,
        # one on the exFAT part alone read byte for byte.
        part, on = 1, {}
        for line in open(os.path.join(EMU, "cards", "manifest.txt"), encoding="utf-8"):
            f = line.rstrip("\n").split("|")
            if len(f) > 2 and f[1] == card:
                if f[0] == "fsck":
                    part += 1
                elif f[0] == "file":
                    on[f[2]] = part
        for p, pt in sorted(on.items()):
            if p.endswith((".0", ".1", ".2")):
                continue
            rc, out, err = run(raw, "catx", p)
            if pt == 1:
                check(rc != 0 and "not on an exFAT part" in err, "%s: %s (FAT32 part) refused by the exFAT-only open" % (card, p))
            else:
                check(rc == 0 and hashlib.sha256(out).hexdigest() == files[p][1], "%s: %s (exFAT part) by the exFAT-only open" % (card, p))
    for k, name in enumerate(listed):
        rc, out, err = run(raw, "catn", str(k))
        p = [q for q in files if q.split("/")[-1] == name]
        check(rc == 0 and p and hashlib.sha256(out).hexdigest() == files[p[0]][1], "%s: game %d %s from the list" % (card, k, name))
    os.remove(raw)

print("\n%d failed" % len(fails))
sys.exit(1 if fails else 0)
