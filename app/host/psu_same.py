"""v0.67: two helpers for host/psu_test.sh (plain Python, no card code of the app).

    python psu_same.py <a.psu> <b.psu>
        the two files hold the same save: the same folder name, the same file names in the same order with
        the same lengths, modes, dates and bytes. (The cluster / place fields and the filling after a file
        may differ between tools and are not compared.) Exit 1 when they differ.

    python psu_same.py make-bad <good.psu> <out dir>
        writes files a reader must refuse: bad_folder_inside, bad_twice, bad_count, bad_name, bad_dotname,
        bad_notdir
"""
import struct
import sys

DF_EXISTS, DF_DIR, DF_FILE = 0x8000, 0x0020, 0x0010


def parse(path):
    d = open(path, "rb").read()
    mode, _, length = struct.unpack_from("<HHI", d, 0)
    name = d[0x40:0x60].split(b"\0")[0]
    out = {"name": name, "mode": mode, "created": d[8:16], "modified": d[0x18:0x20], "files": []}
    at = 512
    for i in range(length):
        e = d[at:at + 512]
        at += 512
        m, _, ln = struct.unpack_from("<HHI", e, 0)
        nm = e[0x40:0x60].split(b"\0")[0]
        if i < 2:
            continue
        data = d[at:at + ln]
        if len(data) != ln:
            raise SystemExit("%s: %s is cut short" % (path, nm))
        at += (ln + 1023) // 1024 * 1024
        out["files"].append((nm, m, ln, e[8:16], e[0x18:0x20], data))
    return out


def ent(mode, length, name, base):
    e = bytearray(base[:512])
    struct.pack_into("<HHI", e, 0, mode, 0, length)
    e[0x40:0x60] = name.ljust(32, b"\0")[:32]
    return bytes(e)


def make_bad(good, out):
    d = open(good, "rb").read()
    head, dot, dotdot = d[:512], d[512:1024], d[1024:1536]
    first = d[1536:2048]
    flen = struct.unpack_from("<I", first, 4)[0]
    fdata = d[2048:2048 + (flen + 1023) // 1024 * 1024]
    w = lambda n, b: open(out + "/" + n + ".psu", "wb").write(b)
    two = ent(0x8427, 3, b"BADSAVE", head)
    # a folder where a file should be
    w("bad_folder_inside", two + dot + dotdot + ent(0x8427, 0, b"SUBDIR", first))
    # the same file name twice
    four = ent(0x8427, 4, b"BADSAVE", head)
    w("bad_twice", four + dot + dotdot + first + fdata + first + fdata)
    # an entry count no save has
    w("bad_count", ent(0x8427, 5000, b"BADSAVE", head) + dot + dotdot + first + fdata)
    # names a card must not hold
    w("bad_name", ent(0x8427, 3, b"BAD/SAVE", head) + dot + dotdot + first + fdata)
    w("bad_dotname", ent(0x8427, 3, b"..", head) + dot + dotdot + first + fdata)
    # the first entry is a file, not a save's folder
    w("bad_notdir", ent(0x8497, 3, b"BADSAVE", head) + dot + dotdot + first + fdata)
    print("six bad files written")


def main(a):
    if a[0] == "make-bad":
        make_bad(a[1], a[2])
        return 0
    x, y = parse(a[0]), parse(a[1])
    bad = 0
    for k in ("name", "mode", "created", "modified"):
        if x[k] != y[k]:
            print("FAIL folder %s: %r vs %r" % (k, x[k], y[k]))
            bad += 1
    if [f[0] for f in x["files"]] != [f[0] for f in y["files"]]:
        print("FAIL file names or order: %r vs %r" % ([f[0] for f in x["files"]], [f[0] for f in y["files"]]))
        bad += 1
    else:
        for f, g in zip(x["files"], y["files"]):
            for i, what in ((1, "mode"), (2, "length"), (3, "created"), (4, "modified"), (5, "bytes")):
                if f[i] != g[i]:
                    print("FAIL %s %s differs" % (f[0], what))
                    bad += 1
    print("%s and its mymcplus twin: %d files, %d bytes, %s" % (x["name"].decode(), len(x["files"]), sum(f[2] for f in x["files"]),
                                                                 "the same save" if not bad else "DIFFERENT"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
