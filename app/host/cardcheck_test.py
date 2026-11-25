"""Tests for src/cardcheck_core.c (Prism v0.65: does a card slot give back the card's real bytes?), run in WSL:

    python3 host/cardcheck_test.py            the checks, then the planted bugs

The cards are real: Linux's mkfs.vfat and mkfs.exfat make the volumes, Linux's own drivers (loop mounts, sudo) fill
them with files, and sfdisk writes the MBR and GPT partition tables. cardcheck_core.c owns none of that.
The readers (host/cardcheck_test.c): a sound one; "slip", which is Bananafuzzbuddy's SD-to-CF adaptor of 11-24 (the real bytes,
slipping half a byte at a time once a transfer is past its first sector); "slip0" (slipping from the start); "oddlba"
(the same junk in every odd sector however it is read, which only the card's own copies and checksums can show); and
"ioerr". Planted bugs: each must make a check FAIL; the unplanted code must pass every check first."""
import os
import random
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
W = "/tmp/prism_cardcheck"
MB = 1024 * 1024
OK, BAD, IOERR, THIN = 0, 1, 2, 3
quiet = False
passes = fails = 0


def check(ok, what):
    global passes, fails
    if not quiet:
        print(("PASS  " if ok else "FAIL  ") + what)
    if ok:
        passes += 1
    else:
        fails += 1


def sh(*a, **k):
    return subprocess.run(list(a), capture_output=True, text=True, **k)


def build(core):
    r = sh("gcc", "-O2", "-Wall", "-I" + SRC, os.path.join(HERE, "cardcheck_test.c"), core, "-o", W + "/t")
    return r.stderr if r.returncode else None


def run(img, reader, frm=0):
    r = sh(W + "/t", img, reader, str(frm))
    p = r.stdout.split(" ", 5)
    if r.returncode or len(p) < 4 or p[0] != "result":
        return None, -1, "the driver failed: " + r.stdout + r.stderr
    return int(p[1]), int(p[3]), (p[5].strip() if len(p) > 5 else "")


def make_fs(kind, path, mb, fill):
    with open(path, "wb") as f:
        f.truncate(mb * MB)
    r = sh("/usr/sbin/mkfs.vfat", "-F", "32", "-n", "PRISM", path) if kind == "fat" else sh("/usr/sbin/mkfs.exfat", "-L", "PRISMEX", path)
    if r.returncode:
        sys.exit("mkfs failed: " + r.stderr)
    if not fill:
        return
    mnt = W + "/mnt"
    os.makedirs(mnt, exist_ok=True)
    sh("sudo", "umount", mnt)
    r = sh("sudo", "mount", "-o", "loop,uid=%d,gid=%d" % (os.getuid(), os.getgid()), path, mnt)
    if r.returncode:
        sys.exit("mount failed: " + r.stderr)
    rnd = random.Random(kind)
    os.makedirs(mnt + "/PS2ISO", exist_ok=True)
    for i in range(30):		# enough names that the root directory runs past its first sector
        with open("%s/A game with a long file name number %02d.iso" % (mnt, i), "wb") as f:
            f.write(rnd.randbytes(rnd.randint(1, 600) * 1024))
    with open(mnt + "/PS2ISO/BIG GAME.iso", "wb") as f:
        f.write(rnd.randbytes(8 * MB))
    sh("sync")
    r = sh("sudo", "umount", mnt)
    if r.returncode:
        sys.exit("umount failed: " + r.stderr)


def assemble(path, label, parts):
    """a card image with a real partition table (sfdisk) and the volumes copied in at the places it chose"""
    total = 1 * MB + sum(os.path.getsize(p) for p, _ in parts) + 1 * MB
    with open(path, "wb") as f:
        f.truncate(total)
    script = "label: %s\n" % label
    for p, t in parts:
        script += "size=%d, type=%s\n" % (os.path.getsize(p) // 512, t)
    r = subprocess.run(["/usr/sbin/sfdisk", "--no-reread", "--no-tell-kernel", path], input=script, capture_output=True, text=True)
    if r.returncode:
        sys.exit("sfdisk failed: " + r.stderr)
    r = sh("/usr/sbin/sfdisk", "--json", path)
    import json
    starts = [e["start"] for e in json.loads(r.stdout)["partitiontable"]["partitions"]]
    with open(path, "r+b") as f:
        for (p, _), s in zip(parts, starts):
            f.seek(s * 512)
            with open(p, "rb") as g:
                shutil.copyfileobj(g, f)
    return starts


def poke(src, dst, edits):
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        for off, data in edits:
            f.seek(off)
            f.write(data)


def suite(imgs):
    full, fresh, gpt, floppy, flags, zeros, ex_at, gex = imgs
    r, j, why = run(full, "good")
    check(r == OK and j >= 8, "a sound slot, a card with files: OK, at least 8 tests judged (%s, %d, %s)" % (r, j, why))
    r2, j2, why = run(gpt, "good")
    check(r2 == OK and j2 == j, "the same volumes behind a GPT: OK, the same %d tests judged (%s, %d, %s)" % (j, r2, j2, why))
    r, j, why = run(fresh, "good")
    check(r == OK, "a sound slot, freshly formatted empty volumes: no false alarm (%s, %d, %s)" % (r, j, why))
    r, j, why = run(floppy, "good")
    check(r == OK and j >= 3, "a sound slot, FAT32 with no partition table: OK (%s, %d, %s)" % (r, j, why))
    r, j, why = run(flags, "good")
    check(r == OK, "dirty flags set in FAT 1 only and exFAT's live fields changed: still no false alarm (%s, %s)" % (r, why))
    r, j, why = run(full, "slip")
    check(r == BAD, "Bananafuzzbuddy's slipping adaptor, a card with files: BAD (%s, %s)" % (r, why))
    r, j, why = run(fresh, "slip")
    check(r == BAD, "Bananafuzzbuddy's slipping adaptor, an EMPTY card: still BAD, by the card's own copies (%s, %s)" % (r, why))
    r, j, why = run(full, "slip0")
    check(r == BAD, "slipping from the first byte (single-sector reads wrong too): BAD (%s, %s)" % (r, why))
    r, j, why = run(full, "slip16")
    check(r == BAD, "an adaptor sound for short reads that slips past 16 sectors (a game's long reads): BAD (%s, %s)" % (r, why))
    r, j, why = run(full, "oddlba")
    check(r == BAD, "the same junk however it is read: BAD, by checksums and copies (%s, %s)" % (r, why))
    r, j, why = run(floppy, "oddlba")
    check(r == BAD, "the same junk, FAT32 with no partition table: BAD (%s, %s)" % (r, why))
    r, j, why = run(full, "oddlba", ex_at)
    check(r == BAD, "junk only in the SECOND volume (exFAT): BAD, the second partition is tested too (%s, %s)" % (r, why))
    r, j, why = run(gpt, "slip", gex)
    check(r == BAD, "slips only in the GPT card's second volume: BAD (%s, %s)" % (r, why))
    r, j, why = run(full, "ioerr")
    check(r == IOERR, "reads that fail: a read error, not OK (%s, %s)" % (r, why))
    r, j, why = run(zeros, "good")
    check(r == THIN, "a blank card: says it cannot judge, does not say OK (%s, %s)" % (r, why))


MUTANTS = [
    ("exFAT checksum leaves out the wrong byte", "i == 106 || i == 107 || i == 112", "i == 105 || i == 107 || i == 112"),
    ("FAT32 not recognised", "le16(s + 17) == 0 && le16(s + 22) == 0", "le16(s + 17) != 0 && le16(s + 22) == 0"),
    ("FAT mirror compared from entry 0", "for (i = 8; i < m * 512; i++)", "for (i = 0; i < m * 512; i++)"),
    ("exFAT backup compared with no exceptions", "if (c->a[i] != c->b[i] && i != 106 && i != 107 && i != 112)", "if (c->a[i] != c->b[i])"),
    ("a failure never marks BAD", "\tif (bad)\n\t\tc->bad = 1;", "\tif (bad)\n\t\tc->bad = 0;"),
    ("only the first partition read", "for (i = 0; i < 4; i++)", "for (i = 0; i < 1; i++)"),
    ("GPT entries not read", "if (first && !(first >> 32) && first < total)", "if (0)"),
    ("a blank card called OK", "if (!c.judged)", "if (0)"),
    ("data-less tests counted", "if (nz >= 256)", "if (nz >= 0)"),
    ("read errors ignored", "\t\tnote(c, 0, \"a read of %u sector(s) at %u failed\", n, lba);\n\t\treturn -1;", "\t\treturn -1;"),
    ("singles and halves not compared", [("if (memcmp(c->a + k * 512, c->b + k * 512, 512))", "if (0)"), ("if (memcmp(c->a + k * 512, c->b + (h + k) * 512, 512))", "if (0)")]),
    ("test transfers cut to 8 sectors", "if (n > CC_SPAN)\n\t\tn = CC_SPAN;", "if (n > 8)\n\t\tn = 8;"),
]


def main():
    global quiet, passes, fails
    os.makedirs(W, exist_ok=True)
    core = os.path.join(SRC, "cardcheck_core.c")
    err = build(core)
    if err:
        sys.exit("build failed:\n" + err)
    fat, ex = W + "/fat.img", W + "/ex.img"
    fat0, ex0 = W + "/fat0.img", W + "/ex0.img"
    make_fs("fat", fat, 80, True)
    make_fs("exfat", ex, 64, True)
    make_fs("fat", fat0, 80, False)
    make_fs("exfat", ex0, 64, False)
    full, fresh, gpt = W + "/full.img", W + "/fresh.img", W + "/gpt.img"
    starts = assemble(full, "dos", [(fat, "c"), (ex, "7")])
    assemble(fresh, "dos", [(fat0, "c"), (ex0, "7")])
    assemble(gpt, "gpt", [(fat, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"), (ex, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")])
    r = sh("/usr/sbin/sfdisk", "--json", gpt)
    import json
    gex = json.loads(r.stdout)["partitiontable"]["partitions"][1]["start"]
    # FAT 1's entry 1 with its "dirty" bits flipped, FAT 2 left alone; exFAT's VolumeFlags and PercentInUse changed in the
    # main boot sector only (both outside its checksum)
    vbr = open(fat, "rb").read(512)
    fat1 = starts[0] * 512 + struct.unpack_from("<H", vbr, 14)[0] * 512
    e1 = struct.unpack_from("<I", open(full, "rb").read(fat1 + 8)[fat1:fat1 + 8], 4)[0] ^ 0x0C000000
    flags = W + "/flags.img"
    poke(full, flags, [(fat1 + 4, struct.pack("<I", e1)), (starts[1] * 512 + 106, b"\x02\x00"), (starts[1] * 512 + 112, b"\x2a")])
    zeros = W + "/zeros.img"
    with open(zeros, "wb") as f:
        f.truncate(16 * MB)
    imgs = (full, fresh, gpt, fat, flags, zeros, starts[1], gex)
    # the GPT card's second volume starts elsewhere: its own slip test uses its own start
    suite_imgs = imgs
    print("== the checks (exFAT volume at sector %d in the MBR card, %d in the GPT card)" % (starts[1], gex))
    suite(suite_imgs)
    base_fail = fails
    print("%d passed, %d failed" % (passes, fails))
    if fails or base_fail:
        sys.exit(1)
    print("== planted bugs")
    src = open(core).read()
    killed = broken = 0
    for m in MUTANTS:
        name, pairs = m[0], (m[1] if isinstance(m[1], list) else [(m[1], m[2])])
        if any(src.count(o) != 1 for o, _ in pairs):
            print("NOT PLANTED: %s" % name)
            broken += 1
            continue
        ms = src
        for o, n_ in pairs:
            ms = ms.replace(o, n_)
        mp = W + "/core_m.c"
        open(mp, "w").write(ms)
        err = build(mp)
        if err:
            print("BROKEN (does not build): %s" % name)
            broken += 1
            continue
        quiet, passes, fails = True, 0, 0
        suite(suite_imgs)
        quiet = False
        print(("killed  " if fails else "SURVIVED") + "  %s (%d check(s) failed)" % (name, fails))
        killed += fails > 0
    build(core)
    print("%d/%d planted bugs killed, %d not planted or broken" % (killed, len(MUTANTS), broken))
    sys.exit(0 if killed == len(MUTANTS) else 1)


main()
