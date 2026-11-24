"""Facts about real .VM2 cards that the app's writer depends on, measured on the cards themselves
(2026-11-22). Read-only.

    python vmc_inspect.py <card.VM2> [...]

1. ECC: for every page, the 16 spare bytes against the ECC computed by the algorithm the app
   uses (3 bytes per 128-byte chunk, mymc's ps2mc_ecc), and what the last 4 spare bytes hold.
2. The directory entries of every save: the "." and ".." entries and the root entry, raw fields,
   so the writer copies what real cards hold rather than what a document says.
3. FAT values: how free entries look (never-used vs freed).
"""
import collections
import struct
import sys


def parity(b):
    b ^= b >> 4
    b ^= b >> 2
    b ^= b >> 1
    return b & 1


CPMASK = [(parity(a & 0x55) << 0) | (parity(a & 0x33) << 1) | (parity(a & 0x0F) << 2) |
          (parity(a & 0xAA) << 4) | (parity(a & 0xCC) << 5) | (parity(a & 0xF0) << 6) for a in range(256)]


def ecc128(chunk):
    cp, lp0, lp1 = 0x77, 0x7F, 0x7F
    for i, a in enumerate(chunk):
        cp ^= CPMASK[a]
        if parity(a):
            lp0 ^= ~i & 0x7F
            lp1 ^= i
    return bytes([cp, lp0 & 0x7F, lp1 & 0x7F])


def main(paths):
    for path in paths:
        raw = open(path, "rb").read()
        n = len(raw) // 528
        ok = bad = 0
        tails = collections.Counter()
        blank = 0
        first_bad = []
        for p in range(n):
            d = raw[p * 528:p * 528 + 512]
            sp = raw[p * 528 + 512:p * 528 + 528]
            want = b"".join(ecc128(d[i:i + 128]) for i in range(0, 512, 128))
            tails[sp[12:]] += 1
            if sp[:12] == want:
                ok += 1
            elif d == b"\xff" * 512 and sp == b"\xff" * 16:
                blank += 1
            else:
                bad += 1
                if len(first_bad) < 4:
                    first_bad.append((p, sp.hex(), want.hex()))
        print("%s: %d pages, ECC match %d, erased (all FF) %d, mismatch %d" % (path, n, ok, blank, bad))
        for fb in first_bad:
            print("   mismatch page %d spare %s want %s" % fb)
        print("   last 4 spare bytes:", dict((k.hex(), v) for k, v in tails.most_common(4)))

        data = b"".join(raw[i:i + 512] for i in range(0, len(raw), 528))
        ppc = struct.unpack_from("<H", data, 0x2A)[0]
        cl = 512 * ppc
        alloc_offset, alloc_end, rootdir = struct.unpack_from("<III", data, 0x34)
        ifc = struct.unpack_from("<32I", data, 0x50)
        per = cl // 4
        print("   superblock: page 512, ppc %d, alloc_offset %d, alloc_end %d, rootdir %d, ifc[0] %d, card size field %s" % (
            ppc, alloc_offset, alloc_end, rootdir, ifc[0], struct.unpack_from("<III", data, 0x2C)))

        def clus(k):
            return data[k * cl:(k + 1) * cl]

        def fat(k):
            fc = struct.unpack_from("<I", clus(ifc[k // (per * per)]), 4 * ((k // per) % per))[0]
            return struct.unpack_from("<I", clus(fc), 4 * (k % per))[0]

        vals = collections.Counter()
        for k in range(alloc_end):
            e = fat(k)
            vals["alloc" if e & 0x80000000 and e != 0xFFFFFFFF else "end" if e == 0xFFFFFFFF else "free %08x" % e if e in (0x7FFFFFFF, 0) else "free-link"] += 1
        print("   FAT:", dict(vals))
        fatcls = sorted(set(struct.unpack_from("<I", clus(ifc[0]), 4 * i)[0] for i in range((alloc_end + per - 1) // per)))
        print("   FAT clusters (absolute):", fatcls[:3], "...", fatcls[-1], "count", len(fatcls))

        def chain(first, length):
            out, k = [], first
            while len(out) * cl < length:
                out.append(clus(alloc_offset + k))
                e = fat(k)
                if e == 0xFFFFFFFF or not e & 0x80000000:
                    break
                k = e & 0x7FFFFFFF
            return b"".join(out)[:length]

        def ent(e):
            mode, length = struct.unpack_from("<HxxI", e, 0)
            c, de, attr = struct.unpack_from("<III", e, 0x10)[0], struct.unpack_from("<I", e, 0x14)[0], struct.unpack_from("<I", e, 0x20)[0]
            name = e[0x40:0x60].split(b"\0")[0].decode("latin-1")
            tail_zero = e[0x60:] == b"\0" * (512 - 0x60)
            return "mode %04x len %6d cl %10d dirent %3d attr %08x cr %s mo %s gap0x24 %s tail0 %s %r" % (
                mode, length, c, de, attr, e[8:16].hex(), e[0x18:0x20].hex(), e[0x24:0x40].hex() if any(e[0x24:0x40]) else "0", tail_zero, name)

        r0 = chain(rootdir, 512)
        rlen = struct.unpack_from("<I", r0, 4)[0]
        root = chain(rootdir, rlen * 512)
        print("   root: %d entries" % rlen)
        for i in range(rlen):
            e = root[i * 512:(i + 1) * 512]
            print("     [%d] %s" % (i, ent(e)))
            mode = struct.unpack_from("<H", e)[0]
            if mode & 0x8000 and mode & 0x20 and i >= 2 and len(sys.argv) > 1:
                c = struct.unpack_from("<I", e, 0x10)[0]
                dl = struct.unpack_from("<I", e, 4)[0]
                sub = chain(c, dl * 512)
                for j in range(min(dl, 3)):
                    print("          (%d) %s" % (j, ent(sub[j * 512:(j + 1) * 512])))


if __name__ == "__main__":
    main(sys.argv[1:])
