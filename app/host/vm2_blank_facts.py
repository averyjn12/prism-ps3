"""What an EMPTY part of a real PS3 .VM2 card looks like, measured on the cards themselves (2026-11-25),
for the app's "new card" (src/vmcnew.c). Read-only.

    python vm2_blank_facts.py <card.VM2> [...]

Prints the superblock field by field, the indirect FAT / FAT layout, the root directory's entries, and how
never-written pages look (data bytes and the 16 spare bytes).
"""
import collections
import struct
import sys

PAGE, SPARE = 512, 16
RAW = PAGE + SPARE


def page(d, n):
    o = n * RAW
    return d[o:o + PAGE], d[o + PAGE:o + RAW]


def main(path):
    d = open(path, "rb").read()
    print("==", path, len(d), "bytes,", len(d) // RAW, "pages")
    sb, sp = page(d, 0)
    print(" magic   ", sb[0:28], sb[28:40])
    (page_len, ppc, ppb, x1) = struct.unpack("<HHHH", sb[40:48])
    (cpc, alloc_off, alloc_end, rootdir, bb1, bb2) = struct.unpack("<IIIIII", sb[48:72])
    print(" page_len %d pages/cluster %d pages/block %d unk %04x" % (page_len, ppc, ppb, x1))
    print(" clusters %d alloc_offset %d alloc_end %d rootdir %d backup1 %d backup2 %d" % (cpc, alloc_off, alloc_end, rootdir, bb1, bb2))
    print(" 72..80  ", sb[72:80].hex())
    ifc = struct.unpack("<32I", sb[80:208])
    bad = struct.unpack("<32I", sb[208:336])
    print(" ifc     ", [x for x in ifc if x], "zeros:", sum(1 for x in ifc if x == 0))
    print(" badblock", collections.Counter(bad))
    print(" 336..344", sb[336:344].hex(), " rest of page:", collections.Counter(sb[344:]).most_common(3))
    print(" page 0 spare", sp.hex())
    cl = page_len * ppc
    # indirect FAT cluster(s)
    for c in [x for x in ifc if x]:
        raw = b"".join(page(d, c * ppc + k)[0] for k in range(ppc))
        ent = struct.unpack("<%dI" % (cl // 4), raw)
        used = [x for x in ent if x != 0xFFFFFFFF]
        print(" indirect cluster %d: %d FAT clusters: %s .. %s, the rest %s" % (c, len(used), used[:3], used[-2:], collections.Counter(x for x in ent if x == 0xFFFFFFFF)))
        fat = []
        for fc in used:
            raw = b"".join(page(d, fc * ppc + k)[0] for k in range(ppc))
            fat += struct.unpack("<%dI" % (cl // 4), raw)
        print(" FAT entries", len(fat), "histogram of values (top 6):")
        for v, n in collections.Counter(fat).most_common(6):
            print("    %08x x %d" % (v, n))
        print(" FAT[0..8]", ["%08x" % x for x in fat[:8]])
        print(" FAT beyond alloc_end (first 4):", ["%08x" % x for x in fat[alloc_end:alloc_end + 4]], "last:", "%08x" % fat[-1])
    # root directory
    rc = alloc_off + rootdir
    raw = b"".join(page(d, rc * ppc + k)[0] for k in range(ppc))
    for i in range(2):
        e = raw[i * 512:(i + 1) * 512]
        mode, unk, length = struct.unpack("<HHI", e[0:8])
        print(" root entry %d: mode %04x unk %04x length %d created %s cluster %d dir_entry %d modified %s attr %08x" % (
            i, mode, unk, length, e[8:16].hex(), struct.unpack("<I", e[16:20])[0], struct.unpack("<I", e[20:24])[0], e[24:32].hex(),
            struct.unpack("<I", e[32:36])[0]))
        print("    32..64", e[32:64].hex(), "name", e[64:96].rstrip(b"\0"), "rest", collections.Counter(e[96:]).most_common(2))
    print(" root pages spare:", page(d, rc * ppc)[1].hex(), page(d, rc * ppc + 1)[1].hex())
    # how pages look by kind
    kinds = collections.Counter()
    first = {}
    for n in range(len(d) // RAW):
        dat, spr = page(d, n)
        k = ("FF" if dat == b"\xff" * PAGE else "00" if dat == bytes(PAGE) else "data", spr.hex())
        if k[0] != "data":
            kinds[k] += 1
            first.setdefault(k, n)
    for k, n in kinds.most_common(6):
        print(" pages all-%s with spare %s: %d (first at page %d)" % (k[0], k[1], n, first[k]))
    # the two backup blocks and the last pages
    for b in (bb1, bb2):
        dat, spr = page(d, b * ppb)
        print(" block %d first page: %s spare %s" % (b, "FF" if dat == b"\xff" * PAGE else "00" if dat == bytes(PAGE) else dat[:16].hex(), spr.hex()))
    print(" pages in all-FF data, by region: superblock block pages 1..15:", sum(1 for n in range(1, 16) if page(d, n)[0] == b"\xff" * PAGE))


for a in sys.argv[1:]:
    main(a)
