"""Compare saves between PS2 card images through mymcplus (an outside reader, not the app's code).
Run with the Python that has mymcplus installed. Read-only: cards are opened "rb".

    python mc_compare.py same    <card A> <card B> <save folder> [...]   every file of the save,
                                  read through mymcplus (ECC checked), byte-identical on A and B
    python mc_compare.py absent  <card> <save folder> [...]
Prints one line per save; exit 1 if any fails.
"""
import hashlib
import sys

from mymcplus import ps2mc

DF_EXISTS, DF_FILE = 0x8000, 0x0010


def files(path, save):
    """{name: sha256} of the live files of a save, read through mymcplus with ECC checking"""
    with open(path, "rb") as f:
        mc = ps2mc.ps2mc(f)
        out = {}
        d = mc.dir_open("/" + save)
        try:
            for ent in d:
                mode, name = ent[0], ent[8]
                name = name.decode("latin-1") if isinstance(name, bytes) else name
                if not (mode & DF_EXISTS) or not (mode & DF_FILE):
                    continue
                g = mc.open("/%s/%s" % (save, name), "rb")
                try:
                    data = g.read()
                finally:
                    g.close()
                if len(data) != ent[2]:
                    raise SystemExit("%s/%s: read %d of %d bytes" % (save, name, len(data), ent[2]))
                out[name] = (len(data), hashlib.sha256(data).hexdigest())
        finally:
            d.close()
        mc.close()
        return out


def main(a):
    bad = 0
    if a[0] == "same":
        for s in a[3:]:
            try:
                x, y = files(a[1], s), files(a[2], s)
            except Exception as e:  # an ECC error is a failure, reported
                print("FAIL %s: %s: %s" % (s, type(e).__name__, e))
                bad += 1
                continue
            if x == y and x:
                print("%s: %d file(s), %d bytes, identical" % (s, len(x), sum(v[0] for v in x.values())))
            else:
                print("FAIL %s: A %s / B %s" % (s, sorted(x), sorted(y)))
                for k in sorted(set(x) | set(y)):
                    if x.get(k) != y.get(k):
                        print("     %s: %s vs %s" % (k, x.get(k), y.get(k)))
                bad += 1
    elif a[0] == "absent":
        with open(a[1], "rb") as f:
            mc = ps2mc.ps2mc(f)
            d = mc.dir_open("/")
            live = set()
            for ent in d:
                n = ent[8].decode("latin-1") if isinstance(ent[8], bytes) else ent[8]
                if ent[0] & DF_EXISTS:
                    live.add(n)
            d.close()
            mc.close()
        for s in a[2:]:
            if s in live:
                print("FAIL %s still on the card" % s)
                bad += 1
            else:
                print("%s: not on the card" % s)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main(sys.argv[1:])
